// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The single-slot Normalize around its plan (#142), from the THEORY of the
// action, never from its code. What must hold, and what these try to break:
//
//   1. the target the player types is a number in the Settings window or
//      nothing: NaN and the infinities JUCE reads out of "nan" and "inf" are
//      nothing, like every typo

#include "support.hpp"

#include "../app/TargetLufs.h"

#include <cmath>

using namespace loopercat;

namespace {

bool near(double a, double b, double slack) { return std::abs(a - b) <= slack; }

} // namespace

int main()
{
    // --- 1. the target the player types ---
    {
        const auto parsed = [](const char* text) { return targetlufs::parse(juce::String(text)); };
        CHECK(parsed("-18").has_value() && near(*parsed("-18"), -18.0, 1.0e-12));
        CHECK(parsed("  -14.5 ").has_value() && near(*parsed("  -14.5 "), -14.5, 1.0e-12));
        CHECK(parsed("-30").has_value()); // the window's edges are targets
        CHECK(parsed("-8").has_value());
        CHECK(!parsed("-30.1").has_value());
        CHECK(!parsed("-7.9").has_value());
        CHECK(!parsed("").has_value());    // reads as 0.0
        CHECK(!parsed("loud").has_value()); // reads as 0.0
        // JUCE reads these as numbers that are not numbers: none is a target.
        CHECK(std::isnan(juce::String("nan").getDoubleValue())); // the premise
        for (const char* text : { "nan", "NaN", "-nan", "inf", "-inf", "Inf", "1e999", "-1e999" })
            CHECK(!parsed(text).has_value());
    }

    return testkit::summary("normalize_step_tests");
}
