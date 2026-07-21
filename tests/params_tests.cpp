// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The boot-indexing parameter formula against every golden value captured
// from real hardware, plus the rejection edges. Derived from the observed
// behavior spec (see Params.hpp), not from the implementation.

#include "support.hpp"

#include <loopercat/Params.hpp>

using loopercat::params::computeSlotParams;

int main()
{
    const auto& g = testkit::golden();

    // The formula's constants pinned to the hardware-derived fixture — if a
    // constant drifts from what real hardware exhibits, this is the tripwire.
    {
        const auto& c = g.at("constants");
        CHECK_EQ(loopercat::params::kSamplesPerMinute, c.at("samplesPerMinute").get<long long>());
        CHECK_EQ(loopercat::params::kMaxTempoTenths, c.at("maxTempoTenths").get<int>());
        CHECK_EQ(loopercat::params::kMinTempoTenths, c.at("minTempoTenths").get<int>());
        CHECK_EQ(loopercat::params::kMeasureFieldOffset, c.at("measureFieldOffset").get<int>());
    }

    // Every golden value from real hardware, both pedal-authored and
    // pedal-accepted configurations.
    for (const auto& entry : g.at("slotParams")) {
        const auto frames = entry.at("frames").get<long long>();
        const auto p = computeSlotParams(frames);
        CHECK_EQ(p.measures, entry.at("measures").get<int>());
        CHECK_EQ(p.tempoTenths, entry.at("tempoTenths").get<int>());
        CHECK_EQ(p.measureField(),
                 entry.at("measures").get<int>() + g.at("constants").at("measureFieldOffset").get<int>());
    }

    // The golden list must actually exercise the two behavior-proving cases:
    // truncation (144.99 -> 1449) and the inclusive 160.0 boundary. Guards
    // against the fixture silently losing its teeth.
    {
        bool sawTruncation = false, sawBoundary = false;
        for (const auto& entry : g.at("slotParams")) {
            sawTruncation |= entry.at("tempoTenths").get<int>() == 1449;
            sawBoundary |= entry.at("tempoTenths").get<int>() == 1600;
        }
        CHECK(sawTruncation);
        CHECK(sawBoundary);
    }

    // A sample too short for the tempo range throws instead of writing nonsense.
    CHECK_THROWS(computeSlotParams(g.at("outOfRange").at("tooShortFrames").get<long long>()),
                 "too short");

    // A sample too long: even 1 measure implies a below-range tempo. The value
    // (3e9 frames) also forces 64-bit arithmetic — 32-bit math would overflow
    // long before reaching the range check.
    CHECK_THROWS(computeSlotParams(g.at("outOfRange").at("tooLongFrames").get<long long>()),
                 "below the pedal");

    // Invalid frame counts.
    CHECK_THROWS(computeSlotParams(0), "positive");
    CHECK_THROWS(computeSlotParams(-1), "positive");

    return testkit::summary("params");
}
