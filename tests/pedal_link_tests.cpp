// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pedal's MIDI endpoints, from what a bus with two RC-5s looks like —
// measured 2026-09-24, both pedals and an RC-500 attached: the OS lists the
// two RC-5s under ONE name, "BOSS_RC-5", and only their identifiers (the
// CoreMIDI uniqueIDs) differ. The rule this suite exists for: the link hands
// out ALL of them, in the order the OS gave, identifiers intact — never just
// the first, which on that day was the new pedal rather than the main one.

#include "support.hpp"

#include "../app/PedalLink.h"

#include <string>

using namespace loopercat;

int main()
{
    // The bus as CoreMIDI enumerated it (names and uniqueIDs verbatim).
    const juce::Array<juce::MidiDeviceInfo> bus {
        juce::MidiDeviceInfo("Clarett+ 8Pre", "-1825808221"),
        juce::MidiDeviceInfo("BOSS_RC-5", "883557304"),
        juce::MidiDeviceInfo("Quad Cortex", "-360620525"),
        juce::MidiDeviceInfo("BOSS_RC-500", "904629978"),
        juce::MidiDeviceInfo("BOSS_RC-5", "2016199893"),
    };

    const auto pedals = pedallink::pedalsAmong(bus);
    CHECK_EQ(pedals.size(), 2u);
    if (pedals.size() == 2) {
        // Both, in bus order, each with the identifier that tells them apart.
        CHECK_EQ(pedals[0].name.toStdString(), "BOSS_RC-5");
        CHECK_EQ(pedals[0].identifier.toStdString(), "883557304");
        CHECK_EQ(pedals[1].name.toStdString(), "BOSS_RC-5");
        CHECK_EQ(pedals[1].identifier.toStdString(), "2016199893");
        CHECK(pedals[0].identifier != pedals[1].identifier);
        CHECK(pedals[0].name == pedals[1].name);
    }
    // The RC-500 shares the prefix and is not an RC-5.
    for (const auto& pedal : pedals)
        CHECK(pedal.identifier != "904629978");

    // Order is the bus's, whatever it is.
    const juce::Array<juce::MidiDeviceInfo> reversed {
        juce::MidiDeviceInfo("BOSS_RC-5", "2016199893"),
        juce::MidiDeviceInfo("BOSS_RC-5", "883557304"),
    };
    const auto swapped = pedallink::pedalsAmong(reversed);
    CHECK_EQ(swapped.size(), 2u);
    if (swapped.size() == 2)
        CHECK_EQ(swapped[0].identifier.toStdString(), "2016199893");

    // No pedal, or only other RC models: nothing.
    CHECK(pedallink::pedalsAmong({}).empty());
    CHECK(pedallink::pedalsAmong({ juce::MidiDeviceInfo("BOSS_RC-500", "1"),
                                   juce::MidiDeviceInfo("BOSS_RC-505", "2") })
              .empty());

    // One pedal is one pedal.
    const auto one = pedallink::pedalsAmong({ juce::MidiDeviceInfo("Quad Cortex", "9"),
                                              juce::MidiDeviceInfo("BOSS_RC-5", "883557304") });
    CHECK_EQ(one.size(), 1u);
    if (one.size() == 1)
        CHECK_EQ(one[0].identifier.toStdString(), "883557304");

    // --- the family: the same bus, every profiled pedal with its model ---

    const auto family = pedallink::familyAmong(bus);
    CHECK_EQ(family.size(), 3u);
    if (family.size() == 3) {
        CHECK_EQ(std::string(family[0].family()), "RC-5");
        CHECK_EQ(family[0].endpoint.identifier.toStdString(), "883557304");
        CHECK_EQ(std::string(family[1].family()), "RC-500");
        CHECK_EQ(family[1].endpoint.identifier.toStdString(), "904629978");
        CHECK_EQ(std::string(family[2].family()), "RC-5");
        CHECK_EQ(family[2].endpoint.identifier.toStdString(), "2016199893");
        // Each carries its own model id — the RC-500's frames go out with 77.
        CHECK(family[1].modelId() == loopercat::profile::kRc500.modelId);
        CHECK(family[0].modelId() == loopercat::profile::kRc5.modelId);
        CHECK(family[0].profile == family[2].profile);
        CHECK(family[0].profile != family[1].profile);
    }
    // An RC model without a profile is announced, not handed out.
    CHECK(pedallink::familyAmong({ juce::MidiDeviceInfo("BOSS_RC-600", "1"), juce::MidiDeviceInfo("IRX", "2") })
              .empty());
    // A bare endpoint becomes a Pedal by its name, or is refused by name.
    CHECK_EQ(std::string(pedallink::pedalOf(juce::MidiDeviceInfo("BOSS_RC-500", "904629978")).family()), "RC-500");
    CHECK_THROWS(pedallink::pedalOf(juce::MidiDeviceInfo("BOSS_RC-600", "7")), "not an RC model this app has a profile for");
    CHECK_THROWS(pedallink::pedalOf(juce::MidiDeviceInfo("Quad Cortex", "9")), "not an RC model");

    return testkit::summary("pedal_link");
}
