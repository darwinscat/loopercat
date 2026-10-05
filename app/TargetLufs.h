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
// What the settings file holds is read back through the same rule (#142
// review): a file written by a build whose field let "nan" through, or edited
// by hand, holds text that is not a target, and it is reported as that — no
// target is put in its place.
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

// A target as every label shows it: one decimal, none when it is whole.
inline juce::String format(double lufs)
{
    juce::String s(lufs, 1);
    return s.endsWith(".0") ? s.dropLastCharacters(2) : s;
}

// What the player is told, once per launch and on every Normalize asked for
// meanwhile, when the stored text is not a target.
inline juce::String unusableStored(const juce::String& stored)
{
    return "The normalize target in Settings is not a number LooperCat can use (" + stored
         + juce::String::fromUTF8("): set it again in Settings \xe2\x86\x92 Import");
}

} // namespace loopercat::targetlufs
