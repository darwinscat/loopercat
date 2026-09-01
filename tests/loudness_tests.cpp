// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The BS.1770 meter (issue #53) against the THEORY of the measure, never the
// implementation:
//
//   1. EBU Tech 3341 Table 1 defines the conformance signals mathematically —
//      997 Hz sines at set levels and durations, expected integrated loudness
//      ±0.1 LU — so the fixtures are synthesized right here from the spec;
//   2. the gates must actually gate: quiet passages (case 3), near-silence
//      below the absolute gate (case 4), and a louder middle (case 5) all
//      land on the same -23.0 LUFS;
//   3. the K in K-weighting must be real: a high tone reads hotter (the +4 dB
//      shelf), a low tone reads quieter (the RLB high-pass), DC is rejected —
//      a flat RMS meter fails all three;
//   4. unmeasurable input says so (nullopt): silence, sub-block-length audio,
//      signal wholly under the -70 LUFS gate — never a made-up number;
//   5. chunking must not matter: the same stream fed in odd-sized pieces
//      reads identically;
//   6. the gain rule is the rsgain/loudgain contract: cuts in full, boosts
//      capped by the peak ceiling, and the cap never inverts a boost.

#include "support.hpp"

#include <loopercat/Loudness.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

using namespace loopercat;

namespace {

constexpr int kRate = 44100;      // the pedal's world — wav::kSampleRate
constexpr double kToneHz = 997.0; // EBU Tech 3341's reference tone

// Appends `seconds` of a stereo sine at `dbfs` peak amplitude, identical in
// both channels, phase-continuous across calls through the caller's running
// sample counter — the level steps of cases 3-5 must not add click energy.
void feedSine(loudness::Meter& meter, double hz, double dbfs, double seconds,
              long long& sampleIndex, std::size_t chunkFrames = 8192)
{
    const double amp = std::pow(10.0, dbfs / 20.0);
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    auto frames = static_cast<std::size_t>(std::llround(seconds * kRate));
    std::vector<float> buf(2 * chunkFrames);
    while (frames > 0) {
        const std::size_t n = std::min(frames, chunkFrames);
        for (std::size_t i = 0; i < n; ++i) {
            const auto v
                = static_cast<float>(amp * std::sin(w * static_cast<double>(sampleIndex++)));
            buf[2 * i] = v;
            buf[2 * i + 1] = v;
        }
        meter.process(buf.data(), n);
        frames -= n;
    }
}

// One fresh meter over a whole Tech 3341-style program: (dbfs, seconds) steps.
std::optional<double> measure(const std::vector<std::pair<double, double>>& steps)
{
    loudness::Meter meter(kRate);
    long long sampleIndex = 0;
    for (const auto& [dbfs, seconds] : steps)
        feedSine(meter, kToneHz, dbfs, seconds, sampleIndex);
    return meter.integratedLufs();
}

void checkNear(double actual, double expected, double tol, const char* what, const char* file,
               int line)
{
    ++testkit::checksRun;
    if (!(std::abs(actual - expected) <= tol)) {
        std::ostringstream os;
        os << what << "  (actual: " << actual << ", expected: " << expected << " +/- " << tol
           << ")";
        testkit::fail(os.str(), file, line);
    }
}

#define CHECK_NEAR(actual, expected, tol)                                                          \
    ::checkNear((actual), (expected), (tol), #actual " ~= " #expected, __FILE__, __LINE__)

} // namespace

int main()
{
    // --- EBU Tech 3341 Table 1, the integrated-loudness minimum requirements

    {
        // Case 1: 997 Hz at -23 dBFS, 20 s -> -23.0 LUFS. Calibration: the
        // -0.691 term exactly cancels the K-filter's gain at 997 Hz.
        const auto i = measure({ { -23.0, 20.0 } });
        CHECK(i.has_value());
        CHECK_NEAR(*i, -23.0, 0.1);
    }
    {
        // Case 2: same tone at -33 dBFS -> -33.0 LUFS (linearity).
        const auto i = measure({ { -33.0, 20.0 } });
        CHECK(i.has_value());
        CHECK_NEAR(*i, -33.0, 0.1);
    }
    {
        // Case 3: quiet -36 dBFS shoulders around a -23 dBFS body. The
        // relative gate (-10 LU under the gated mean) drops the shoulders;
        // ungated averaging would read ~-24.2.
        const auto i = measure({ { -36.0, 10.0 }, { -23.0, 60.0 }, { -36.0, 10.0 } });
        CHECK(i.has_value());
        CHECK_NEAR(*i, -23.0, 0.1);
    }
    {
        // Case 4: case 3 wrapped in -72 dBFS — under the -70 LUFS absolute
        // gate, so those blocks never even join the mean the relative gate is
        // computed from.
        const auto i = measure({ { -72.0, 10.0 },
                                 { -36.0, 10.0 },
                                 { -23.0, 60.0 },
                                 { -36.0, 10.0 },
                                 { -72.0, 10.0 } });
        CHECK(i.has_value());
        CHECK_NEAR(*i, -23.0, 0.1);
    }
    {
        // Case 5: -26 / -20 / -26 dBFS, 20 / 20.1 / 20 s — everything gated
        // IN (all blocks within 10 LU of the mean), power-averaging to -23.0.
        const auto i = measure({ { -26.0, 20.0 }, { -20.0, 20.1 }, { -26.0, 20.0 } });
        CHECK(i.has_value());
        CHECK_NEAR(*i, -23.0, 0.1);
    }

    // --- unmeasurable input answers nullopt, never a number

    {
        // Digital silence: blocks exist, every one of them is -inf.
        loudness::Meter meter(kRate);
        const std::vector<float> zeros(2 * 44100, 0.0f);
        meter.process(zeros.data(), 44100);
        CHECK(!meter.integratedLufs().has_value());
        CHECK_EQ(std::bit_cast<std::uint32_t>(meter.samplePeak()),
                 std::bit_cast<std::uint32_t>(0.0f));
    }
    {
        // A tone wholly under the absolute gate: -80 dBFS reads ~-80.7 LUFS,
        // below -70 — gated to nothing.
        const auto i = measure({ { -80.0, 2.0 } });
        CHECK(!i.has_value());
    }
    {
        // 399 ms is one sub-block short of the first 400 ms gating block.
        const auto i = measure({ { -23.0, 0.399 } });
        CHECK(!i.has_value());
    }
    {
        // Exactly 400 ms is exactly one block — measurable.
        const auto i = measure({ { -23.0, 0.400 } });
        CHECK(i.has_value());
    }

    // --- the K in K-weighting: shelf up high, RLB down low, no DC

    {
        // 8 kHz sits near the top of the shelf's knee: the prototype's G is
        // +4 dB asymptotically, so the reading lands a few dB hot of -23.
        loudness::Meter meter(kRate);
        long long n = 0;
        feedSine(meter, 8000.0, -23.0, 10.0, n);
        const auto i = meter.integratedLufs();
        CHECK(i.has_value());
        CHECK(*i > -23.0 + 2.5);
        CHECK(*i < -23.0 + 4.5);
    }
    {
        // 60 Hz is on the RLB high-pass slope (f0 ~38 Hz, overdamped):
        // |H| ~ -3 dB there, so the reading lands a few dB under -23.
        loudness::Meter meter(kRate);
        long long n = 0;
        feedSine(meter, 60.0, -23.0, 10.0, n);
        const auto i = meter.integratedLufs();
        CHECK(i.has_value());
        CHECK(*i < -23.0 - 2.0);
        CHECK(*i > -23.0 - 4.5);
    }
    {
        // DC at -6 dBFS: a flat RMS meter would read ~-3.7 LUFS. The
        // high-pass leaves only the switch-on step transient (the first
        // blocks), so whatever survives gating is far, far quieter.
        loudness::Meter meter(kRate);
        std::vector<float> dc(2 * 44100, 0.5f);
        for (int s = 0; s < 5; ++s)
            meter.process(dc.data(), 44100);
        const auto i = meter.integratedLufs();
        CHECK(!i.has_value() || *i < -15.0);
        // The peak meter is pre-filter: it must see the raw 0.5 exactly.
        CHECK_EQ(std::bit_cast<std::uint32_t>(meter.samplePeak()),
                 std::bit_cast<std::uint32_t>(0.5f));
    }

    // --- chunking must not matter

    {
        loudness::Meter one(kRate);
        loudness::Meter many(kRate);
        long long n1 = 0, n2 = 0;
        feedSine(one, kToneHz, -23.0, 5.0, n1, 220500); // one call
        feedSine(many, kToneHz, -23.0, 5.0, n2, 4097);  // odd chunks, straddling sub-blocks
        const auto a = one.integratedLufs();
        const auto b = many.integratedLufs();
        CHECK(a.has_value() && b.has_value());
        CHECK_NEAR(*a, *b, 1e-9);
    }

    // --- the peak meter reads raw samples, sign-blind, across both channels

    {
        loudness::Meter meter(kRate);
        // Dyadic values: the expected peak is exact in binary float.
        const std::vector<float> frames = { 0.25f, -0.75f, 0.5f, 0.125f };
        meter.process(frames.data(), 2);
        CHECK_EQ(std::bit_cast<std::uint32_t>(meter.samplePeak()),
                 std::bit_cast<std::uint32_t>(0.75f));
    }

    // --- the meter refuses a rate it cannot cut into 100 ms sub-blocks

    {
        CHECK_THROWS(loudness::Meter(44101), "divisible by 10");
        CHECK_THROWS(loudness::Meter(0), "positive sample rate");
        CHECK_THROWS(loudness::Meter(-44100), "positive sample rate");
    }

    // --- bytes that are not audio are counted, never averaged into a "loudness"

    {
        // inf, NaN, the absurd and the merely impossible (-9: honest audio
        // stops at +/-8) — four wild samples among four honest ones.
        loudness::Meter meter(kRate);
        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::vector<float> frames = { inf, 0.5f, 0.1f, nan, 1.0e30f, -9.0f, 0.25f, -0.25f };
        meter.process(frames.data(), 4);
        CHECK_EQ(meter.wildSamples(), 4);
    }
    {
        // A whole second of a hot-but-honest tone (-0.5 dBFS): zero wild.
        loudness::Meter meter(kRate);
        long long n = 0;
        feedSine(meter, kToneHz, -0.5, 1.0, n);
        CHECK_EQ(meter.wildSamples(), 0);
    }

    // --- normalizeGainDb: cuts in full, boosts capped, cap never inverts

    {
        // A cut ignores the peak entirely — attenuation cannot clip.
        CHECK_NEAR(loudness::normalizeGainDb(-10.0, -18.0, 1.0f, loudness::kPeakCeilingDb), -8.0,
                   1e-12);
        // A boost with headroom applies in full: peak 0.05 is ~-26 dBFS,
        // +12 dB keeps it far under the -1 dB ceiling.
        CHECK_NEAR(loudness::normalizeGainDb(-30.0, -18.0, 0.05f, loudness::kPeakCeilingDb), 12.0,
                   1e-12);
        // A quiet-but-peaky track: +12 dB wanted, but peak 0.5 (-6.02 dBFS)
        // caps the boost at ceiling - peak = -1 + 6.0206 dB.
        CHECK_NEAR(loudness::normalizeGainDb(-30.0, -18.0, 0.5f, loudness::kPeakCeilingDb),
                   -1.0 - 20.0 * std::log10(0.5), 1e-6);
        // Already peaking above the ceiling: the boost collapses to zero —
        // never into a cut the player did not ask for.
        CHECK_NEAR(loudness::normalizeGainDb(-20.0, -18.0, 1.0f, loudness::kPeakCeilingDb), 0.0,
                   1e-12);
        // A boost from a zero peak is a contradiction (a measurable track has
        // signal) — typed error, not a guess.
        CHECK_THROWS(loudness::normalizeGainDb(-30.0, -18.0, 0.0f, loudness::kPeakCeilingDb),
                     "positive sample peak");
    }

    return testkit::summary("loudness");
}
