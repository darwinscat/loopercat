// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Folding a loop's two channels into one signal (issue #43).
//
// Wav.hpp reads headers and never decodes a sample; this is the one place
// that does, so it lives in its own header rather than widening that charter.
//
// Why an app-side fold exists at all: the pedal already places a loop on one
// output jack through its own TRACK1 <Pan> (docs/pedal-settings.md). What it
// cannot do is fold a file it was handed — so the app supplies that half, and
// Pan supplies the placement. A loop whose channels are identical arrives
// whole on the chosen jack whichever way Pan turns out to be implemented: a
// true pan folds to mono itself, a balance keeps one side, and when both
// sides are equal the two agree. That is the point of folding first.
//
// The fold AVERAGES, it does not sum: |0.5*(L+R)| never exceeds max(|L|,|R|),
// so folding can never clip a file that did not already clip. Summing adds up
// to 6 dB on correlated material and the pedal's output would clip it.
//
// Both output channels carry the fold. The pedal wants stereo (Wav.hpp
// assertUploadable) and a one-sided file would be a different feature: this
// one makes the loop channel-independent, it does not choose a side.

#pragma once

#include "Error.hpp"
#include "Wav.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>

namespace loopercat::wav {

namespace detail {

    // WAV float32 samples are little-endian IEEE-754. Assembled byte by byte
    // like every other field in this reader, so the layout never depends on
    // the host's endianness.
    inline float f32(BytesView data, std::size_t offset)
    {
        const std::uint32_t bits = std::uint32_t{ data[offset] }
                                 | std::uint32_t{ data[offset + 1] } << 8
                                 | std::uint32_t{ data[offset + 2] } << 16
                                 | std::uint32_t{ data[offset + 3] } << 24;
        return std::bit_cast<float>(bits);
    }

    inline void putF32(Bytes& out, std::size_t offset, float value)
    {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        out[offset] = static_cast<unsigned char>(bits & 0xff);
        out[offset + 1] = static_cast<unsigned char>((bits >> 8) & 0xff);
        out[offset + 2] = static_cast<unsigned char>((bits >> 16) & 0xff);
        out[offset + 3] = static_cast<unsigned char>((bits >> 24) & 0xff);
    }

    // The shape a file must have before a fold means anything: the pedal's
    // own stereo float32. Any other format is refused outright rather than
    // decoded — a slot in some other format did not come from this app, and
    // guessing at it would be the wrong kind of helpful.
    inline Info assertFoldable(BytesView data)
    {
        const Info info = readWavInfo(data);
        if (info.format() != "float32")
            throw Error("only the pedal's own 32-bit float format can be folded, got "
                        + info.format());
        if (info.channels != 2)
            throw Error("only a stereo file can be folded to mono, got "
                        + std::to_string(info.channels) + " channel(s)");
        // An empty loop must fail here and not further down, where "both
        // channels already match" would be a true sentence about nothing.
        if (info.frames <= 0)
            throw Error("file has no frames to fold");
        return info;
    }

} // namespace detail

// Where samples begin in a file canonicalize() wrote: RIFF+WAVE (12), the
// "fmt " header (8) and its 28-byte float body, then the "data" header (8).
inline constexpr std::size_t kCanonicalFloatDataStart = 12 + 8 + 28 + 8;

// True when folding would change nothing — every frame already carries the
// same bytes in both channels. Byte equality rather than float equality: the
// question is exactly "would the rewrite alter this file", which is what the
// bytes answer, and NaN payloads answer it honestly where == would not.
inline bool isDualMono(BytesView data)
{
    const Info info = detail::assertFoldable(data);
    const std::size_t start = detail::dataChunkStart(data);
    const auto stride = static_cast<std::size_t>(info.blockAlign);
    for (std::int64_t frame = 0; frame < info.frames; ++frame) {
        const std::size_t o = start + static_cast<std::size_t>(frame) * stride;
        for (std::size_t b = 0; b < 4; ++b)
            if (data[o + b] != data[o + 4 + b])
                return false;
    }
    return true;
}

// The loop with both channels carrying 0.5*(L+R), in the pedal's canonical
// shape. Frame count, sample rate and format are untouched: folding moves no
// frame, which is why the caller has no length or tempo field to recompute.
inline Bytes downmixedToMono(BytesView data)
{
    const Info info = detail::assertFoldable(data);
    Bytes out = canonicalize(data); // strips any DAW chunks; samples land at a known offset
    if (!detail::chunkIdIs(out, kCanonicalFloatDataStart - 8, "data"))
        throw Error("internal: the canonical float32 layout moved");

    const auto stride = static_cast<std::size_t>(info.blockAlign);
    for (std::int64_t frame = 0; frame < info.frames; ++frame) {
        const std::size_t o = kCanonicalFloatDataStart + static_cast<std::size_t>(frame) * stride;
        const float mono = 0.5f * (detail::f32(out, o) + detail::f32(out, o + 4));
        detail::putF32(out, o, mono);
        detail::putF32(out, o + 4, mono);
    }
    return out;
}

} // namespace loopercat::wav
