// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>

//==============================================================================
// loopercat::SectionLoopSource — seamless looping of a frame section
// [start, end) over a wrapped PositionableAudioSource, designed around how
// JUCE's BufferingAudioSource actually drives its source: it seeks to ITS OWN
// linear position before every chunk and re-seeks whenever the reported
// position differs. So this wrapper presents a LINEAR position facade —
// monotonic, endless while a section is active — and maps it onto the section
// (start + (p - start) % length) only when touching the inner source. The
// buffering layer then streams the wrapped audio as one continuous take and
// the seam reaches the audio thread pre-buffered and gapless.
//
// Without a section the wrapper is a pure passthrough. Section bounds are
// lock-free atomics set from the message thread; a torn pair can only read
// as "no section" for one block and self-corrects.
//==============================================================================
namespace loopercat
{

class SectionLoopSource final : public juce::PositionableAudioSource
{
public:
    explicit SectionLoopSource(juce::PositionableAudioSource& inner) : inner_(inner) {}

    // While a section is active the linear stream never ends; this is the
    // total length the buffering layer sees then.
    static constexpr juce::int64 kEndless = std::numeric_limits<juce::int64>::max() / 4;

    // [startFrame, endFrame) in source frames; endFrame <= startFrame clears.
    void setSection(juce::int64 startFrame, juce::int64 endFrame)
    {
        linear_.store(inner_.getNextReadPosition(), std::memory_order_relaxed);
        start_.store(startFrame, std::memory_order_relaxed);
        end_.store(endFrame, std::memory_order_relaxed);
    }

    void clearSection() { setSection(0, 0); }

    bool hasSection() const
    {
        return end_.load(std::memory_order_relaxed) > start_.load(std::memory_order_relaxed);
    }

    // start + (p - start) % length for linear positions past the section
    // start; positions before it play straight into the section.
    static juce::int64 mapToSection(juce::int64 linear, juce::int64 start, juce::int64 end)
    {
        return linear < start ? linear : start + (linear - start) % (end - start);
    }

    void prepareToPlay(int samplesPerBlock, double sampleRate) override
    {
        inner_.prepareToPlay(samplesPerBlock, sampleRate);
    }

    void releaseResources() override { inner_.releaseResources(); }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        const juce::int64 start = start_.load(std::memory_order_relaxed);
        const juce::int64 end = end_.load(std::memory_order_relaxed);
        if (end <= start) {
            inner_.getNextAudioBlock(info);
            return;
        }
        int done = 0;
        juce::int64 linear = linear_.load(std::memory_order_relaxed);
        while (done < info.numSamples) {
            const juce::int64 mapped = mapToSection(linear, start, end);
            if (inner_.getNextReadPosition() != mapped)
                inner_.setNextReadPosition(mapped);
            const juce::int64 sliceEnd = mapped < start ? start : end;
            const int run = static_cast<int>(
                juce::jmin<juce::int64>(info.numSamples - done, sliceEnd - mapped));
            inner_.getNextAudioBlock({ info.buffer, info.startSample + done, run });
            done += run;
            linear += run;
        }
        linear_.store(linear, std::memory_order_relaxed);
    }

    void setNextReadPosition(juce::int64 newPosition) override
    {
        linear_.store(newPosition, std::memory_order_relaxed);
        if (!hasSection())
            inner_.setNextReadPosition(newPosition);
    }

    juce::int64 getNextReadPosition() const override
    {
        return hasSection() ? linear_.load(std::memory_order_relaxed)
                            : inner_.getNextReadPosition();
    }

    juce::int64 getTotalLength() const override
    {
        return hasSection() ? kEndless : inner_.getTotalLength();
    }

    bool isLooping() const override { return hasSection() || inner_.isLooping(); }

private:
    juce::PositionableAudioSource& inner_;
    std::atomic<juce::int64> start_ { 0 }, end_ { 0 };
    std::atomic<juce::int64> linear_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SectionLoopSource)
};

} // namespace loopercat
