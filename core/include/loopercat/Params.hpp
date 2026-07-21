// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Slot parameters the RC-5 derives when it indexes a WAV at boot.
//
// Observed behavior, verified against configs the pedal generated itself
// (fixtures/golden.json, "slotParams"): it picks the largest power-of-two
// measure count whose implied tempo stays at or below 160 BPM, computes the
// tempo in tenths of BPM truncated (not rounded), and stores Measure as the
// measure count plus a constant 7. 2646000 is samples-per-minute at 44.1 kHz.

#pragma once

#include "Error.hpp"

#include <cstdint>
#include <string>

namespace loopercat::params {

inline constexpr std::int64_t kSamplesPerMinute = 2646000;
inline constexpr int kMaxTempoTenths = 1600;
inline constexpr int kMinTempoTenths = 200;
inline constexpr int kMeasureFieldOffset = 7;
inline constexpr int kBeatsPerMeasure = 4;
inline constexpr int kMaxMeasures = 4096;

struct SlotParams {
    int measures;
    int tempoTenths;

    // What goes into <Measure>: the measure count plus the constant 7 the
    // pedal always adds (golden.json "measureFieldOffset").
    int measureField() const { return measures + kMeasureFieldOffset; }

    bool operator==(const SlotParams&) const = default;
};

inline SlotParams computeSlotParams(std::int64_t frames)
{
    if (frames <= 0)
        throw Error("frames must be a positive integer, got " + std::to_string(frames));
    for (int measures = kMaxMeasures; measures >= 1; measures /= 2) {
        const std::int64_t beats = std::int64_t{ measures } * kBeatsPerMeasure;
        // Truncating integer division — the pedal truncates, it never rounds
        // (golden.json: 18687375 frames -> 1449, not 1450).
        const std::int64_t tempoTenths = beats * kSamplesPerMinute * 10 / frames;
        if (tempoTenths <= kMaxTempoTenths) {
            if (tempoTenths < kMinTempoTenths)
                throw Error("sample of " + std::to_string(frames) + " frames implies "
                            + std::to_string(tempoTenths / 10) + "." + std::to_string(tempoTenths % 10)
                            + " BPM even at " + std::to_string(measures)
                            + " measures — below the pedal's tempo range");
            return { measures, static_cast<int>(tempoTenths) };
        }
    }
    throw Error("sample of " + std::to_string(frames) + " frames is too short: tempo exceeds "
                + std::to_string(kMaxTempoTenths / 10) + " BPM even at 1 measure");
}

} // namespace loopercat::params
