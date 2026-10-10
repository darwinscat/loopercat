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

// The typed text as a target, or nothing when it is not one. The whole text
// must be the number — an optional leading minus, digits, at most one point —
// because JUCE reads a number off the front of anything: "-18,5" (a European
// -18.5) would become -18, and "-18 LUFS" would pass for -18 (integration
// review of 0.9.6). JUCE also reads "nan" and "inf"; neither passes the shape,
// and the window is asked in the positive besides, since NaN fails every
// comparison and "outside" spelled as two of them let it through into
// Settings (review of #142, P1).
inline std::optional<double> parse(const juce::String& text)
{
    const juce::String t = text.trim();
    const int body = t.startsWithChar('-') ? 1 : 0;
    const juce::String digits = t.substring(body);
    if (digits.isEmpty() || !digits.containsOnly("0123456789.")
        || digits.indexOfChar('.') != digits.lastIndexOfChar('.')
        || !digits.containsAnyOf("0123456789"))
        return std::nullopt;
    const double value = t.getDoubleValue();
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
    const juce::String where = juce::String::fromUTF8("set it again in Settings \xe2\x86\x92 Import");
    if (stored.trim().isEmpty())
        return "The normalize target in Settings is empty: " + where;
    return "The normalize target in Settings is not a number LooperCat can use (" + stored + "): " + where;
}

} // namespace loopercat::targetlufs
