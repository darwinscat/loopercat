// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Loudness measurement for the normalize feature (issue #53): ITU-R BS.1770-4
// gated integrated loudness — the measure behind EBU R128 and ReplayGain 2.0,
// whose reference of -18 LUFS is the modern spelling of mp3gain's "89 dB".
//
// Why hand-rolled: the algorithm is small and fully specified — two fixed
// biquads (K-weighting), 400 ms windows at 75 % overlap, two gates — and the
// conformance signals of EBU Tech 3341 Table 1 are defined mathematically
// (997 Hz sines with expected readings ±0.1 LU), so the test suite can
// synthesize its fixtures from theory, the house style. A library dependency
// would buy nothing the spec does not already give away.
//
// Where every constant comes from:
//   * K-weighting stage 1 (the high shelf modelling the acoustic effect of
//     the head) and stage 2 (the RLB high-pass): BS.1770-4 prints digital
//     coefficients for 48 kHz only. This file derives them at the meter's own
//     sample rate via the bilinear transform from the analog prototype
//     constants (f0, G, Q) pinned by the libebur128 re-derivation — plugging
//     48000 into these formulas reproduces the spec's published table.
//   * -0.691: the spec's own calibration term, chosen so a full-scale 997 Hz
//     sine reads 0 LUFS through the K-filter's +0.691 dB gain at 997 Hz.
//   * 400 ms blocks, 75 % overlap, -70 LUFS absolute gate, -10 LU relative
//     gate: BS.1770-4 Annex 1 (the gating annex).
//
// The meter is stereo by charter: it measures the exact two-channel stream
// the app writes to the card (channel weights are 1.0 for left and right in
// BS.1770), so every track normalized through this path lands equal — which
// is the whole feature: a setlist with no volume jumps between songs.
//
// The gain rule (normalizeGainDb) is the rsgain/loudgain lineage: a cut
// applies in full; a boost stops where the peak would cross the ceiling, so a
// quiet-but-peaky track lands short of target instead of distorting; and the
// cap never turns a boost into a cut. Sample peak is the v1 peak meter; the
// ceiling stays at -1 dB so inter-sample overs — which a true-peak
// (oversampled) meter would catch exactly — keep headroom until that upgrade
// (issue #53).

#pragma once

#include "Error.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace loopercat::loudness {

// BS.1770-4 Annex 1: gating blocks are 400 ms at 75 % overlap, i.e. a new
// block every 100 ms — so the meter accumulates energy in 100 ms sub-blocks
// and every run of four consecutive sub-blocks is one gating block.
inline constexpr std::size_t kSubBlocksPerBlock = 4;

inline constexpr double kAbsoluteGateLufs = -70.0; // BS.1770-4 Annex 1
inline constexpr double kRelativeGateLu = 10.0;    // BS.1770-4 Annex 1
inline constexpr double kCalibrationDb = -0.691;   // BS.1770-4 eq. (2)

// The boost ceiling for normalizeGainDb (issue #53): -1 dB of headroom
// against inter-sample overs, until a true-peak meter measures them exactly.
inline constexpr double kPeakCeilingDb = -1.0;

// A track already within this much of the target is left alone: rewriting
// every sample for a fraction of an LU nobody can hear would spend bytes,
// a trash copy and a pedal write generation on nothing. Shared policy of the
// import path and the on-card normalize command.
inline constexpr double kAlreadyAtTargetLu = 0.2;

// Honest audio never leaves [-8, +8]: float32 masters peak a little over 1,
// and nothing musical is 18 dB past that. A sample beyond it — or not a
// number at all — is bytes that are not audio. Seen on hardware 2026-09-02:
// a card rebuilt after a recovery carried foreign RIFF headers inside five
// takes' data chunks, and the meter read them as 2.4e38 ("767 dB"). The
// meter counts such samples so callers can refuse to act on garbage instead
// of computing a "gain" from it.
inline constexpr float kWildSampleThreshold = 8.0f;

// ITU-R BS.1770-4 gated integrated loudness over a stereo float stream.
// Feed interleaved frames in any chunking; read integratedLufs() at the end.
class Meter {
public:
    // The sample rate must make the 100 ms sub-block an integral number of
    // frames — true of every rate the app meets (the pedal's world is
    // 44100 Hz, wav::kSampleRate).
    explicit Meter(int sampleRate)
    {
        if (sampleRate <= 0 || sampleRate % 10 != 0)
            throw Error("loudness meter needs a positive sample rate divisible by 10 "
                        "(100 ms gating sub-blocks), got "
                        + std::to_string(sampleRate));
        subFrames_ = static_cast<std::size_t>(sampleRate) / 10;

        // K-weighting stage 1: high shelf. Analog prototype constants from
        // the libebur128 re-derivation of the BS.1770 filter; bilinear
        // transform at our rate.
        const double pi = std::numbers::pi;
        {
            const double f0 = 1681.974450955533;
            const double g = 3.999843853973347;
            const double q = 0.7071752369554196;
            const double k = std::tan(pi * f0 / sampleRate);
            const double vh = std::pow(10.0, g / 20.0);
            const double vb = std::pow(vh, 0.4996667741545416);
            const double a0 = 1.0 + k / q + k * k;
            shelf_ = { (vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0,
                       (vh - vb * k / q + k * k) / a0, 2.0 * (k * k - 1.0) / a0,
                       (1.0 - k / q + k * k) / a0 };
        }
        // K-weighting stage 2: the RLB high-pass. The numerator {1, -2, 1} is
        // deliberately unnormalized — exactly as the spec's 48 kHz table
        // prints it (the response is calibrated by kCalibrationDb, not here).
        {
            const double f0 = 38.13547087602444;
            const double q = 0.5003270373238773;
            const double k = std::tan(pi * f0 / sampleRate);
            const double a0 = 1.0 + k / q + k * k;
            highpass_ = { 1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0,
                          (1.0 - k / q + k * k) / a0 };
        }
    }

    void process(const float* interleavedStereo, std::size_t frames)
    {
        for (std::size_t i = 0; i < frames; ++i) {
            const float left = interleavedStereo[2 * i];
            const float right = interleavedStereo[2 * i + 1];
            const float absLeft = std::abs(left), absRight = std::abs(right);
            // `!(a <= t)` is true for NaN as well as for the merely absurd.
            if (!(absLeft <= kWildSampleThreshold))
                ++wild_;
            if (!(absRight <= kWildSampleThreshold))
                ++wild_;
            peak_ = std::max(peak_, std::max(absLeft, absRight));

            const double kl = step(highpass_, hpState_[0],
                                   step(shelf_, shelfState_[0], static_cast<double>(left)));
            const double kr = step(highpass_, hpState_[1],
                                   step(shelf_, shelfState_[1], static_cast<double>(right)));
            subSum_ += kl * kl + kr * kr;

            if (++subFill_ == subFrames_) {
                subSums_.push_back(subSum_);
                subSum_ = 0.0;
                subFill_ = 0;
            }
        }
    }

    // Gated integrated loudness (LUFS). Empty when nothing is measurable:
    // less audio than one 400 ms block, or no block above the -70 LUFS
    // absolute gate (digital silence, or signal too quiet to meter). The
    // caller decides what "unmeasurable" means for its operation — the meter
    // does not guess.
    std::optional<double> integratedLufs() const
    {
        if (subSums_.size() < kSubBlocksPerBlock)
            return std::nullopt;

        // Block power = sum over both channels of the per-channel mean square
        // (channel weights 1.0), i.e. total energy / frames-per-block.
        const double blockFrames = static_cast<double>(kSubBlocksPerBlock * subFrames_);
        std::vector<double> powers;
        powers.reserve(subSums_.size() - (kSubBlocksPerBlock - 1));
        for (std::size_t j = 0; j + kSubBlocksPerBlock <= subSums_.size(); ++j) {
            double energy = 0.0;
            for (std::size_t s = 0; s < kSubBlocksPerBlock; ++s)
                energy += subSums_[j + s];
            powers.push_back(energy / blockFrames);
        }

        const auto lufsOf = [](double power) { return kCalibrationDb + 10.0 * std::log10(power); };
        const double absoluteGatePower
            = std::pow(10.0, (kAbsoluteGateLufs - kCalibrationDb) / 10.0);

        double gatedSum = 0.0;
        std::size_t gatedCount = 0;
        for (const double p : powers) {
            if (p > absoluteGatePower) {
                gatedSum += p;
                ++gatedCount;
            }
        }
        if (gatedCount == 0)
            return std::nullopt;

        // Relative gate: -10 LU below the loudness of the absolutely-gated
        // mean. In the power domain that is exactly mean / 10^(10/10).
        const double relativeGatePower
            = (gatedSum / static_cast<double>(gatedCount)) / std::pow(10.0, kRelativeGateLu / 10.0);

        double sum = 0.0;
        std::size_t count = 0;
        for (const double p : powers) {
            if (p > absoluteGatePower && p > relativeGatePower) {
                sum += p;
                ++count;
            }
        }
        if (count == 0) // unreachable in exact math (max >= mean), kept for totality
            return std::nullopt;
        return lufsOf(sum / static_cast<double>(count));
    }

    // Largest |sample| seen across both channels, unweighted — the v1 peak
    // meter behind the boost ceiling.
    float samplePeak() const { return peak_; }

    // Samples that cannot be audio (see kWildSampleThreshold). Non-zero means
    // the loudness and peak above describe bytes, not music — act on that
    // first.
    std::int64_t wildSamples() const { return wild_; }

private:
    struct Biquad {
        double b0, b1, b2, a1, a2;
    };
    struct BiquadState {
        double z1 = 0.0, z2 = 0.0;
    };

    // Transposed direct form II — one state pair per channel per stage.
    static double step(const Biquad& c, BiquadState& s, double x)
    {
        const double y = c.b0 * x + s.z1;
        s.z1 = c.b1 * x - c.a1 * y + s.z2;
        s.z2 = c.b2 * x - c.a2 * y;
        return y;
    }

    Biquad shelf_ {}, highpass_ {};
    BiquadState shelfState_[2] {}, hpState_[2] {};
    std::size_t subFrames_ = 0; // frames per 100 ms sub-block
    std::size_t subFill_ = 0;   // frames accumulated into the current sub-block
    double subSum_ = 0.0;       // energy of the current sub-block, both channels
    std::vector<double> subSums_;
    float peak_ = 0.0f;
    std::int64_t wild_ = 0;
};

// The one constant gain (dB) that takes a track from its measured loudness to
// the target — rsgain/loudgain clipping semantics:
//   * a cut (gain <= 0) applies in full, unconditionally;
//   * a boost is capped so samplePeak lands at or below ceilingDb;
//   * the cap floors at zero — it never turns a boost into a cut, so a track
//     that already peaks above the ceiling is left as loud as it was, not
//     "rescued" uninvited.
inline double normalizeGainDb(double measuredLufs, double targetLufs, float samplePeak,
                              double ceilingDb)
{
    const double gain = targetLufs - measuredLufs;
    if (gain <= 0.0)
        return gain;
    if (!(samplePeak > 0.0f))
        throw Error("normalize boost needs a positive sample peak; a measurable track "
                    "cannot be all zeros — measure and peak must come from the same pass");
    const double headroom = ceilingDb - 20.0 * std::log10(static_cast<double>(samplePeak));
    return std::min(gain, std::max(headroom, 0.0));
}

} // namespace loopercat::loudness
