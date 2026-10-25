// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The card sweep after it moved from the Maintenance menu to a Settings tab
// (0.9.6), from the gates it has always had:
//
//   - no pedal, a busy pedal, a card this app only reads: no offer, and the
//     sentence names which of the three it is
//   - a pedal connected, idle, with a card this app writes to: on offer,
//     and nothing to explain
//   - the causes are ordered the way a player meets them: a pedal that is
//     away AND busy is first of all away
//   - a card whose model nobody knows, and a card whose model has not been
//     read yet, are refused like any card this app does not write to
//   - and the sweep follows the profile table rather than a list of its own:
//     a model with ANY write open is offered it, the RC-500 included since
//     its pull, rename, clear and restore were opened (#161)
//
// the reading of the window's snapshot that feeds it:
//
//   - only a lifecycle state of connected counts, and only with no error:
//     a volume that is there and unreadable is not a pedal to sweep
//
// and the tab over it, which judges nothing:
//
//   - before the owner says anything, the button is dead and silent
//   - the button's words never change; only whether it can be pressed
//   - a press reaches the owner exactly once, and a dead button reaches
//     nobody however the press arrives -- through the button a player can
//     actually click, and through the action behind it

#include "support.hpp"

#include "../app/MaintenancePanel.h"

#include <string>

using namespace loopercat;

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const std::string writable { profile::kRc5.familyName };
    const std::string alsoWritable { profile::kRc500.familyName };
    const std::string strange { "RC-404" }; // no card the table knows
    // The profile table is the premise of every answer below: the sweep is a
    // write, so it is offered exactly where the table opens one. Both models
    // the table knows have writes open now; a name it does not know has
    // none, and neither has a card whose model has not been read yet.
    CHECK(CardPermissions::of(writable).anyWrite());
    CHECK(CardPermissions::of(alsoWritable).anyWrite());
    CHECK(!CardPermissions::of(strange).anyWrite());
    CHECK(!CardPermissions::of(std::string()).anyWrite());

    // --- no pedal ---
    {
        const auto offer = maintenance::offer({ .connected = false, .busy = false, .family = writable });
        CHECK(!offer.enabled);
        CHECK_EQ(offer.why, std::string("Connect the pedal to clean its card."));
    }

    // --- away and busy at once: away is the first thing to say ---
    {
        const auto offer = maintenance::offer({ .connected = false, .busy = true, .family = strange });
        CHECK(!offer.enabled);
        CHECK_EQ(offer.why, std::string("Connect the pedal to clean its card."));
    }

    // --- busy, whatever the card is: busy is said before the card's model ---
    for (const std::string& family : { writable, strange }) {
        const auto offer = maintenance::offer({ .connected = true, .busy = true, .family = family });
        CHECK(!offer.enabled);
        CHECK_EQ(offer.why, std::string("The pedal is busy. This waits for the job to finish."));
    }

    // --- a card this app writes nothing to, by name or by silence ---
    for (const std::string& family : { strange, std::string() }) {
        const auto offer = maintenance::offer({ .connected = true, .busy = false, .family = family });
        CHECK(!offer.enabled);
        CHECK_EQ(offer.why, CardPermissions::writesOnlyTo() + ". This card keeps its junk.");
    }

    // --- ready: on offer, with nothing to explain, on either model ---
    for (const std::string& family : { writable, alsoWritable }) {
        const auto offer = maintenance::offer({ .connected = true, .busy = false, .family = family });
        CHECK(offer.enabled);
        CHECK(offer.why.empty());
    }

    // --- what the window reads off its snapshot ---
    {
        using lifecycle::State;
        // Connected and readable: the only pedal a sweep may be offered on.
        const auto live = maintenance::pedalFrom(State::connected, "", false, writable);
        CHECK(live.connected);
        CHECK(maintenance::offer(live).enabled);
        // There and unreadable is not connected, whatever the state says.
        const auto unreadable =
            maintenance::pedalFrom(State::connected, "unreadable card", false, writable);
        CHECK(!unreadable.connected);
        CHECK_EQ(maintenance::offer(unreadable).why,
                 std::string("Connect the pedal to clean its card."));
        // And no other state is a pedal to sweep, error or not.
        for (const State state : { State::disconnected, State::ghost, State::ejecting,
                                   State::ejected })
            CHECK(!maintenance::pedalFrom(state, "", false, writable).connected);
        // The other two facts travel through untouched.
        CHECK(maintenance::pedalFrom(State::connected, "", true, writable).busy);
        CHECK_EQ(maintenance::pedalFrom(State::connected, "", false, strange).family, strange);
    }

    // --- the tab: a face over the offer ---
    {
        int swept = 0;
        MaintenancePanel panel;
        panel.setSize(496, 300); // the Settings dialog's area under its tabs
        panel.onCleanJunk = [&] { ++swept; };

        // The button a player clicks, found the way a player finds it: the
        // one button on the tab. Its onClick is the only path a mouse has.
        juce::TextButton* button = nullptr;
        for (auto* child : panel.getChildren())
            if (auto* candidate = dynamic_cast<juce::TextButton*>(child))
                button = candidate;
        CHECK(button != nullptr);
        const auto click = [&button] {
            if (button != nullptr && button->onClick)
                button->onClick();
        };

        // Nothing said yet: dead and silent, and a press reaches nobody.
        CHECK_EQ(panel.offersShown(), 0);
        CHECK(!panel.cleanEnabled());
        CHECK(panel.whyLine().isEmpty());
        CHECK_EQ(panel.cleanButtonText(), juce::String(MaintenancePanel::kCleanJunk));
        panel.cleanJunk();
        click();
        CHECK_EQ(swept, 0);

        const auto refused =
            maintenance::offer({ .connected = true, .busy = true, .family = writable });
        panel.show(refused);
        CHECK_EQ(panel.offersShown(), 1);
        CHECK(!panel.cleanEnabled());
        CHECK_EQ(panel.whyLine(), juce::String::fromUTF8(refused.why.c_str()));
        CHECK_EQ(panel.cleanButtonText(), juce::String(MaintenancePanel::kCleanJunk));
        panel.cleanJunk(); // refused, so nobody hears of it
        click();           // and the same through the button itself
        CHECK_EQ(swept, 0);

        const auto ready =
            maintenance::offer({ .connected = true, .busy = false, .family = writable });
        panel.show(ready);
        CHECK_EQ(panel.offersShown(), 2);
        CHECK(panel.cleanEnabled());
        CHECK(panel.whyLine().isEmpty());
        CHECK_EQ(panel.cleanButtonText(), juce::String(MaintenancePanel::kCleanJunk));

        // The press the owner hears, once per press -- and the button a
        // player clicks is wired to the same action.
        panel.cleanJunk();
        CHECK_EQ(swept, 1);
        click();
        CHECK_EQ(swept, 2);

        // And the pedal goes away again: the offer with it.
        panel.show(maintenance::offer({ .connected = false, .busy = false, .family = writable }));
        CHECK(!panel.cleanEnabled());
        CHECK(!panel.whyLine().isEmpty());
        panel.cleanJunk();
        click();
        CHECK_EQ(swept, 2);

        // The sentence has somewhere to be read: a reason nobody can see is
        // the greyed button with no reason beside it all over again.
        CHECK(!panel.whyLine().isEmpty());
        for (auto* child : panel.getChildren())
            CHECK(child->getBounds().isEmpty()
                  || panel.getLocalBounds().contains(child->getBounds()));
    }

    return testkit::summary("maintenance_tests");
}
