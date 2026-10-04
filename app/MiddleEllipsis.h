// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <juce_core/juce_core.h>

#include <functional>

//==============================================================================
// loopercat::elideMiddle — fit a sentence into a width by cutting its middle.
//
// A detail cut at its end loses the part that matters: "already peaking at
// the -1 dBTP ceiling (measured -19.2 LUFS), nothing to..." (#143). The cut
// goes in the middle instead — one ellipsis in place of as little as has to
// go, the tail never shorter than the head.
//
// Pure: how wide a string is comes from `widthOf` (the font, at the painting
// site), so the fit can be proven without a font. Every candidate is
// measured, so what comes back has been seen to fit, kerning included — or
// is the ellipsis alone, when not even that fits.
//==============================================================================
namespace loopercat
{

inline juce::String elideMiddle(const juce::String& text, int width,
                                const std::function<int(const juce::String&)>& widthOf)
{
    if (width < 0)
        throw Error("a width to fit into is not negative");
    if (text.isEmpty() || widthOf(text) <= width)
        return text;

    const juce::String ellipsis = juce::String::fromUTF8("\xe2\x80\xa6"); // U+2026, one character
    const int length = text.length();
    // `kept` characters stay, split head and tail; an odd one goes to the tail.
    const auto cut = [&text, &ellipsis, length](int kept) {
        const int head = kept / 2;
        return text.substring(0, head) + ellipsis + text.substring(length - (kept - head));
    };

    // The most that fits, by bisection on how much is kept: wider text for
    // more characters is what every font does, to within a kerning pair.
    int fits = -1;
    for (int low = 0, high = length - 1; low <= high;) {
        const int mid = low + (high - low) / 2;
        if (widthOf(cut(mid)) <= width) {
            fits = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return fits < 0 ? ellipsis : cut(fits);
}

} // namespace loopercat
