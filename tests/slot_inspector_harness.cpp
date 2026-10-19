// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless harness for the Properties studio after Start & Stop was hidden
// (0.9.6): the REAL SlotInspector, laid out and painted the way JUCE lays
// out and paints it — no test-only seam.
//
//   - the strip carries two cards, the pedal's own words for them: Play
//     Count-In and One Shot, side by side, both inside the pane
//   - nothing of Start & Stop is in the inspector: no card, and none of its
//     three choices (startMode, stopMode, fadeTime) anywhere in the tree
//   - and nothing is PAINTED where the third card used to be: the pane's
//     own background, every pixel of it, measured against a corner of the
//     pane itself rather than a colour typed in here
//   - the two cards that stayed are painted, and still report their clicks
//   - identity stayed too: the name and the tempo fields are there

#include "support.hpp"

#include "../app/CardPermissions.h"
#include "../app/SlotInspector.h"

#include <loopercat/Catalog.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

using namespace loopercat;

namespace {

template <typename T>
std::vector<T*> childrenOfType(juce::Component& root)
{
    std::vector<T*> found;
    for (auto* child : root.getChildren())
        if (auto* typed = dynamic_cast<T*>(child))
            found.push_back(typed);
    return found;
}

// A component anywhere under this one carrying that id. Component's own
// findChildWithID looks one level down, and the choices this suite must not
// find would be two.
juce::Component* findInTree(juce::Component& root, const juce::String& id)
{
    for (auto* child : root.getChildren()) {
        if (child->getComponentID() == id)
            return child;
        if (auto* deeper = findInTree(*child, id))
            return deeper;
    }
    return nullptr;
}

int comboBoxesInTree(juce::Component& root)
{
    int found = 0;
    for (auto* child : root.getChildren())
        found += (dynamic_cast<juce::ComboBox*>(child) != nullptr ? 1 : 0)
            + comboBoxesInTree(*child);
    return found;
}

// Whether the whole rectangle is one colour: the pane with nothing on it.
bool uniform(const juce::Image& shot, juce::Rectangle<int> area, juce::Colour colour)
{
    for (int y = area.getY(); y < area.getBottom(); ++y)
        for (int x = area.getX(); x < area.getRight(); ++x)
            if (shot.getPixelAt(x, y) != colour)
                return false;
    return true;
}

bool anyPixelOtherThan(const juce::Image& shot, juce::Rectangle<int> area, juce::Colour colour)
{
    return !uniform(shot, area, colour);
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceRuntime;

    SlotInspector inspector;
    inspector.permissions = [] { return CardPermissions::of(profile::kRc5.familyName); };

    // A real slot, read out of the format's synthetic file.
    const auto slots = catalog::listSlots(testkit::syntheticMemoryText());
    CHECK(!slots.empty());
    if (slots.empty())
        return testkit::summary("slot_inspector_harness");
    SlotRow row { slots.front(), "", "", { "" }, std::nullopt };
    inspector.setSlot(&row);
    inspector.setSize(900, 160); // the studio's strip (MainComponent::resized)

    // --- two cards, the pedal's own words, side by side ---
    const auto cards = childrenOfType<UseCaseCard>(inspector);
    CHECK_EQ(cards.size(), std::size_t { 2 });
    CHECK(childrenOfType<PlayStopCard>(inspector).empty());
    for (const char* id : { "startMode", "stopMode", "fadeTime" })
        CHECK(findInTree(inspector, id) == nullptr);
    CHECK_EQ(comboBoxesInTree(inspector), 0); // the studio has no choice boxes left

    if (cards.size() != 2)
        return testkit::summary("slot_inspector_harness");
    UseCaseCard* countIn = cards[0]->title() == "Play Count-In" ? cards[0] : cards[1];
    UseCaseCard* oneShot = cards[0] == countIn ? cards[1] : cards[0];
    CHECK_EQ(countIn->title(), juce::String("Play Count-In"));
    CHECK_EQ(oneShot->title(), juce::String("One Shot"));

    const auto left = countIn->getBounds();
    const auto right = oneShot->getBounds();
    CHECK(!left.isEmpty() && !right.isEmpty());
    CHECK(left.getRight() <= right.getX());          // side by side, not stacked
    CHECK_EQ(left.getHeight(), right.getHeight());
    CHECK(inspector.getLocalBounds().contains(left));
    CHECK(inspector.getLocalBounds().contains(right));
    CHECK(countIn->isVisible() && oneShot->isVisible());

    // --- painted: the two cards are there, the third one's place is bare ---
    juce::Image shot(juce::Image::ARGB, inspector.getWidth(), inspector.getHeight(), true);
    {
        juce::Graphics g(shot);
        inspector.paintEntireComponent(g, false);
    }
    // The pane's own background, read off the pane: below the cards nothing
    // is ever drawn, so that corner IS the background.
    const juce::Colour background = shot.getPixelAt(inspector.getWidth() - 2,
                                                    inspector.getHeight() - 2);
    CHECK(anyPixelOtherThan(shot, left, background));
    CHECK(anyPixelOtherThan(shot, right, background));
    // Where a third card of the same size would have sat, the whole way to
    // the pane's padding: bare.
    const juce::Rectangle<int> third(right.getRight() + 1, right.getY(),
                                     inspector.getWidth() - right.getRight() - 2,
                                     right.getHeight());
    CHECK(third.getWidth() > 0);
    CHECK(uniform(shot, third, background));

    // --- the cards that stayed still work ---
    int countIns = 0, oneShots = 0;
    inspector.onCountInToggled = [&](int) { ++countIns; };
    inspector.onOneShotToggled = [&](int) { ++oneShots; };
    const auto clickOn = [](juce::Component& target) {
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(),
                                     { 4.0f, 4.0f }, juce::ModifierKeys::leftButtonModifier, 1.0f,
                                     0.0f, 0.0f, 0.0f, 0.0f, &target, &target, now, { 4.0f, 4.0f },
                                     now, 1, false);
        target.mouseDown(event);
    };
    clickOn(*countIn);
    clickOn(*oneShot);
    CHECK_EQ(countIns, 1);
    CHECK_EQ(oneShots, 1);

    // --- identity did not move ---
    CHECK_EQ(childrenOfType<juce::TextEditor>(inspector).size(), std::size_t { 2 });

    return testkit::summary("slot_inspector_harness");
}
