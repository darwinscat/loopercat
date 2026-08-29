// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The mono fold (issue #43) against the THEORY of what it must guarantee,
// not against how it is written:
//
//   1. the result is the ARITHMETIC MEAN of the two channels, exactly;
//   2. it can never clip a file that did not already clip;
//   3. both output channels are bit-identical, which is the whole point —
//      the pedal's Pan then lands the loop whole on either jack;
//   4. NO FRAME MOVES: the frame count survives, which is what lets the
//      command leave WavLen/MeasLen/Measure/LpLen alone;
//   5. the output is the pedal's canonical float32 shape;
//   6. anything that is not the pedal's own stereo float32 is refused, not
//      decoded, not guessed at.
//
// Fixtures are hand-assembled from the RIFF spec (never from the reader under
// test), and every sample value is a dyadic rational so the expected result is
// exact in binary floating point and the checks can compare raw bits.

#include "support.hpp"

#include <loopercat/Downmix.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

using namespace loopercat;

namespace {

// A float32 stereo 44.1 kHz WAV holding exactly these frames. `extraChunk`
// inserts a LIST chunk before the audio, which moves the data chunk off the
// canonical offset — the shape a DAW export arrives in, and the one that
// catches a fold that assumed where samples begin.
std::vector<unsigned char> floatWav(const std::vector<std::pair<float, float>>& frames,
                                    bool extraChunk = false)
{
    constexpr int kChannels = 2;
    constexpr int kBits = 32;
    constexpr int kBlockAlign = kChannels * (kBits / 8);
    const auto dataSize = static_cast<int>(frames.size()) * kBlockAlign;
    const int extra = extraChunk ? 8 + 26 : 0;

    std::vector<unsigned char> b;
    const auto ascii = [&b](std::string_view s) {
        for (const char c : s)
            b.push_back(static_cast<unsigned char>(c));
    };
    const auto p16 = [&b](int v) {
        b.push_back(static_cast<unsigned char>(v & 0xff));
        b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    };
    const auto p32 = [&p16](int v) {
        p16(v & 0xffff);
        p16((v >> 16) & 0xffff);
    };
    const auto sample = [&b](float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (int shift = 0; shift < 32; shift += 8)
            b.push_back(static_cast<unsigned char>((bits >> shift) & 0xffu));
    };

    ascii("RIFF"); p32(12 + 24 + extra + 8 + dataSize - 8); ascii("WAVE");
    ascii("fmt "); p32(16);
    p16(3); // IEEE float
    p16(kChannels);
    p32(wav::kSampleRate);
    p32(wav::kSampleRate * kBlockAlign);
    p16(kBlockAlign);
    p16(kBits);
    if (extraChunk) {
        ascii("LIST"); p32(26);
        b.insert(b.end(), 26, 0);
    }
    ascii("data"); p32(dataSize);
    for (const auto& [left, right] : frames) {
        sample(left);
        sample(right);
    }
    return b;
}

// Raw bits of one sample in a canonical float32 file — compared as bits, so
// the checks stay exact and never lean on float equality.
std::uint32_t bitsAt(const wav::Bytes& file, std::int64_t frame, int channel)
{
    const std::size_t o = wav::kCanonicalFloatDataStart
                        + static_cast<std::size_t>(frame) * 8u
                        + static_cast<std::size_t>(channel) * 4u;
    return std::uint32_t{ file[o] } | std::uint32_t{ file[o + 1] } << 8
         | std::uint32_t{ file[o + 2] } << 16 | std::uint32_t{ file[o + 3] } << 24;
}

std::uint32_t bitsOf(float value) { return std::bit_cast<std::uint32_t>(value); }

// A pcm16 stereo file: the format the pedal will not play as an upload and
// the fold must not pretend to understand.
std::vector<unsigned char> pcm16Wav(int frames)
{
    return testkit::syntheticWav({ .tag = 1, .channels = 2, .bits = 16, .frames = frames });
}

} // namespace

int main()
{
    // --- 1. the fold is the exact arithmetic mean ---
    {
        // Every pair is dyadic, so the mean is exact in binary floating point
        // and "close enough" never enters the check.
        const std::vector<std::pair<float, float>> input {
            { 1.0f, -1.0f },   // cancels to silence
            { 0.5f, 0.25f },   // 0.375
            { 1.0f, 1.0f },    // stays 1.0 — a sum would clip here at 2.0
            { 0.75f, -0.25f }, // 0.25
            { -1.0f, -1.0f },  // -1.0, the negative rail
            { 0.0f, 0.125f },  // 0.0625
        };
        const std::vector<float> expected { 0.0f, 0.375f, 1.0f, 0.25f, -1.0f, 0.0625f };

        const auto source = floatWav(input);
        const wav::Bytes folded = wav::downmixedToMono(source, wav::Placement::BothOutputs);

        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto frame = static_cast<std::int64_t>(i);
            CHECK_EQ(bitsAt(folded, frame, 0), bitsOf(expected[i]));
            // --- 3. both channels carry the same bits ---
            CHECK_EQ(bitsAt(folded, frame, 0), bitsAt(folded, frame, 1));
        }
    }

    // --- 2. a fold can never clip a file that did not already clip ---
    {
        // Full-scale, correlated, on both rails: the case where summing
        // instead of averaging would double the peak.
        const auto source = floatWav({ { 1.0f, 1.0f }, { -1.0f, -1.0f }, { 1.0f, 0.5f } });
        const wav::Bytes folded = wav::downmixedToMono(source, wav::Placement::BothOutputs);
        for (std::int64_t frame = 0; frame < 3; ++frame) {
            const float value = std::bit_cast<float>(bitsAt(folded, frame, 0));
            CHECK(value <= 1.0f && value >= -1.0f);
        }
    }

    // --- 4. no frame moves ---
    {
        const auto source = floatWav(std::vector<std::pair<float, float>>(777, { 0.5f, 0.25f }));
        const wav::Info before = wav::readWavInfo(source);
        const wav::Info after = wav::readWavInfo(wav::downmixedToMono(source, wav::Placement::BothOutputs));
        CHECK_EQ(after.frames, before.frames);
        CHECK_EQ(after.frames, 777);
        CHECK_EQ(after.sampleRate, before.sampleRate);
        CHECK_EQ(after.channels, 2);
    }

    // --- 5. the output is the pedal's canonical float32 shape ---
    {
        const auto source = floatWav({ { 0.5f, 0.25f }, { 0.5f, 0.25f } });
        const wav::Bytes folded = wav::downmixedToMono(source, wav::Placement::BothOutputs);
        // Canonical means RIFF + fmt(28) + data and nothing else.
        CHECK_EQ(folded.size(), wav::kCanonicalFloatDataStart + 2u * 8u);
        CHECK_EQ(wav::readWavInfo(folded).format(), std::string("float32"));
        wav::assertUploadable(wav::readWavInfo(folded)); // throws if the pedal would refuse it
        // And it is already canonical: folding a folded file changes nothing.
        const wav::Bytes twice = wav::downmixedToMono(folded, wav::Placement::BothOutputs);
        CHECK(twice == folded);
    }

    // --- 5b. a DAW chunk before the audio does not shift the fold ---
    {
        // If the fold read samples from a fixed offset, this file would fold
        // the LIST chunk's zero bytes and quietly produce silence.
        const std::vector<std::pair<float, float>> input { { 1.0f, 0.5f }, { 0.25f, 0.75f } };
        const auto padded = floatWav(input, /*extraChunk=*/true);
        const wav::Bytes folded = wav::downmixedToMono(padded, wav::Placement::BothOutputs);
        CHECK_EQ(folded.size(), wav::kCanonicalFloatDataStart + 2u * 8u);
        CHECK_EQ(bitsAt(folded, 0, 0), bitsOf(0.75f));
        CHECK_EQ(bitsAt(folded, 1, 0), bitsOf(0.5f));
    }

    // --- 6. anything that is not the pedal's own stereo float32 is refused ---
    {
        CHECK_THROWS(wav::downmixedToMono(pcm16Wav(64), wav::Placement::BothOutputs), "32-bit float");
        CHECK_THROWS(wav::isDualMono(pcm16Wav(64)), "32-bit float");
        CHECK_THROWS(wav::downmixedToMono(testkit::syntheticWav(
                         { .tag = 1, .channels = 2, .bits = 24, .frames = 64 }), wav::Placement::BothOutputs),
                     "32-bit float");
        // Mono float32: the pedal never writes one, and folding it is not a
        // no-op to wave through — it is a file that does not belong here.
        CHECK_THROWS(wav::downmixedToMono(testkit::syntheticWav(
                         { .tag = 3, .channels = 1, .bits = 32, .frames = 64 }), wav::Placement::BothOutputs),
                     "stereo");
        // An empty loop: "both channels already match" would be true and
        // useless, so it must fail as what it is.
        CHECK_THROWS(wav::downmixedToMono(floatWav({}), wav::Placement::BothOutputs), "no frames");
        CHECK_THROWS(wav::isDualMono(floatWav({})), "no frames");
        // Truncated mid-audio — the reader's own guard must still fire.
        auto truncated = floatWav({ { 0.5f, 0.5f }, { 0.5f, 0.5f } });
        truncated.resize(truncated.size() - 5);
        CHECK_THROWS(wav::downmixedToMono(truncated, wav::Placement::BothOutputs), "truncated");
        CHECK_THROWS(wav::downmixedToMono(std::vector<unsigned char> { 'n', 'o' }, wav::Placement::BothOutputs),
                     "not a RIFF/WAVE file");
    }

    // --- isDualMono answers "would folding change this file" ---
    {
        const std::size_t kFrames = 64;
        const std::vector<std::pair<float, float>> same(kFrames, { 0.5f, 0.5f });
        CHECK(wav::isDualMono(floatWav(same)));
        CHECK(wav::isDualMono(floatWav(same, /*extraChunk=*/true))); // offset-independent

        // A single differing frame must be found wherever it hides — the
        // first, the last, and one in the middle: an off-by-one in either
        // bound would report a stereo loop as already folded and refuse to
        // fold it.
        for (const std::size_t where : { std::size_t { 0 }, kFrames / 2, kFrames - 1 }) {
            auto frames = same;
            frames[where] = { 0.5f, 0.25f };
            CHECK(!wav::isDualMono(floatWav(frames)));
        }

        // Differing in the low mantissa byte alone — inaudible, but the file
        // is not dual mono and the fold would still rewrite it.
        {
            auto frames = same;
            frames[7] = { 0.5f, std::bit_cast<float>(bitsOf(0.5f) | 1u) };
            CHECK(!wav::isDualMono(floatWav(frames)));
        }

        // Two NaNs with the same payload: identical bytes, so folding changes
        // nothing — the answer float equality could not give, since a NaN
        // never compares equal to itself.
        {
            const float nan = std::bit_cast<float>(std::uint32_t { 0x7fc00001u });
            CHECK(wav::isDualMono(floatWav({ { nan, nan } })));
        }

        // A folded file is dual mono by construction — the two functions must
        // agree, or the command could fold the same slot forever.
        {
            const std::vector<std::pair<float, float>> stereo { { 1.0f, -0.5f }, { 0.25f, 0.75f } };
            CHECK(!wav::isDualMono(floatWav(stereo)));
            CHECK(wav::isDualMono(wav::downmixedToMono(floatWav(stereo), wav::Placement::BothOutputs)));
        }
    }


    // --- placement: the fold goes where it was told, the other side is silent ---
    {
        // Channel 1 reaches OUTPUT A and channel 2 reaches OUTPUT B (measured
        // on hardware 2026-08-25), so "which channel" IS "which jack".
        const std::vector<std::pair<float, float>> stereo { { 1.0f, 0.5f }, { -0.5f, 0.25f } };
        const std::vector<float> mean { 0.75f, -0.125f };

        const wav::Bytes onA = wav::downmixedToMono(floatWav(stereo), wav::Placement::OutputAOnly);
        const wav::Bytes onB = wav::downmixedToMono(floatWav(stereo), wav::Placement::OutputBOnly);

        for (std::int64_t frame = 0; frame < 2; ++frame) {
            const auto i = static_cast<std::size_t>(frame);
            CHECK_EQ(bitsAt(onA, frame, 0), bitsOf(mean[i]));
            CHECK_EQ(bitsAt(onB, frame, 1), bitsOf(mean[i]));
            // Exactly positive zero, not "small". A residue of the discarded
            // side would defeat the whole point of choosing a jack.
            CHECK_EQ(bitsAt(onA, frame, 1), 0u);
            CHECK_EQ(bitsAt(onB, frame, 0), 0u);
        }

        // Placement changes nothing else about the file.
        const wav::Info before = wav::readWavInfo(floatWav(stereo));
        for (const wav::Bytes& placed : { onA, onB }) {
            const wav::Info after = wav::readWavInfo(placed);
            CHECK_EQ(after.frames, before.frames);
            CHECK_EQ(after.sampleRate, before.sampleRate);
            CHECK_EQ(after.channels, 2);
            CHECK_EQ(placed.size(), wav::kCanonicalFloatDataStart + 2u * 8u);
            wav::assertUploadable(wav::readWavInfo(placed));
        }
    }

    // --- placement must not cost the loop 6 dB, ever ---
    {
        // The path a user actually walks: fold onto OUTPUT A, change their
        // mind, fold onto OUTPUT B. Averaging a lone signal against digital
        // silence would halve it here, and again on the way back.
        const std::vector<std::pair<float, float>> stereo { { 1.0f, 0.5f }, { -0.5f, 0.25f } };
        const std::vector<float> mean { 0.75f, -0.125f };

        const wav::Bytes onA = wav::downmixedToMono(floatWav(stereo), wav::Placement::OutputAOnly);
        const wav::Bytes moved = wav::downmixedToMono(onA, wav::Placement::OutputBOnly);
        const wav::Bytes spread = wav::downmixedToMono(onA, wav::Placement::BothOutputs);
        const wav::Bytes back = wav::downmixedToMono(moved, wav::Placement::OutputAOnly);

        for (std::int64_t frame = 0; frame < 2; ++frame) {
            const auto i = static_cast<std::size_t>(frame);
            CHECK_EQ(bitsAt(moved, frame, 1), bitsOf(mean[i]));  // full level, not half
            CHECK_EQ(bitsAt(moved, frame, 0), 0u);
            CHECK_EQ(bitsAt(spread, frame, 0), bitsOf(mean[i]));
            CHECK_EQ(bitsAt(spread, frame, 1), bitsOf(mean[i]));
        }
        CHECK(back == onA); // A -> B -> A is exactly where it started

        // Same rule for a take that arrived one-sided on its own — a guitar
        // recorded through INPUT A alone must not fold 6 dB down.
        const wav::Bytes lone =
            wav::downmixedToMono(floatWav({ { 0.5f, 0.0f }, { -0.25f, 0.0f } }),
                                 wav::Placement::BothOutputs);
        CHECK_EQ(bitsAt(lone, 0, 0), bitsOf(0.5f));
        CHECK_EQ(bitsAt(lone, 1, 0), bitsOf(-0.25f));

        // Negative zero is still silence: the rule must not depend on how the
        // zero was spelled.
        const wav::Bytes negZero =
            wav::downmixedToMono(floatWav({ { 0.5f, -0.0f } }), wav::Placement::BothOutputs);
        CHECK_EQ(bitsAt(negZero, 0, 0), bitsOf(0.5f));

        // A NaN is NOT silence — it is signal, and the mean must be taken.
        {
            const float nan = std::bit_cast<float>(std::uint32_t { 0x7fc00001u });
            const wav::Bytes withNan =
                wav::downmixedToMono(floatWav({ { 0.5f, nan } }), wav::Placement::BothOutputs);
            CHECK(std::isnan(std::bit_cast<float>(bitsAt(withNan, 0, 0))));
        }
    }

    // --- "would this change anything" is asked per placement ---
    {
        const std::vector<std::pair<float, float>> stereo { { 1.0f, -0.5f }, { 0.25f, 0.75f } };
        const auto source = floatWav(stereo);

        // A real stereo take is changed by every placement.
        for (const wav::Placement placement : { wav::Placement::BothOutputs,
                                                wav::Placement::OutputAOnly,
                                                wav::Placement::OutputBOnly })
            CHECK(!wav::foldWouldChangeNothing(source, placement));

        // A loop already folded across both jacks: a no-op for BothOutputs,
        // and a genuine rewrite for either single jack. This is exactly what
        // isDualMono could not answer once placement existed.
        const wav::Bytes both = wav::downmixedToMono(source, wav::Placement::BothOutputs);
        CHECK(wav::foldWouldChangeNothing(both, wav::Placement::BothOutputs));
        CHECK(!wav::foldWouldChangeNothing(both, wav::Placement::OutputAOnly));
        CHECK(!wav::foldWouldChangeNothing(both, wav::Placement::OutputBOnly));

        // And a loop already sitting on one jack.
        const wav::Bytes onA = wav::downmixedToMono(source, wav::Placement::OutputAOnly);
        CHECK(wav::foldWouldChangeNothing(onA, wav::Placement::OutputAOnly));
        CHECK(!wav::foldWouldChangeNothing(onA, wav::Placement::OutputBOnly));
        CHECK(!wav::foldWouldChangeNothing(onA, wav::Placement::BothOutputs));

        // The answer is about samples, so a DAW chunk before the audio must
        // not change it — a header rewrite is not what a player means by
        // "this changed my loop".
        const std::vector<std::pair<float, float>> same(4, { 0.5f, 0.5f });
        CHECK(wav::foldWouldChangeNothing(floatWav(same), wav::Placement::BothOutputs));
        CHECK(wav::foldWouldChangeNothing(floatWav(same, /*extraChunk=*/true),
                                          wav::Placement::BothOutputs));
        CHECK(!wav::foldWouldChangeNothing(floatWav(same, /*extraChunk=*/true),
                                           wav::Placement::OutputAOnly));

        // The format gate answers before the question does.
        CHECK_THROWS(wav::foldWouldChangeNothing(pcm16Wav(64), wav::Placement::BothOutputs),
                     "32-bit float");
    }

    return testkit::summary("downmix");
}
