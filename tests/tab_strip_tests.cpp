// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The tab strip's lead column (#145), driven without a mouse, from what the
// strip promises:
//
//   - built with a lead column, it keeps that width ahead of its tabs
//     whether or not a label is set, and the k-th tab starts past it
//   - a point inside the lead column names no tab: pressing there changes
//     nothing and tells nobody; a point just past it names the first tab
//   - what is drawn and what is hit agree at both edges of every tab
//   - a label that would not fit the column is refused, and the strip
//     keeps the label it had; every label the pedal can need, SLOT 01 to
//     SLOT 99, fits (99 memories, RC-5 Owner's Manual)
//   - a label with a line break is refused: the width is measured on one
//     line, and a second line would hide the number
//   - built plain (the Settings dialog), the first tab starts at the left
//     edge, the hit test is x over the tab width, and no label can be set
//   - select() out of range or of the live tab tells nobody
//
// A press is the strip's hit test followed by select(), which is all its
// mouseDown does: a synthetic MouseEvent needs juce::Desktop, which counts
// the screens and has no place in a headless suite.

#include "support.hpp"

#include "../app/TabStrip.h"

#include <string>
#include <vector>

using namespace loopercat;

namespace {

void press(TabStrip& strip, int x) { strip.select(strip.tabAt(x)); }

std::vector<juce::Rectangle<int>> boundsOf(const TabStrip& strip, int tabs)
{
    std::vector<juce::Rectangle<int>> out;
    for (int i = 0; i < tabs; ++i)
        out.push_back(strip.tabBounds(i));
    return out;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    constexpr int kTabs = 4;
    constexpr int kHeight = 26; // the studio's strip (MainComponent::resized)
    const juce::StringArray titles { "Audio", "Properties", "Rhythm", "History" };

    // --- a lead column: the tabs start past it, a point in it names no tab ---
    {
        TabStrip strip(titles, TabStrip::Lead::label);
        strip.setSize(600, kHeight);
        int fired = 0, last = -1;
        strip.onTabChanged = [&](int index) { ++fired; last = index; };

        for (int k = 0; k < kTabs; ++k) {
            const auto tab = strip.tabBounds(k);
            CHECK_EQ(tab.getX(), TabStrip::kLeadWidth + k * TabStrip::kTabWidth);
            CHECK_EQ(tab.getWidth(), TabStrip::kTabWidth);
            CHECK_EQ(tab.getHeight(), kHeight);
            // Drawn and hit agree at both edges.
            CHECK_EQ(strip.tabAt(tab.getX()), k);
            CHECK_EQ(strip.tabAt(tab.getRight() - 1), k);
            CHECK_EQ(strip.tabAt(tab.getRight()), k + 1 < kTabs ? k + 1 : -1);
        }
        CHECK_EQ(strip.tabAt(0), -1);
        CHECK_EQ(strip.tabAt(TabStrip::kLeadWidth / 2), -1);
        CHECK_EQ(strip.tabAt(TabStrip::kLeadWidth - 1), -1);
        CHECK_EQ(strip.tabAt(TabStrip::kLeadWidth), 0);
        CHECK_EQ(strip.tabAt(-1), -1);
        CHECK_EQ(strip.tabAt(TabStrip::kLeadWidth + kTabs * TabStrip::kTabWidth + 500), -1);

        // Pressing the label: the live tab stays, nobody hears of a change.
        strip.select(1);
        CHECK_EQ(fired, 1);
        CHECK_EQ(last, 1);
        press(strip, 0);
        press(strip, TabStrip::kLeadWidth / 2);
        press(strip, TabStrip::kLeadWidth - 1);
        CHECK_EQ(strip.selected(), 1);
        CHECK_EQ(fired, 1);
        // Just past it is the first tab.
        press(strip, TabStrip::kLeadWidth);
        CHECK_EQ(strip.selected(), 0);
        CHECK_EQ(fired, 2);
        CHECK_EQ(last, 0);
        press(strip, TabStrip::kLeadWidth); // the live tab again: no change
        CHECK_EQ(fired, 2);
        press(strip, TabStrip::kLeadWidth + 3 * TabStrip::kTabWidth + 5);
        CHECK_EQ(strip.selected(), 3);
        CHECK_EQ(fired, 3);
        CHECK_EQ(last, 3);
        // Past the last tab: nothing.
        press(strip, TabStrip::kLeadWidth + kTabs * TabStrip::kTabWidth);
        CHECK_EQ(strip.selected(), 3);
        CHECK_EQ(fired, 3);
    }

    // --- the label comes and goes; the tabs do not move, nobody is told ---
    {
        TabStrip strip(titles, TabStrip::Lead::label);
        strip.setSize(600, kHeight);
        int fired = 0;
        strip.onTabChanged = [&](int) { ++fired; };
        strip.select(2);
        CHECK_EQ(fired, 1);

        const auto before = boundsOf(strip, kTabs);
        CHECK(strip.leadingLabel().isEmpty());
        strip.setLeadingLabel("SLOT 09");
        CHECK_EQ(strip.leadingLabel(), juce::String("SLOT 09"));
        CHECK(boundsOf(strip, kTabs) == before);
        CHECK_EQ(strip.tabAt(TabStrip::kLeadWidth - 1), -1);
        CHECK_EQ(strip.tabAt(TabStrip::kLeadWidth), 0);
        strip.setLeadingLabel("SLOT 10");
        CHECK_EQ(strip.leadingLabel(), juce::String("SLOT 10"));
        strip.setLeadingLabel(juce::String());
        CHECK(strip.leadingLabel().isEmpty());
        CHECK(boundsOf(strip, kTabs) == before);
        CHECK_EQ(strip.selected(), 2);
        CHECK_EQ(fired, 1);

        // Every label the pedal can need fits, whatever this platform's font
        // makes of each digit: a font that refuses one fails here, not in
        // the app.
        for (int slot = 1; slot <= 99; ++slot) {
            const juce::String label = "SLOT " + juce::String(slot).paddedLeft('0', 2);
            try {
                strip.setLeadingLabel(label);
                CHECK_EQ(strip.leadingLabel(), label);
            } catch (const Error& e) {
                testkit::fail(label.toStdString() + " refused: " + e.what(), __FILE__, __LINE__);
            }
        }
        CHECK(boundsOf(strip, kTabs) == before);
    }

    // --- a label wider than the column is refused, not cut ---
    {
        TabStrip strip(titles, TabStrip::Lead::label);
        strip.setSize(600, kHeight);
        strip.setLeadingLabel("SLOT 12");
        const auto before = boundsOf(strip, kTabs);
        CHECK_THROWS(strip.setLeadingLabel("SLOT 12 and the name of its loop"), "does not fit");
        CHECK_EQ(strip.leadingLabel(), juce::String("SLOT 12"));
        CHECK(boundsOf(strip, kTabs) == before);
        CHECK_EQ(strip.tabAt(TabStrip::kLeadWidth), 0);

        // A line break is refused too, wherever it sits: the first line
        // alone would pass the width check, and the number would paint out
        // of sight.
        CHECK_THROWS(strip.setLeadingLabel("SLOT\n12"), "line break");
        CHECK_THROWS(strip.setLeadingLabel("SLOT 12\n"), "line break");
        CHECK_THROWS(strip.setLeadingLabel("\nSLOT 12"), "line break");
        CHECK_THROWS(strip.setLeadingLabel("SLOT\r\n12"), "line break");
        CHECK_THROWS(strip.setLeadingLabel("SLOT 12\r"), "line break");
        CHECK_THROWS(strip.setLeadingLabel("\n"), "line break");
        CHECK_EQ(strip.leadingLabel(), juce::String("SLOT 12"));
        CHECK(boundsOf(strip, kTabs) == before);
    }

    // --- plain (the Settings dialog): the first tab starts at the edge ---
    {
        TabStrip strip({ "Audio", "Columns", "Import", "History" });
        strip.setSize(600, kHeight);
        int fired = 0;
        strip.onTabChanged = [&](int) { ++fired; };
        for (int k = 0; k < kTabs; ++k) {
            CHECK_EQ(strip.tabBounds(k).getX(), k * TabStrip::kTabWidth);
            CHECK_EQ(strip.tabBounds(k).getWidth(), TabStrip::kTabWidth);
            CHECK_EQ(strip.tabAt(k * TabStrip::kTabWidth), k);
            CHECK_EQ(strip.tabAt((k + 1) * TabStrip::kTabWidth - 1), k);
        }
        CHECK_EQ(strip.tabAt(kTabs * TabStrip::kTabWidth), -1);
        CHECK_EQ(strip.tabAt(-1), -1);
        strip.select(3);
        press(strip, 0);
        CHECK_EQ(strip.selected(), 0);
        CHECK_EQ(fired, 2);
        CHECK_THROWS(strip.setLeadingLabel("SLOT 01"), "no lead column");
        CHECK(strip.leadingLabel().isEmpty());
        CHECK_EQ(strip.tabBounds(0).getX(), 0);
        CHECK_EQ(strip.tabAt(0), 0);
    }

    // --- select(): out of range or the live tab tells nobody ---
    {
        TabStrip strip(titles, TabStrip::Lead::label);
        int fired = 0, last = -1;
        strip.onTabChanged = [&](int index) { ++fired; last = index; };
        CHECK_EQ(strip.selected(), 0);
        strip.select(0); // already live
        strip.select(-1);
        strip.select(kTabs);
        strip.select(1000);
        CHECK_EQ(fired, 0);
        CHECK_EQ(strip.selected(), 0);
        strip.select(2);
        CHECK_EQ(fired, 1);
        CHECK_EQ(last, 2);
        strip.select(2);
        CHECK_EQ(fired, 1);
    }

    return testkit::summary("tab_strip_tests");
}
