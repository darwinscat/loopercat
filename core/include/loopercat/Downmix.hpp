// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Folding a loop's two channels into one signal (issue #43).
//
// Wav.hpp reads headers and never decodes a sample; this is the one place
// that does, so it lives in its own header rather than widening that charter.
//
// Why the app does the whole job, placement included: the RC-5 has no pan.
// Its reference manual prints every memory parameter in three tables (LOOP,
// RHYTHM, NAME) and none of them is a pan — that is the decisive evidence,
// since a per-memory parameter would be printed there. It is absent from the
// SETUP pages as well. So the <Pan> tag sitting in TRACK1 of a .RC0 is a field
// this format inherited from its RC-500/RC-505 relatives, not a knob this
// pedal turns. Writing it would be writing to nobody.
//
// What the pedal DOES do is send file channel 1 to OUTPUT A and channel 2 to
// OUTPUT B, untouched and unmixed — measured on real hardware 2026-08-25 with
// 441 Hz in one channel and 1470 Hz in the other, each arriving at its own
// jack alone. That measurement had BOTH jacks patched, and the condition is
// load-bearing: with only OUTPUT A (MONO) in use the pedal folds its own
// output down to mono, so a loop placed on OUTPUT B alone is still heard.
// Placement separates the jacks; it does not hide a loop from a mono rig.
//
// So the loop's position is a property of the FILE, and it is ours to write.
//
// That is what makes the reporter's ask reachable (issue #43): fold the two
// channels into one signal, then put that signal where it should come out —
// both jacks, OUTPUT A alone, or OUTPUT B alone. Folding first is what makes
// a one-sided placement honest: a loop placed on OUTPUT B alone still carries
// everything that was in the take, because the fold already merged it.
//
// The fold AVERAGES, it does not sum: |0.5*(L+R)| never exceeds max(|L|,|R|),
// so folding can never clip a file that did not already clip. Summing adds up
// to 6 dB on correlated material and the pedal's output would clip it.
//
// With ONE exception, and it exists because placement created the case: when
// a channel is digital silence in every single frame, the mono signal is the
// OTHER channel at its own level, not half of it. A signal must not be
// diluted by nothing. Without this rule, moving an already-placed loop from
// OUTPUT A to OUTPUT B would quietly cost it 6 dB, spreading it back across
// both jacks would cost another 6, and a take recorded through INPUT A alone
// would fold 6 dB down for no reason a player could see.
//
// A one-sided file is still stereo on disk — the pedal only accepts stereo
// (Wav.hpp assertUploadable) — with the unused channel written as positive
// zero rather than left as it was. Silence has to be exact: a residue of the
// discarded side would defeat the entire point of the placement.

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

// Where the folded signal should come out. The names are the pedal's own —
// its jacks are lettered OUTPUT A (MONO) and OUTPUT B, not left and right,
// and a user reading a label here is looking at the back of an RC-5.
//
// File channel 1 reaches OUTPUT A and channel 2 reaches OUTPUT B; that
// mapping is measured, not assumed (see the note at the top of this file).
enum class Placement {
    BothOutputs, // the fold in both channels: the loop stops caring about jacks
    OutputAOnly, // the fold in channel 1, channel 2 exactly silent
    OutputBOnly, // the fold in channel 2, channel 1 exactly silent
};

// For error messages and dialogs, so one spelling of these names exists.
inline std::string placementName(Placement placement)
{
    switch (placement) {
        case Placement::BothOutputs: return "both outputs";
        case Placement::OutputAOnly: return "OUTPUT A only";
        case Placement::OutputBOnly: return "OUTPUT B only";
    }
    throw Error("internal: unknown placement");
}

// The loop folded to one signal and put where `placement` says, in the
// pedal's canonical shape. Frame count, sample rate and format are untouched:
// folding moves no frame, which is why the caller has no length or tempo
// field to recompute.
//
// The silent side is written as positive zero for every frame rather than
// left alone — see the note at the top about silence having to be exact.
inline Bytes downmixedToMono(BytesView data, Placement placement)
{
    const Info info = detail::assertFoldable(data);
    Bytes out = canonicalize(data); // strips any DAW chunks; samples land at a known offset
    if (!detail::chunkIdIs(out, kCanonicalFloatDataStart - 8, "data"))
        throw Error("internal: the canonical float32 layout moved");

    const auto stride = static_cast<std::size_t>(info.blockAlign);
    const auto sampleAt = [&out, stride](std::int64_t frame, std::size_t channel) {
        return detail::f32(out, kCanonicalFloatDataStart
                                    + static_cast<std::size_t>(frame) * stride + channel * 4);
    };

    // Which sides carry anything at all. Zero compares equal whether it is
    // written +0.0 or -0.0, which is what we want; a NaN is not zero and so
    // counts as signal, which is also what we want — it is not silence.
    bool silent[2] = { true, true };
    for (std::int64_t frame = 0; frame < info.frames && (silent[0] || silent[1]); ++frame)
        for (std::size_t c = 0; c < 2; ++c)
            if (silent[c] && sampleAt(frame, c) != 0.0f)
                silent[c] = false;

    // Exactly one side carries signal: that side IS the mono signal, undiluted
    // (see the note above). Otherwise the mean of the two.
    const int lone = silent[0] != silent[1] ? (silent[0] ? 1 : 0) : -1;

    for (std::int64_t frame = 0; frame < info.frames; ++frame) {
        const std::size_t o = kCanonicalFloatDataStart + static_cast<std::size_t>(frame) * stride;
        const float mono = lone >= 0
                             ? sampleAt(frame, static_cast<std::size_t>(lone))
                             : 0.5f * (sampleAt(frame, 0) + sampleAt(frame, 1));
        detail::putF32(out, o, placement == Placement::OutputBOnly ? 0.0f : mono);
        detail::putF32(out, o + 4, placement == Placement::OutputAOnly ? 0.0f : mono);
    }
    return out;
}

// True when this exact fold would leave every sample exactly as it is.
//
// The question the caller actually has is "is this operation a no-op", and
// with a placement to choose it is no longer the same question as "are the
// two channels equal": a dual-mono loop is untouched by a fold to both
// outputs and genuinely rewritten by a fold onto OUTPUT B. Producing the
// result and comparing answers it without a special case per placement.
//
// SAMPLES, not the whole file: canonicalising a header is not what a player
// means by "this changed my loop", and a fold that only rewrote chunk
// boundaries would still spend a trash copy and a pedal write generation. So
// the comparison walks frames from wherever each file's audio begins, exactly
// as isDualMono does.
inline bool foldWouldChangeNothing(BytesView data, Placement placement)
{
    const Info info = detail::assertFoldable(data);
    const Bytes folded = downmixedToMono(data, placement);
    const std::size_t from = detail::dataChunkStart(data);
    const auto stride = static_cast<std::size_t>(info.blockAlign);
    for (std::int64_t frame = 0; frame < info.frames; ++frame) {
        const std::size_t src = from + static_cast<std::size_t>(frame) * stride;
        const std::size_t dst = kCanonicalFloatDataStart + static_cast<std::size_t>(frame) * stride;
        for (std::size_t b = 0; b < 8; ++b)
            if (data[src + b] != folded[dst + b])
                return false;
    }
    return true;
}

} // namespace loopercat::wav
