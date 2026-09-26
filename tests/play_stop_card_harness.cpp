// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless harness for the Start & Stop card: the REAL PlayStopCard, its
// three choices changed the way JUCE changes them — no test-only seam.
//
//   - the slot's values land on the boxes, with the manual's names, and the
//     lists are the manual's lengths (2, 3, 68)
//   - a choice reports its manual number as ONE Edits with ONE field; a
//     re-selected choice reports nothing
//   - a greyed card is mute whoever reaches its boxes
//   - the fade-time caption says when no fade is set

#include "support.hpp"

#include "../app/PlayStopCard.h"

#include <juce_gui_basics/juce_gui_basics.h>

using namespace loopercat;
namespace playstop = loopercat::usecases::playstop;

namespace {

int fieldsSet(const playstop::Edits& e)
{
    return int(e.startMode.has_value()) + int(e.stopMode.has_value()) + int(e.fadeTime.has_value());
}

juce::Label* labelSaying(juce::Component& root, const juce::String& text)
{
    for (auto* child : root.getChildren())
        if (auto* label = dynamic_cast<juce::Label*>(child))
            if (label->getText() == text)
                return label;
    return nullptr;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceRuntime;

    PlayStopCard card;
    card.setSize(300, 82);
    auto* start = dynamic_cast<juce::ComboBox*>(card.findChildWithID("startMode"));
    auto* stop = dynamic_cast<juce::ComboBox*>(card.findChildWithID("stopMode"));
    auto* fade = dynamic_cast<juce::ComboBox*>(card.findChildWithID("fadeTime"));
    CHECK(start && stop && fade);
    if (!(start && stop && fade))
        return testkit::summary("play_stop_card_harness");

    int calls = 0;
    playstop::Edits last;
    card.onEdit = [&](playstop::Edits edits) { ++calls; last = std::move(edits); };
    const auto choose = [](juce::ComboBox& box, long long number) {
        box.setSelectedId(static_cast<int>(number) + 1, juce::sendNotificationSync);
    };

    // The lists are the manual's.
    CHECK_EQ(start->getNumItems(), 2);
    CHECK_EQ(stop->getNumItems(), 3);
    CHECK_EQ(fade->getNumItems(), 68);
    CHECK_EQ(fade->getItemText(4), "1MEAS");
    CHECK_EQ(fade->getItemText(67), "64MEAS");

    // The factory slot: on the spot both ways, 2MEAS set and unheard.
    card.setValues(playstop::read(testkit::syntheticSlotBody()));
    CHECK_EQ(start->getText(), "IMMEDIATE");
    CHECK_EQ(stop->getText(), "IMMEDIATE");
    CHECK_EQ(fade->getText(), "2MEAS");
    CHECK(labelSaying(card, "FADE TIME (no fade set)") != nullptr);

    // A greyed card is mute, whoever reaches its boxes — and what such a
    // reach left on a box does not outlive the greying: the card shows the
    // slot's own values again the moment it can be touched.
    card.setEnabled(false);
    choose(*start, 1);
    choose(*fade, 10);
    CHECK_EQ(calls, 0);
    card.setEnabled(true);
    CHECK_EQ(start->getText(), "IMMEDIATE");
    CHECK_EQ(fade->getText(), "2MEAS");

    // Each choice reports its manual number, one field per Edits.
    choose(*start, 1);
    CHECK_EQ(calls, 1);
    CHECK(last.startMode.has_value() && *last.startMode == 1);
    CHECK_EQ(fieldsSet(last), 1);
    choose(*stop, 2);
    CHECK_EQ(calls, 2);
    CHECK(last.stopMode.has_value() && *last.stopMode == 2);
    CHECK_EQ(fieldsSet(last), 1);
    choose(*fade, 67);
    CHECK_EQ(calls, 3);
    CHECK(last.fadeTime.has_value() && *last.fadeTime == 67);
    choose(*fade, 0);
    CHECK_EQ(calls, 4);
    CHECK(last.fadeTime.has_value() && *last.fadeTime == 0);

    // What the slot already has is nothing — after the snapshot puts it back.
    card.setValues(playstop::read(testkit::syntheticSlotBody()));
    CHECK_EQ(start->getText(), "IMMEDIATE");
    choose(*start, 0);
    choose(*stop, 0);
    choose(*fade, 5);
    CHECK_EQ(calls, 4);

    // A fade on either end: the caption is plain again.
    {
        const std::string faded = playstop::apply(testkit::syntheticSlotBody(), { .stopMode = 1 });
        card.setValues(playstop::read(faded));
        CHECK_EQ(stop->getText(), "FADE OUT");
        CHECK(labelSaying(card, "FADE TIME") != nullptr);
        CHECK(labelSaying(card, "FADE TIME (no fade set)") == nullptr);
    }
    // LOOP END is not a fade.
    {
        const std::string toEnd = playstop::apply(testkit::syntheticSlotBody(), { .stopMode = 2 });
        card.setValues(playstop::read(toEnd));
        CHECK_EQ(stop->getText(), "LOOP END");
        CHECK(labelSaying(card, "FADE TIME (no fade set)") != nullptr);
    }

    return testkit::summary("play_stop_card_harness");
}
