// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>

#include <optional>

//==============================================================================
// loopercat::targetlufs — the loudness target as the player types it in
// Settings → Import, shared by normalize-on-upload and the on-card Normalize.
//
// The field refuses values outside this window: hotter than -8 leaves no
// headroom against a live band's transients, quieter than -30 buries the loop
// under any stage noise — both are typos, not choices.
//
// JUCE-core only, so it is tested without a window
// (tests/normalize_step_tests.cpp).
//==============================================================================
namespace loopercat::targetlufs
{

inline constexpr double kMinLufs = -30.0, kMaxLufs = -8.0;

// The typed text as a target, or nothing when it is not one. Unparsable text
// reads as 0.0, which sits outside the window like every other non-target.
// JUCE also reads "nan" and "inf": the window is asked in the positive, since
// NaN fails every comparison and "outside" spelled as two of them let it
// through into Settings (review of #142, P1).
inline std::optional<double> parse(const juce::String& text)
{
    const double value = text.trim().getDoubleValue();
    if (!(value >= kMinLufs && value <= kMaxLufs))
        return std::nullopt;
    return value;
}

} // namespace loopercat::targetlufs
