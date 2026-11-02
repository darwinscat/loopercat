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
//   - and nothing is PAINTED anywhere the inspector does not show a child:
//     every such pixel is the pane's own background, measured against a
//     corner of the pane rather than a colour typed in here. Not "the band
//     where the third card sat" -- anywhere, because a farewell box in the
//     bottom strip is as much a Start & Stop as a card in its old place
//   - the two cards that stayed are really drawn (most of each card is the
//     card's own fill, not one pixel of difference) and still report their
//     clicks, and the row is as tall as the taller of the two asks
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

// The cards a player can see: a card the memory has no use for (the second
// track's One Shot of a one-track memory) is a hidden child, not a card.
std::vector<UseCaseCard*> visibleCards(juce::Component& root)
{
    std::vector<UseCaseCard*> found;
    for (auto* card : childrenOfType<UseCaseCard>(root))
        if (card->isVisible())
            found.push_back(card);
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

// How much of a rectangle is one colour, in pixels.
int pixelsOf(const juce::Image& shot, juce::Rectangle<int> area, juce::Colour colour)
{
    int found = 0;
    for (int y = area.getY(); y < area.getBottom(); ++y)
        for (int x = area.getX(); x < area.getRight(); ++x)
            if (shot.getPixelAt(x, y) == colour)
                ++found;
    return found;
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
    const auto cards = visibleCards(inspector);
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
    // The row is as tall as the taller card asks -- the hidden third card,
    // which asks for 96, must not be what sets it, and a card clipped short
    // of its orange line must not pass either.
    CHECK_EQ(left.getHeight(),
             juce::jmax(countIn->preferredHeight(), oneShot->preferredHeight()));
    CHECK(inspector.getLocalBounds().contains(left));
    CHECK(inspector.getLocalBounds().contains(right));
    CHECK(countIn->isVisible() && oneShot->isVisible());

    // --- painted: the two cards are there, the third one's place is bare ---
    juce::Image shot(juce::Image::ARGB, inspector.getWidth(), inspector.getHeight(), true);
    {
        juce::Graphics g(shot);
        inspector.paintEntireComponent(g, false);
    }
    // The pane's own background, read off the pane: the corner below the
    // cards is never drawn on. It has to be opaque, or a paint() that stopped
    // filling would leave the image cleared and every check below would be
    // comparing transparency with transparency.
    const juce::Colour background = shot.getPixelAt(inspector.getWidth() - 2,
                                                    inspector.getHeight() - 2);
    CHECK_EQ(static_cast<int>(background.getAlpha()), 255);

    // Each card is really drawn: most of its area is the card's own fill
    // (UseCaseCard::paint), not a pixel's worth of difference.
    const juce::Colour cardFill { 0xff17171d };
    CHECK(pixelsOf(shot, left, cardFill) > left.getWidth() * left.getHeight() / 2);
    CHECK(pixelsOf(shot, right, cardFill) > right.getWidth() * right.getHeight() / 2);

    // And every pixel the inspector does not show a child at is background.
    const auto underAChild = [&inspector](int x, int y) {
        for (auto* child : inspector.getChildren())
            if (child->isVisible() && child->getBounds().contains(x, y))
                return true;
        return false;
    };
    int strayPixels = 0;
    for (int y = 0; y < inspector.getHeight(); ++y)
        for (int x = 0; x < inspector.getWidth(); ++x)
            if (!underAChild(x, y) && shot.getPixelAt(x, y) != background)
                ++strayPixels;
    CHECK_EQ(strayPixels, 0);

    // --- the cards that stayed still work ---
    int countIns = 0, oneShots = 0;
    inspector.onCountInToggled = [&](int) { ++countIns; };
    inspector.onOneShotToggled = [&](int, int) { ++oneShots; };
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

    // --- a memory of two tracks: a One Shot per track ---
    //
    // The flag lives in each TRACK section, so a two-track memory shows one
    // card per track, each with that track's own state, and a click names
    // its track. Track 1 loops and track 2 plays once here, so a card that
    // read the other track's flag shows the wrong state.
    {
        std::string text = testkit::syntheticTwoTrackMemoryText();
        text = rc0::replaceSlotBody(text, 1,
                                    rc0::setSectionField(rc0::slotBody(text, 1), "TRACK2", "One", 1));
        const auto twoTrack = catalog::listSlots(text);
        SlotRow both { twoTrack.front(), "", "", { "" }, std::nullopt };
        inspector.permissions = [] { return CardPermissions::of(profile::kRc500.familyName); };
        inspector.setSlot(&both);
        const auto three = visibleCards(inspector);
        CHECK_EQ(three.size(), std::size_t { 3 });
        UseCaseCard* t1 = nullptr;
        UseCaseCard* t2 = nullptr;
        for (auto* card : three) {
            if (card->title() == "One Shot T1")
                t1 = card;
            if (card->title() == "One Shot T2")
                t2 = card;
        }
        CHECK(t1 != nullptr && t2 != nullptr);
        if (t1 != nullptr && t2 != nullptr) {
            CHECK(!t1->isOn());
            CHECK(t2->isOn());
            CHECK(t1->isEnabled() && t2->isEnabled()); // the RC-500 opens One Shot
            CHECK(t1->getBounds().getRight() <= t2->getBounds().getX()); // side by side
            CHECK(inspector.getLocalBounds().contains(t2->getBounds()));
            std::vector<int> tracks;
            inspector.onOneShotToggled = [&tracks](int slot, int track) {
                CHECK_EQ(slot, 1);
                tracks.push_back(track);
            };
            clickOn(*t2);
            clickOn(*t1);
            CHECK((tracks == std::vector<int> { 2, 1 }));
        }
        // The second track alone recorded: both cards, the flag kept either way.
        SlotRow solo { twoTrack[10], "", "", { "" }, std::nullopt };
        inspector.setSlot(&solo);
        CHECK_EQ(visibleCards(inspector).size(), std::size_t { 3 });
        // A one-track memory again: one One Shot card, its old word.
        inspector.permissions = [] { return CardPermissions::of(profile::kRc5.familyName); };
        inspector.setSlot(&row);
        const auto back = visibleCards(inspector);
        CHECK_EQ(back.size(), std::size_t { 2 });
        for (auto* card : back)
            CHECK(card->title() == "Play Count-In" || card->title() == "One Shot");
    }

    return testkit::summary("slot_inspector_harness");
}
