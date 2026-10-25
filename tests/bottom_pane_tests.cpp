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
//   - and when the window shows the face the strip cannot name, the strip
//     underlines nothing rather than claim the pane is Audio -- after which
//     Audio is clickable again, which it would not be if the strip still
//     thought it was the live tab
//
// What the window does with all this -- building its strip from these
// titles, and turning the tab a player clicked into the face of the same
// number -- is two lines of MainComponent that no headless suite can reach.
// hidden_features_tests anchors both.

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

    // No point on the strip names a fourth tab: the last tab's own edges,
    // the first x past it, and well beyond. (TabStrip's own sweep is
    // tab_strip_tests'; this is about there being three.)
    const auto lastTab = strip.tabBounds(bottompane::history);
    CHECK_EQ(strip.tabAt(lastTab.getX()), static_cast<int>(bottompane::history));
    CHECK_EQ(strip.tabAt(lastTab.getRight() - 1), static_cast<int>(bottompane::history));
    CHECK_EQ(strip.tabAt(lastTab.getRight()), -1);
    CHECK_EQ(strip.tabAt(599), -1);

    strip.select(bottompane::rhythm);
    CHECK_EQ(fired, 0);
    CHECK_EQ(strip.selected(), static_cast<int>(bottompane::audio));

    // A click on the third tab: History, by the number the window uses.
    strip.select(strip.tabAt(strip.tabBounds(2).getCentreX()));
    CHECK_EQ(fired, 1);
    CHECK_EQ(last, static_cast<int>(bottompane::history));
    CHECK_EQ(strip.selected(), static_cast<int>(bottompane::history));

    // --- the face with no tab: the strip underlines nothing, and says so ---
    strip.deselect();
    CHECK_EQ(strip.selected(), -1);
    CHECK_EQ(fired, 1); // deselecting is the owner's own doing, not a change
    // And the tab that was live before is live again on the next click: a
    // strip that still thought History was selected would swallow it.
    strip.select(bottompane::history);
    CHECK_EQ(strip.selected(), static_cast<int>(bottompane::history));
    CHECK_EQ(fired, 2);
    strip.deselect();
    strip.deselect(); // already nothing: tells nobody, changes nothing
    CHECK_EQ(strip.selected(), -1);
    CHECK_EQ(fired, 2);

    return testkit::summary("bottom_pane_tests");
}
