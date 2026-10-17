// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Loudness.hpp>
#include <loopercat/Normalize.hpp>
#include <loopercat/Wav.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

//==============================================================================
// loopercat::readerloudness — the player's meter over JUCE's decoder, and the
// gate on what it may file.
//
// The player's read pass meters the loaded loop while it draws the waveform,
// through whatever JUCE's reader decodes. For the number on screen that is
// fine. For the history (#140) it is not enough: a reading filed under the
// bytes' hash claims to be what the core measures off those bytes, and JUCE
// reads more than the core does — it pads a truncated file to its header's
// length with silence, and it decodes pcm16 and mono files the pedal gate
// refuses. The two agree bit for bit on exactly one shape: the pedal's own
// stereo float32, whole. So:
//
//   ReaderMeter        the decoder-side meter, one place for how a decoded
//                      block is fed (mono into both channels, as the pedal
//                      plays it) — shared by the pass and by the suite that
//                      pins it to wav::measureLoudness bit for bit;
//   measurableInfo     the gate, from the file's first bytes and its size,
//                      no whole-file read: the core's own header walk with
//                      the real size as its bound, then the upload gate, then
//                      "has frames" — what wav::measureLoudness demands — and
//                      what the core found: its Info;
//   decodesAsCore      the other half of the proof. The gate says the core
//                      would measure the file; it does not say JUCE decoded
//                      the same frames. JUCE stops its chunk walk at the RIFF
//                      size field and sizes the data chunk with the frame
//                      size it knows when it meets it, so a RIFF size that
//                      ends after fmt, or inside a LIST before data, or a
//                      data chunk ahead of fmt, gives a reader of 0 frames
//                      over a file the core reads whole. A number from that
//                      reader is not the core's. The pass files only when the
//                      reader's frame count, rate and channels are the core's.
//
// A take refused by either still gets its number on screen; it just does
// not get filed.
//==============================================================================
namespace loopercat::readerloudness
{

// Frames per decoded block in the pass: ~0.7 s at the pedal's rate, a fine
// grain for stopping between blocks. The suite feeds the meter in the same
// blocks, so what it pins is what ships.
inline constexpr int kBlockFrames = 32768;

// The first bytes of a file the gate reads: room for RIFF, a fmt chunk of
// either body length the pedal writes (16 or 28 bytes), a DAW's LIST chunk,
// and the data chunk's header. A header that does not fit is a file this
// app did not write, and the gate refuses it rather than reading further.
inline constexpr int kHeaderProbeBytes = 4096;

class ReaderMeter
{
public:
    // Throws like loudness::Meter: a rate that does not cut into 100 ms
    // sub-blocks is not one the meter reads.
    explicit ReaderMeter(double sampleRate)
        : meter_(static_cast<int>(sampleRate)),
          interleaved_(2 * static_cast<std::size_t>(kBlockFrames))
    {
    }

    // One decoded block, `frames` long, from a reader with one or two
    // channels. Throws past the meter's capacity, like the meter.
    void feed(const juce::AudioBuffer<float>& block, int frames)
    {
        if (frames > kBlockFrames)
            throw Error("a decoded block is at most " + std::to_string(kBlockFrames)
                        + " frames, got " + std::to_string(frames));
        // Mono feeds both meter channels — the pedal plays it that way.
        const float* left = block.getReadPointer(0);
        const float* right = block.getReadPointer(block.getNumChannels() >= 2 ? 1 : 0);
        for (int i = 0; i < frames; ++i) {
            interleaved_[2 * static_cast<std::size_t>(i)] = left[i];
            interleaved_[2 * static_cast<std::size_t>(i) + 1] = right[i];
        }
        meter_.process(interleaved_.data(), static_cast<std::size_t>(frames));
    }

    wav::LoudnessReading reading() const
    {
        return { meter_.integratedLufs(), meter_.samplePeak(), meter_.truePeakDb(),
                 meter_.wildSamples() };
    }

private:
    loudness::Meter meter_;
    std::vector<float> interleaved_;
};

// The whole reader through ReaderMeter, in the pass's blocks — for the
// suite, and for any caller that wants only the number. Throws where the
// meter throws, and when the reader stops short of its own length.
inline wav::LoudnessReading measure(juce::AudioFormatReader& reader)
{
    ReaderMeter meter(reader.sampleRate);
    const int channels = static_cast<int>(reader.numChannels);
    juce::AudioBuffer<float> buffer(channels, kBlockFrames);
    for (juce::int64 position = 0; position < reader.lengthInSamples;) {
        const int n = static_cast<int>(
            std::min<juce::int64>(kBlockFrames, reader.lengthInSamples - position));
        if (!reader.read(&buffer, 0, n, position, true, true))
            throw Error("the decoder stopped " + std::to_string(reader.lengthInSamples - position)
                        + " frame(s) short of the length it declared");
        meter.feed(buffer, n);
        position += n;
    }
    return meter.reading();
}

// What the core makes of a file that begins with `head` and is `fileSize`
// bytes long, when wav::measureLoudness would accept it: a RIFF/WAVE whose
// every chunk lies inside the file (the core's truncation check, against the
// real size), 44.1 kHz stereo float32 (the upload gate), with frames to
// measure. Nothing when it would not — a refusal is an answer about the
// file, not a throw.
inline std::optional<wav::Info> measurableInfo(wav::BytesView head, std::int64_t fileSize)
{
    try {
        const wav::Info info = wav::assertUploadable(wav::readWavInfo(head, fileSize));
        if (info.frames <= 0)
            return std::nullopt;
        return info;
    } catch (const Error&) {
        return std::nullopt;
    }
}

// The same, off the file itself: its first kHeaderProbeBytes and its size.
// A file that cannot be opened or read that far is not one to file.
inline std::optional<wav::Info> measurableInfo(const juce::File& file)
{
    juce::FileInputStream in(file);
    if (!in.openedOk())
        return std::nullopt;
    const juce::int64 size = in.getTotalLength();
    if (size < 0)
        return std::nullopt;
    std::vector<unsigned char> head(
        static_cast<std::size_t>(std::min<juce::int64>(kHeaderProbeBytes, size)));
    const int got = in.read(head.data(), static_cast<int>(head.size()));
    if (got != static_cast<int>(head.size()))
        return std::nullopt;
    return measurableInfo(wav::BytesView(head.data(), head.size()), size);
}

// Whether JUCE's reader decodes the very frames the core would measure: as
// many of them, at the core's rate, in the core's channels. The rate is an
// integer in a WAV header and comes back whole from either reader, so the
// comparison is exact.
inline bool decodesAsCore(const juce::AudioFormatReader& reader, const wav::Info& info)
{
    return reader.lengthInSamples == info.frames
        && static_cast<int>(reader.numChannels) == info.channels
        && static_cast<std::int64_t>(std::llround(reader.sampleRate)) == info.sampleRate;
}

} // namespace loopercat::readerloudness
