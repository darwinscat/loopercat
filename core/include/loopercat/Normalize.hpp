// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The byte-level half of post-upload normalization (issue #53): measure a
// slot's loudness straight from its pedal-shaped bytes, and rewrite those
// bytes through one constant gain. The import path normalizes streams on
// their way in (app/WavImport); this header is for loops ALREADY on the card
// — a setlist assembled before the option existed can be evened without
// re-importing anything.
//
// It leans on Downmix.hpp deliberately: that header owns the float32 sample
// access (detail::f32/putF32) and the canonical rewrite (canonicalize), and
// this one reuses them rather than growing a second decoder. Like the fold,
// a gain moves NO frame — WavLen, MeasLen, Measure and LpLen all still
// describe the loop exactly, so the caller has no length or tempo field to
// recompute.
//
// And like the fold, any shape that is not the pedal's own stereo float32 is
// refused, not guessed at: a slot in another format did not come from this
// app, and normalizing what we misread would be the wrong kind of helpful.

#pragma once

#include "Downmix.hpp"
#include "Error.hpp"
#include "Loudness.hpp"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace loopercat::wav {

namespace detail {

    // The shape a file must have before its loudness means anything — the
    // fold's gate with an honest verb in the error.
    inline Info assertPedalStereoFloat(BytesView data, const std::string& verb)
    {
        const Info info = readWavInfo(data);
        if (info.format() != "float32")
            throw Error("only the pedal's own 32-bit float format can be " + verb + ", got "
                        + info.format());
        if (info.channels != 2)
            throw Error("only a stereo file can be " + verb + ", got "
                        + std::to_string(info.channels) + " channel(s)");
        if (info.frames <= 0)
            throw Error("file has no frames to be " + verb);
        return info;
    }

} // namespace detail

struct LoudnessReading {
    std::optional<double> integratedLufs; // empty: silence, or under one 400 ms gating block
    float samplePeak = 0.0f;
};

// BS.1770 integrated loudness and sample peak of a pedal-shaped file, read
// straight from its bytes — the same meter the import path runs on streams.
inline LoudnessReading measureLoudness(BytesView data)
{
    const Info info = detail::assertPedalStereoFloat(data, "measured");
    loudness::Meter meter(static_cast<int>(info.sampleRate));

    const std::size_t start = detail::dataChunkStart(data);
    const auto stride = static_cast<std::size_t>(info.blockAlign);
    constexpr std::size_t kChunkFrames = 4096; // decode in slices, not a whole-file copy
    std::vector<float> interleaved(2 * kChunkFrames);
    std::size_t filled = 0;
    for (std::int64_t frame = 0; frame < info.frames; ++frame) {
        const std::size_t o = start + static_cast<std::size_t>(frame) * stride;
        interleaved[2 * filled] = detail::f32(data, o);
        interleaved[2 * filled + 1] = detail::f32(data, o + 4);
        if (++filled == kChunkFrames) {
            meter.process(interleaved.data(), filled);
            filled = 0;
        }
    }
    if (filled > 0)
        meter.process(interleaved.data(), filled);
    return { meter.integratedLufs(), meter.samplePeak() };
}

// The file with one constant gain baked into every sample, in the pedal's
// canonical shape (DAW-added chunks stripped, samples at a known offset).
// Zero stays zero — sign included — because scaling cannot invent signal.
inline Bytes withGainDb(BytesView data, double gainDb)
{
    const Info info = detail::assertPedalStereoFloat(data, "normalized");
    Bytes out = canonicalize(data);
    if (!detail::chunkIdIs(out, kCanonicalFloatDataStart - 8, "data"))
        throw Error("internal: the canonical float32 layout moved");

    const auto scale = static_cast<float>(std::pow(10.0, gainDb / 20.0));
    const auto stride = static_cast<std::size_t>(info.blockAlign);
    for (std::int64_t frame = 0; frame < info.frames; ++frame) {
        const std::size_t o = kCanonicalFloatDataStart + static_cast<std::size_t>(frame) * stride;
        for (std::size_t channel = 0; channel < 2; ++channel)
            detail::putF32(out, o + channel * 4,
                           detail::f32(out, o + channel * 4) * scale);
    }
    return out;
}

} // namespace loopercat::wav
