// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The bottom pane's faces after Rhythm was hidden (0.9.6), driven through
// the very strip the window builds from them:
//
//   - three tabs, in the order the strip paints them: Audio, Properties,
//     History; no tab says Rhythm
//   - the tab at index k shows Face(k), so the number the window selects
//     History by is the index the player clicks
//   - Rhythm's face is past the strip: select() refuses it, no x on the
//     strip names it, and the strip tells nobody about it
//   - History is still reachable by a click, and lands on its own face

#include "support.hpp"

#include "../app/BottomPane.h"
#include "../app/TabStrip.h"

using namespace loopercat;

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::StringArray titles = bottompane::tabTitles();

    // --- three tabs, and Rhythm is not one of them ---
    CHECK_EQ(titles.size(), 3);
    CHECK_EQ(titles[0], juce::String("Audio"));
    CHECK_EQ(titles[1], juce::String("Properties"));
    CHECK_EQ(titles[2], juce::String("History"));
    CHECK(!titles.contains("Rhythm"));

    // --- the tab at index k shows Face(k) ---
    CHECK_EQ(static_cast<int>(bottompane::audio), titles.indexOf("Audio"));
    CHECK_EQ(static_cast<int>(bottompane::properties), titles.indexOf("Properties"));
    CHECK_EQ(static_cast<int>(bottompane::history), titles.indexOf("History"));
    // The hidden face sits past the last tab, where no index can reach it.
    CHECK(static_cast<int>(bottompane::rhythm) >= titles.size());

    // --- the real strip: no click reaches Rhythm, History still answers ---
    TabStrip strip(titles, TabStrip::Lead::label);
    strip.setSize(600, 26); // the studio's strip (MainComponent::resized)
    int fired = 0, last = -1;
    strip.onTabChanged = [&](int index) { ++fired; last = index; };

    // Every x across the whole strip, well past its tabs: no point on it
    // names a fourth tab.
    for (int x = -4; x < 600; ++x)
        CHECK(strip.tabAt(x) <= static_cast<int>(bottompane::history));

    strip.select(bottompane::rhythm);
    CHECK_EQ(fired, 0);
    CHECK_EQ(strip.selected(), static_cast<int>(bottompane::audio));

    // A click on the third tab: History, by the number the window uses.
    strip.select(strip.tabAt(strip.tabBounds(2).getCentreX()));
    CHECK_EQ(fired, 1);
    CHECK_EQ(last, static_cast<int>(bottompane::history));
    CHECK_EQ(strip.selected(), static_cast<int>(bottompane::history));

    return testkit::summary("bottom_pane_tests");
}
