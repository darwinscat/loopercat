// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pedal's name on the MIDI bus, tested from what the names ARE rather
// than from how the matcher is written. The facts: with an RC-5 and an
// RC-500 on the bus at once (2026-09-16), macOS listed them as "BOSS_RC-5"
// and "BOSS_RC-500", and the rest of the RC family carries the same prefix —
// RC-300, RC-505, RC-600, RC-10R.
//
// The bug this suite exists for: a substring search for "RC-5" also finds
// it inside "BOSS_RC-500" (and "BOSS_RC-505"). An RC-500 on USB was greeted
// as "RC-5 detected", and Connect sent it a frame carrying the RC-5's model
// id — which the RC-500 ignores in silence, so the attempt just ran out.

#include "support.hpp"

#include "../app/PedalPortName.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

using namespace loopercat::portname;

namespace {

std::string modelOf(std::string_view name)
{
    const auto model = announcedModel(name);
    return model ? *model : std::string("<none>");
}

} // namespace

int main()
{
    // --- the two pedals on the desk, exactly as the OS named them ---

    CHECK(isRc5("BOSS_RC-5"));
    CHECK_EQ(modelOf("BOSS_RC-5"), std::string("RC-5"));
    CHECK(!isRc5("BOSS_RC-500"));
    CHECK_EQ(modelOf("BOSS_RC-500"), std::string("RC-500"));

    // --- the rest of the family, and anything else that merely BEGINS with
    // "RC-5": each is its own model, and none of them is ours ---

    for (const std::string_view model :
         { "RC-300", "RC-505", "RC-505MKII", "RC-600", "RC-10R", "RC-50", "RC-55", "RC-5X" }) {
        const std::string name = "BOSS_" + std::string(model);
        CHECK(!isRc5(name));
        CHECK_EQ(modelOf(name), std::string(model));
    }

    // --- the wrappers an OS or driver may put around the same name ---

    for (const std::string_view name :
         { "RC-5", "BOSS RC-5", "BOSS_RC-5 MIDI 1", "MIDIIN2 (BOSS_RC-5)", "2- BOSS_RC-5",
           "BOSS_RC-5 BOSS_RC-5 MIDI 1" /* the Linux backend's name + subdevice */ })
        CHECK(isRc5(name));
    CHECK(!isRc5("MIDIIN2 (BOSS_RC-500)"));
    CHECK(!isRc5("BOSS_RC-500 MIDI 1"));

    // Case is not identity: the old match ignored it, and so does this one —
    // and the model it reports reads the way the pedal spells it.
    CHECK(isRc5("boss_rc-5"));
    CHECK_EQ(modelOf("boss_rc-500"), std::string("RC-500"));
    CHECK_EQ(modelOf("boss_rc-10r"), std::string("RC-10R"));

    // --- not a pedal at all ---

    for (const std::string_view name :
         { "", "IAC Driver Bus 1", "Clarett+ 8Pre", "RC5", "RC-", "RC-X1", "BOSS_RC-" })
        CHECK_EQ(modelOf(name), std::string("<none>"));

    // "RC-" has to start a word: a longer name that merely ends in those
    // letters is another device entirely.
    CHECK_EQ(modelOf("ARC-5"), std::string("<none>"));
    CHECK_EQ(modelOf("XRC-500"), std::string("<none>"));
    CHECK_EQ(modelOf("2RC-5"), std::string("<none>"));

    // --- bytes outside ASCII neither match nor crash the scan ---

    CHECK(isRc5("\xe2\x99\xaa RC-5"));    // a note sign before the name
    CHECK(isRc5("\xffRC-5"));             // a lone high byte right against it
    CHECK(isRc5("RC-5\xe2\x84\xa2"));     // a trademark sign after it
    CHECK(!isRc5("RC-50\xe2\x84\xa2"));

    // --- both pedals on the bus: whichever the OS lists first, the RC-5 is
    // the one Connect talks to ---

    {
        const std::array<std::string_view, 2> busA { "BOSS_RC-500", "BOSS_RC-5" };
        const std::array<std::string_view, 2> busB { "BOSS_RC-5", "BOSS_RC-500" };
        const auto pick = [](const auto& bus) {
            const auto it = std::find_if(bus.begin(), bus.end(),
                                         [](std::string_view n) { return isRc5(n); });
            return it == bus.end() ? std::string("<none>") : std::string(*it);
        };
        CHECK_EQ(pick(busA), std::string("BOSS_RC-5"));
        CHECK_EQ(pick(busB), std::string("BOSS_RC-5"));
    }

    // --- only the RC-500 on the bus: nothing to connect to ---

    {
        const std::array<std::string_view, 2> bus { "IAC Driver Bus 1", "BOSS_RC-500" };
        CHECK(std::none_of(bus.begin(), bus.end(), [](std::string_view n) { return isRc5(n); }));
    }

    // --- the profile behind a name: the two models on the desk resolve to
    // their table entries, the rest of the family and everything else to none ---

    CHECK(familyProfile("BOSS_RC-5") == &loopercat::profile::kRc5);
    CHECK(familyProfile("BOSS_RC-500") == &loopercat::profile::kRc500);
    CHECK(familyProfile("MIDIIN2 (BOSS_RC-500)") == &loopercat::profile::kRc500);
    CHECK(familyProfile("BOSS_RC-5 BOSS_RC-5 MIDI 1") == &loopercat::profile::kRc5);
    for (const std::string_view name :
         { "BOSS_RC-600", "BOSS_RC-505", "BOSS_RC-10R", "BOSS_RC-50", "Quad Cortex", "Clarett+ 8Pre", "" })
        CHECK(familyProfile(name) == nullptr);
    // The profile carries the model id the frames go out with.
    CHECK(familyProfile("BOSS_RC-500")->modelId[3] == 0x77);
    CHECK(familyProfile("BOSS_RC-5")->modelId[3] == 0x76);

    return testkit::summary("pedal_port_name");
}
