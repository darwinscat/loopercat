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
//
// and the tab over it, which judges nothing:
//
//   - before the owner says anything, the button is dead and silent
//   - the button's words never change; only whether it can be pressed
//   - a press reaches the owner exactly once, and a dead button reaches
//     nobody however the press arrives

#include "support.hpp"

#include "../app/MaintenancePanel.h"

#include <string>

using namespace loopercat;

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const std::string writable { profile::kRc5.familyName };
    const std::string readOnly { profile::kRc500.familyName };
    // The profile table is the premise of every "no" below: the sweep writes,
    // and only one model is open for writes.
    CHECK(CardPermissions::of(writable).anyWrite());
    CHECK(!CardPermissions::of(readOnly).anyWrite());

    // --- no pedal ---
    {
        const auto offer = maintenance::offer({ .connected = false, .busy = false, .family = writable });
        CHECK(!offer.enabled);
        CHECK_EQ(offer.why, std::string("Connect the pedal to clean its card."));
    }

    // --- away and busy at once: away is the first thing to say ---
    {
        const auto offer = maintenance::offer({ .connected = false, .busy = true, .family = readOnly });
        CHECK(!offer.enabled);
        CHECK_EQ(offer.why, std::string("Connect the pedal to clean its card."));
    }

    // --- busy ---
    {
        const auto offer = maintenance::offer({ .connected = true, .busy = true, .family = writable });
        CHECK(!offer.enabled);
        CHECK_EQ(offer.why, std::string("The pedal is busy. This waits for the job to finish."));
    }

    // --- a card this app only reads, and one whose model is not known ---
    for (const std::string& family : { readOnly, std::string("RC-404"), std::string() }) {
        const auto offer = maintenance::offer({ .connected = true, .busy = false, .family = family });
        CHECK(!offer.enabled);
        CHECK_EQ(offer.why, CardPermissions::writesOnlyTo() + ". This card keeps its junk.");
    }

    // --- ready: on offer, with nothing to explain ---
    {
        const auto offer = maintenance::offer({ .connected = true, .busy = false, .family = writable });
        CHECK(offer.enabled);
        CHECK(offer.why.empty());
    }

    // --- the tab: a face over the offer ---
    {
        int swept = 0;
        MaintenancePanel panel;
        panel.setSize(496, 300); // the Settings dialog's area under its tabs
        panel.onCleanJunk = [&] { ++swept; };

        // Nothing said yet: dead and silent, and a press reaches nobody.
        CHECK_EQ(panel.offersShown(), 0);
        CHECK(!panel.cleanEnabled());
        CHECK(panel.whyLine().isEmpty());
        CHECK_EQ(panel.cleanButtonText(), juce::String(MaintenancePanel::kCleanJunk));
        panel.cleanJunk();
        CHECK_EQ(swept, 0);

        const auto refused =
            maintenance::offer({ .connected = true, .busy = true, .family = writable });
        panel.show(refused);
        CHECK_EQ(panel.offersShown(), 1);
        CHECK(!panel.cleanEnabled());
        CHECK_EQ(panel.whyLine(), juce::String::fromUTF8(refused.why.c_str()));
        CHECK_EQ(panel.cleanButtonText(), juce::String(MaintenancePanel::kCleanJunk));
        panel.cleanJunk(); // refused, so nobody hears of it
        CHECK_EQ(swept, 0);

        const auto ready =
            maintenance::offer({ .connected = true, .busy = false, .family = writable });
        panel.show(ready);
        CHECK_EQ(panel.offersShown(), 2);
        CHECK(panel.cleanEnabled());
        CHECK(panel.whyLine().isEmpty());
        CHECK_EQ(panel.cleanButtonText(), juce::String(MaintenancePanel::kCleanJunk));

        // The press the owner hears, once per press.
        panel.cleanJunk();
        CHECK_EQ(swept, 1);

        // And the pedal goes away again: the offer with it.
        panel.show(maintenance::offer({ .connected = false, .busy = false, .family = writable }));
        CHECK(!panel.cleanEnabled());
        CHECK(!panel.whyLine().isEmpty());
        panel.cleanJunk();
        CHECK_EQ(swept, 1);
    }

    return testkit::summary("maintenance_tests");
}
