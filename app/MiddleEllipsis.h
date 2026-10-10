// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <vector>

//==============================================================================
// loopercat::elideMiddle — fit a sentence into a width by cutting its middle.
//
// A detail cut at its end loses the part that matters: "already peaking at
// the -1 dBTP ceiling (measured -19.2 LUFS), nothing to..." (#143). The cut
// goes in the middle instead — one ellipsis put in for as little as has to
// go, an odd character going to the tail. "One ellipsis" means one inserted:
// a sentence that carries its own keeps it, cut or not.
//
// The cut lands between grapheme clusters, never inside one: a decomposed
// accent stays on its letter, a joined emoji stays whole, a flag keeps both
// its letters — take names from macOS often come decomposed, and a cluster
// cut in two is valid UTF-8 that renders wrong. The clusters are UAX #29's
// (Unicode Text Segmentation) as far as names and sentences go: grapheme::.
//
// Pure: how wide a string is comes from `widthOf` (the font, at the painting
// site), so the fit can be proven without a font. Every candidate is
// measured, so what comes back has been seen to fit, kerning included — or
// is the ellipsis alone, when not even that fits.
//==============================================================================
namespace loopercat
{

namespace grapheme
{
constexpr juce::juce_wchar kZeroWidthJoiner = 0x200D; // U+200D ZWJ: glues emoji into one picture

// A character that stays with the one before it (UAX #29 rules GB9, GB9a:
// Extend and ZWJ). The joiner's other side stays too — the character after
// it — which clusterStarts handles.
inline bool extendsCluster(juce::juce_wchar c)
{
    return (c >= 0x0300 && c <= 0x036F)   // combining diacritical marks: a decomposed accent
        || (c >= 0x20D0 && c <= 0x20FF)   // combining marks for symbols
        || (c >= 0xFE00 && c <= 0xFE0F)   // variation selectors: emoji presentation
        || (c >= 0x1F3FB && c <= 0x1F3FF) // emoji skin-tone modifiers
        || c == kZeroWidthJoiner;
}

// A regional indicator letter; two in a row are one flag (UAX #29 GB12, GB13).
inline bool isRegionalIndicator(juce::juce_wchar c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }

// Where the clusters start, as character indexes: 0 first, and the text's
// length last to close the list, so every whole-cluster head ends at an
// entry and every whole-cluster tail starts at one.
inline std::vector<int> clusterStarts(const juce::String& text)
{
    std::vector<int> starts;
    juce::juce_wchar previous = 0;
    int indicatorsInARow = 0;
    int index = 0;
    for (const juce::juce_wchar c : text) {
        const bool secondOfFlag = isRegionalIndicator(c) && indicatorsInARow % 2 == 1;
        const bool joined = index > 0
            && (extendsCluster(c) || previous == kZeroWidthJoiner || secondOfFlag);
        if (!joined)
            starts.push_back(index);
        indicatorsInARow = isRegionalIndicator(c) ? indicatorsInARow + 1 : 0;
        previous = c;
        ++index;
    }
    starts.push_back(index);
    return starts;
}
} // namespace grapheme

inline juce::String elideMiddle(const juce::String& text, int width,
                                const std::function<int(const juce::String&)>& widthOf)
{
    if (width < 0)
        throw Error("a width to fit into is not negative");
    if (text.isEmpty() || widthOf(text) <= width)
        return text;

    const juce::String ellipsis = juce::String::fromUTF8("\xe2\x80\xa6"); // U+2026, one character
    const int length = text.length();
    const std::vector<int> starts = grapheme::clusterStarts(text);
    // `kept` characters stay, split head and tail with an odd one to the
    // tail; then each side pulls back to the cluster it would have cut into.
    const auto cut = [&text, &ellipsis, &starts, length](int kept) {
        const int head = kept / 2;
        const int tail = kept - head;
        const int headEnd = *std::prev(std::upper_bound(starts.begin(), starts.end(), head));
        const int tailStart = *std::lower_bound(starts.begin(), starts.end(), length - tail);
        return text.substring(0, headEnd) + ellipsis + text.substring(tailStart);
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
