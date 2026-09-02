// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Loudness measurement for the normalize feature (issue #53): ITU-R BS.1770-4
// gated integrated loudness — the measure behind EBU R128 and ReplayGain 2.0,
// whose reference of -18 LUFS is the modern spelling of mp3gain's "89 dB" —
// and the true (inter-sample) peak that bounds how far a loop may be raised.
//
// The meters are the family's, from felitronics-core:
//   * felitronics::analysis::LoudnessMeter — K-weighting recomputed at the
//     meter's own rate from the BS.1770 analog prototype, 400 ms blocks at
//     75 % overlap, the -70 LUFS absolute and -10 LU relative gates of
//     Annex 1;
//   * felitronics::analysis::TruePeakMeter — the waveform between the
//     samples, per Annex 2: 4x oversampled at the pedal's 44.1 kHz, so the
//     overs a DAC reconstructs and a sample meter never sees are measured,
//     not guessed at.
// This header is the product's adapter over them. It speaks the app's
// interleaved stereo stream, adds the one measurement the suite does not
// make (samples that cannot be audio), turns the meters' sentinels into typed
// absence, and owns the policy: the boost ceiling and the gain rule. The EBU
// Tech 3341 conformance suite in tests/loudness_tests runs the family meter
// through this adapter — the gate that let the first release's hand-rolled
// meter go.
//
// The meter is stereo by charter: it measures the exact two-channel stream
// the app writes to the card (channel weights are 1.0 for left and right in
// BS.1770), so every track normalized through this path lands equal — which
// is the whole feature: a setlist with no volume jumps between songs.
//
// The gain rule (normalizeGainDb) is the rsgain/loudgain lineage: a cut
// applies in full; a boost stops where the true peak would cross the
// ceiling, so a quiet-but-peaky track lands short of target instead of
// distorting; and the cap never turns a boost into a cut.

#pragma once

#include "Error.hpp"

#include <felitronics/analysis/LoudnessMeter.h>
#include <felitronics/analysis/TruePeakMeter.h>
#include <felitronics/core/Config.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace loopercat::loudness {

inline constexpr double kAbsoluteGateLufs = -70.0; // BS.1770-4 Annex 1

// The boost ceiling for normalizeGainDb, in dBTP: EBU R128's maximum
// permitted true peak. Measured on the oversampled waveform, so it carries no
// guess-headroom for inter-sample overs — the meter sees them.
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

// The longest program one meter accepts. The gated measure keeps every
// 400 ms block's energy to the end (the relative gate is computed over all of
// them), so the family meter pre-allocates for a stated maximum and past it
// would silently stop recording blocks; this adapter refuses instead, at the
// sample where the program runs over. Four hours covers every file the
// pedal's format can hold — RIFF's 4 GiB ceiling is 3.4 hours of 44.1 kHz
// stereo float32 — for 1.2 MB of block energies.
inline constexpr double kMaxProgramSeconds = 4.0 * 3600.0;

// ITU-R BS.1770-4 gated integrated loudness and true peak over a stereo float
// stream. Feed interleaved frames in any chunking; read the results at the
// end.
class Meter {
public:
    // The sample rate must make the 100 ms gating hop an integral number of
    // frames — true of every rate the app meets (the pedal's world is
    // 44100 Hz, wav::kSampleRate). The family meter would round; this
    // adapter refuses, so a reading is never taken over approximate blocks.
    explicit Meter(int sampleRate, double maxProgramSeconds = kMaxProgramSeconds)
    {
        if (sampleRate <= 0 || sampleRate % 10 != 0)
            throw Error("loudness meter needs a positive sample rate divisible by 10 "
                        "(100 ms gating sub-blocks), got "
                        + std::to_string(sampleRate));
        if (!(maxProgramSeconds > 0.0))
            throw Error("loudness meter needs a positive program capacity in seconds, got "
                        + std::to_string(maxProgramSeconds));
        capacitySeconds_ = maxProgramSeconds;
        capacityFrames_ = static_cast<std::int64_t>(
            std::ceil(maxProgramSeconds * static_cast<double>(sampleRate)));
        loudness_.prepare(static_cast<double>(sampleRate), kChannels, maxProgramSeconds);
        truePeak_.prepare(static_cast<double>(sampleRate), static_cast<int>(kSliceFrames),
                          kChannels);
    }

    // Throws Error the moment the program exceeds the meter's capacity: a
    // reading over a silently truncated program would be a wrong number
    // with a straight face.
    void process(const float* interleavedStereo, std::size_t frames)
    {
        if (framesFed_ + static_cast<std::int64_t>(frames) > capacityFrames_)
            throw Error("audio runs past the loudness meter's capacity of "
                        + std::to_string(std::llround(capacitySeconds_)) + " s");

        // The family meters take planar channels in blocks; the app's stream
        // is interleaved and chunked however the caller pleased. Slices no
        // larger than the family's block bound keep the scratch fixed and
        // the caller's chunking irrelevant to the reading.
        while (frames > 0) {
            const std::size_t n = std::min(frames, kSliceFrames);
            for (std::size_t i = 0; i < n; ++i) {
                const float left = interleavedStereo[2 * i];
                const float right = interleavedStereo[2 * i + 1];
                left_[i] = left;
                right_[i] = right;
                const float absLeft = std::abs(left), absRight = std::abs(right);
                // `!(a <= t)` is true for NaN as well as for the merely absurd.
                if (!(absLeft <= kWildSampleThreshold))
                    ++wild_;
                if (!(absRight <= kWildSampleThreshold))
                    ++wild_;
                peak_ = std::max(peak_, std::max(absLeft, absRight));
            }
            const float* planar[kChannels] = { left_.data(), right_.data() };
            loudness_.process(planar, kChannels, static_cast<int>(n));
            truePeak_.process(planar, kChannels, static_cast<int>(n));
            interleavedStereo += 2 * n;
            frames -= n;
            framesFed_ += static_cast<std::int64_t>(n);
        }
    }

    // Gated integrated loudness (LUFS). Empty when nothing is measurable:
    // less audio than one 400 ms block, or no block above the -70 LUFS
    // absolute gate (digital silence, or signal too quiet to meter). The
    // caller decides what "unmeasurable" means for its operation — the meter
    // does not guess.
    //
    // The family meter answers that state with -120. Any real gated mean lies
    // above the absolute gate by construction — it averages blocks that each
    // passed it — so the gate itself is the line between a reading and the
    // sentinel.
    std::optional<double> integratedLufs() const
    {
        // Belt and braces over the capacity check in process(): the family meter counts the
        // gating blocks it could not keep, and this adapter promised never to let a program
        // reach that point — a non-zero count here is a wrong guard, not a long file.
        if (loudness_.droppedBlocks() != 0)
            throw Error("internal: the loudness meter dropped "
                        + std::to_string(loudness_.droppedBlocks())
                        + " gating block(s) past its capacity — the adapter's capacity guard "
                          "let a program through");
        const double lufs = loudness_.integratedLufs();
        if (lufs <= kAbsoluteGateLufs)
            return std::nullopt;
        return lufs;
    }

    // Largest |sample| seen across both channels, unweighted, exact — the
    // grid's own maximum. The true peak is never below it.
    float samplePeak() const { return peak_; }

    // True peak in dBTP: the largest |sample| of the waveform reconstructed
    // between the grid points (BS.1770-4 Annex 2), across both channels. For
    // digital silence it is -inf — 20·log10(0), the value, not a sentinel.
    double truePeakDb() const { return 20.0 * std::log10(truePeak_.truePeakLinear()); }

    // Samples that cannot be audio (see kWildSampleThreshold). Non-zero means
    // the loudness and peaks above describe bytes, not music — act on that
    // first.
    std::int64_t wildSamples() const { return wild_; }

private:
    static constexpr int kChannels = 2;
    // The family's own bound for block-scoped scratch — the slice the
    // adapter hands its meters at a time.
    static constexpr std::size_t kSliceFrames
        = static_cast<std::size_t>(felitronics::core::kMaxBlockSize);

    felitronics::analysis::LoudnessMeter loudness_;
    felitronics::analysis::TruePeakMeter truePeak_;
    std::vector<float> left_ = std::vector<float>(kSliceFrames);
    std::vector<float> right_ = std::vector<float>(kSliceFrames);
    double capacitySeconds_ = 0.0;
    std::int64_t capacityFrames_ = 0;
    std::int64_t framesFed_ = 0;
    float peak_ = 0.0f;
    std::int64_t wild_ = 0;
};

// The one constant gain (dB) that takes a track from its measured loudness to
// the target — rsgain/loudgain clipping semantics, against the true peak:
//   * a cut (gain <= 0) applies in full, unconditionally;
//   * a boost is capped so peakDb (dBTP) lands at or below ceilingDb;
//   * the cap floors at zero — it never turns a boost into a cut, so a track
//     that already peaks above the ceiling is left as loud as it was, not
//     "rescued" uninvited.
inline double normalizeGainDb(double measuredLufs, double targetLufs, double peakDb,
                              double ceilingDb)
{
    const double gain = targetLufs - measuredLufs;
    if (gain <= 0.0)
        return gain;
    if (!std::isfinite(peakDb))
        throw Error("normalize boost needs a finite peak level; a measurable track "
                    "cannot be silent — measure and peak must come from the same pass");
    const double headroom = ceilingDb - peakDb;
    return std::min(gain, std::max(headroom, 0.0));
}

} // namespace loopercat::loudness
