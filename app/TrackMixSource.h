// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SectionLoopSource.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
// loopercat::TrackMixSource — a memory with more than one track, played the
// way its pedal plays it: every track's take at once, each at the level the
// memory gives it (TRACK<n>/PlyLvl, 100 = unity), summed into one stream.
// Solo keeps one track and mutes the rest; the master level stays on the
// transport, as for a one-track memory.
//
// Each track is its own chain — reader (looping on its own length) ->
// SectionLoopSource (the marker loop, in the mix's frames) -> buffering on
// the shared read-ahead thread — and the sum happens ABOVE the buffers, on
// the audio thread, from audio that is already in memory. That is what makes
// a solo or a level change immediate: below the buffers it would be heard
// only after the read-ahead drained, a second and a half later. The transport
// therefore sits on this source without a buffer of its own.
//
// Each track loops on ITS OWN length, as on the pedal, where a 4-bar track
// runs twice under an 8-bar one. The mix's clock is linear and folds at the
// longest track; with track lengths that are multiples of each other — the
// pedal's own arithmetic — that fold is also the shorter track's wrap, so
// every seam is gapless. A seek moves every track to the same time. This is
// a model for the preview: what the pedal does with lengths that are not
// multiples depends on settings this app does not read.
//
// The audio thread allocates nothing: the per-track scratch buffer is sized
// in prepareToPlay, and gains and the solo are atomics set from the message
// thread. What it does do is read each track's buffering source — the same
// lock the stock one-track chain takes inside the transport.
//==============================================================================
namespace loopercat
{

class TrackMixSource final : public juce::PositionableAudioSource
{
public:
    static constexpr int kMaxTracks = 2;

    struct Track {
        std::unique_ptr<juce::AudioFormatReaderSource> source; // owns its reader; null = no take
        float gain = 1.0f;                                     // PlyLvl / 100
    };

    // The tracks in track order, one entry per track the memory has: a track
    // without a take keeps its number and its silence, as on the pedal, so
    // that solo T2 means the pedal's track 2 whatever track 1 holds. At least
    // one take, at most kMaxTracks entries. Every take's read-ahead runs on
    // `readAhead`, `readAheadSamples` deep.
    TrackMixSource(std::vector<Track> tracks, juce::TimeSliceThread& readAhead,
                   int readAheadSamples)
    {
        jassert(!tracks.empty() && tracks.size() <= static_cast<std::size_t>(kMaxTracks));
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            Lane lane;
            gains_[i].store(tracks[i].gain, std::memory_order_relaxed);
            if (tracks[i].source != nullptr) {
                lane.reader = std::move(tracks[i].source);
                lane.reader->setLooping(true);
                lane.length = lane.reader->getTotalLength();
                lane.section = std::make_unique<SectionLoopSource>(*lane.reader);
                lane.buffer = std::make_unique<juce::BufferingAudioSource>(
                    lane.section.get(), readAhead, false, readAheadSamples, 2, true);
                longest_ = juce::jmax(longest_, lane.length);
            }
            lanes_.push_back(std::move(lane));
        }
        jassert(longest_ > 0); // a mix of nothing is not a mix
    }

    int trackCount() const { return static_cast<int>(lanes_.size()); }

    // The longest track's length in frames: the mix's length as the UI shows it.
    juce::int64 longestLength() const { return longest_; }

    // A track's level as a gain, 1.0 = the memory's own 100. MESSAGE THREAD.
    void setTrackGain(int track, float gain)
    {
        if (track >= 1 && track <= trackCount())
            gains_[static_cast<std::size_t>(track - 1)].store(gain, std::memory_order_relaxed);
    }

    // Solo one track (1-based); 0 plays them all. MESSAGE THREAD.
    void setSolo(int track) { solo_.store(track, std::memory_order_relaxed); }
    int solo() const { return solo_.load(std::memory_order_relaxed); }

    // Every track loops on its own length, or none does. MESSAGE THREAD.
    void setLooping(bool shouldLoop) override
    {
        looping_.store(shouldLoop, std::memory_order_relaxed);
        for (auto& lane : lanes_)
            if (lane.reader != nullptr)
                lane.reader->setLooping(shouldLoop);
    }

    // The marker loop, in the mix's frames, on every track. MESSAGE THREAD.
    void setSection(juce::int64 startFrame, juce::int64 endFrame)
    {
        for (auto& lane : lanes_)
            if (lane.section != nullptr)
                lane.section->setSection(startFrame, endFrame);
    }

    void clearSection() { setSection(0, 0); }

    bool hasSection() const
    {
        for (const auto& lane : lanes_)
            if (lane.section != nullptr)
                return lane.section->hasSection();
        return false;
    }

    // --- PositionableAudioSource ---

    void prepareToPlay(int samplesPerBlock, double sampleRate) override
    {
        for (auto& scratch : scratch_)
            scratch.setSize(2, samplesPerBlock, false, false, true);
        for (auto& lane : lanes_)
            if (lane.buffer != nullptr)
                lane.buffer->prepareToPlay(samplesPerBlock, sampleRate);
    }

    void releaseResources() override
    {
        for (auto& lane : lanes_)
            if (lane.buffer != nullptr)
                lane.buffer->releaseResources();
    }

    // AUDIO THREAD: read every track from its buffer, then add each at its
    // gain. A muted track is read too, so its clock keeps pace. Nothing is
    // allocated: the per-track scratch buffers were sized in prepareToPlay,
    // and a larger block is summed in pieces.
    //
    // One clock for every track, kept by hand. A buffering source that
    // misses its cache entirely answers with silence and does NOT advance
    // its position — the one-track chain simply pauses on that, and resumes
    // where it was once the read-ahead has caught up. The mix does the same
    // as a whole: if any track missed, the piece is silence and the clock
    // stands, and a track that did read has run ahead by that piece, so
    // before every read a track that is off the clock is put back on it
    // (through the same lock the read takes). No frame is ever skipped and
    // no track ever trails another.
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        const int solo = solo_.load(std::memory_order_relaxed);
        const bool looping = looping_.load(std::memory_order_relaxed);
        int done = 0;
        while (done < info.numSamples) {
            const juce::int64 clock = position_.load(std::memory_order_relaxed);
            const int run = juce::jmin(info.numSamples - done, scratch_[0].getNumSamples());
            bool missed = false;
            for (std::size_t i = 0; i < lanes_.size(); ++i) {
                Lane& lane = lanes_[i];
                if (lane.buffer == nullptr)
                    continue; // no take: silent, and no clock to keep
                if (lane.buffer->getNextReadPosition() != laneClock(lane, clock, looping))
                    lane.buffer->setNextReadPosition(clock);
                const juce::int64 before = lane.buffer->getNextReadPosition();
                lane.buffer->getNextAudioBlock({ &scratch_[i], 0, run });
                missed = missed || lane.buffer->getNextReadPosition() == before;
            }
            if (missed)
                return; // silence for the rest, the clock where it was
            for (std::size_t i = 0; i < lanes_.size(); ++i) {
                if (lanes_[i].buffer == nullptr)
                    continue;
                const int number = static_cast<int>(i) + 1;
                const float gain = solo != 0 && solo != number
                                     ? 0.0f
                                     : gains_[i].load(std::memory_order_relaxed);
                if (juce::exactlyEqual(gain, 0.0f))
                    continue;
                const int channels = juce::jmin(info.buffer->getNumChannels(), 2);
                for (int channel = 0; channel < channels; ++channel)
                    info.buffer->addFrom(channel, info.startSample + done, scratch_[i],
                                         juce::jmin(channel, scratch_[i].getNumChannels() - 1),
                                         0, run, gain);
            }
            position_.store(clock + run, std::memory_order_relaxed);
            done += run;
        }
    }

    // One clock for every track: each buffer folds the mix position into its
    // own loop (a looping source under a BufferingAudioSource is read modulo
    // its length), and the marker loop above the readers keeps the linear
    // facade every track shares.
    void setNextReadPosition(juce::int64 newPosition) override
    {
        position_.store(newPosition, std::memory_order_relaxed);
        for (auto& lane : lanes_)
            if (lane.buffer != nullptr)
                lane.buffer->setNextReadPosition(newPosition);
    }

    // The audible position: folded at the longest track while looping
    // without a section (as the one-track buffer folds it), linear otherwise
    // (a section's facade is endless; a one-shot runs dry at the end).
    juce::int64 getNextReadPosition() const override
    {
        const juce::int64 position = position_.load(std::memory_order_relaxed);
        if (looping_.load(std::memory_order_relaxed) && !hasSection() && longest_ > 0)
            return position % longest_;
        return position;
    }

    juce::int64 getTotalLength() const override
    {
        return hasSection() ? SectionLoopSource::kEndless : longest_;
    }

    bool isLooping() const override
    {
        return hasSection() || looping_.load(std::memory_order_relaxed);
    }

private:
    struct Lane {
        std::unique_ptr<juce::AudioFormatReaderSource> reader; // null = no take
        std::unique_ptr<SectionLoopSource> section;
        std::unique_ptr<juce::BufferingAudioSource> buffer;
        juce::int64 length = 0;
    };

    // Where a track's buffer reports itself when it is on the mix's clock: a
    // looping source under a BufferingAudioSource reads modulo its length
    // (and a section makes the facade linear, so nothing folds then).
    static juce::int64 laneClock(const Lane& lane, juce::int64 clock, bool looping)
    {
        if (lane.section->hasSection() || !looping || lane.length <= 0 || clock <= 0)
            return clock;
        return clock % lane.length;
    }

    std::vector<Lane> lanes_;
    std::array<std::atomic<float>, kMaxTracks> gains_ { { 1.0f, 1.0f } };
    std::atomic<int> solo_ { 0 };
    std::atomic<bool> looping_ { true };
    std::atomic<juce::int64> position_ { 0 };
    juce::int64 longest_ = 0;
    std::array<juce::AudioBuffer<float>, kMaxTracks> scratch_; // one per track, sized once

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackMixSource)
};

} // namespace loopercat
