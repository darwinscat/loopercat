// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The empty window's words about the bus (issues #98, #82), from what they
// promise: Connect is offered by the profile table's read gate, never by a
// name; one readable pedal is named; several are counted; a model closed
// for reading is named for what it is and offered nothing; nothing on the
// bus is an instruction, not a shrug.

#include "support.hpp"

#include "../app/PedalPresence.h"

using namespace loopercat;

int main()
{
    // nothing on the bus
    {
        const auto v = presence::describe({});
        CHECK(!v.connectable);
        CHECK(v.hint.find("USB") != std::string::npos);
        CHECK_EQ(v.status, std::string("No looper found"));
    }
    // one readable pedal, named
    {
        const auto v = presence::describe({ { "RC-5", true } });
        CHECK(v.connectable);
        CHECK(v.hint.find("RC-5 detected") != std::string::npos);
        CHECK(v.hint.find("Connect") != std::string::npos);
        CHECK(v.status.find("RC-5 on USB") != std::string::npos);
    }
    // two readable pedals of one model, and of two models: counted, choice announced
    {
        const auto same = presence::describe({ { "RC-5", true }, { "RC-5", true } });
        CHECK(same.connectable);
        CHECK(same.hint.find("2 loopers") != std::string::npos);
        CHECK(same.hint.find("choose") != std::string::npos);
        const auto mixed = presence::describe({ { "RC-5", true }, { "RC-500", true }, { "RC-5", true } });
        CHECK(mixed.connectable);
        CHECK(mixed.hint.find("3 loopers") != std::string::npos);
    }
    // a model closed for reading: named, and Connect is not offered
    {
        const auto v = presence::describe({ { "RC-500", false } });
        CHECK(!v.connectable);
        CHECK(v.hint.find("RC-500 detected") != std::string::npos);
        CHECK(v.hint.find("only speaks") != std::string::npos);
        CHECK(v.status.find("not readable") != std::string::npos);
    }
    // a readable pedal beside a closed one: Connect is offered, the readable one is named
    {
        const auto v = presence::describe({ { "RC-500", false }, { "RC-5", true } });
        CHECK(v.connectable);
        CHECK(v.hint.find("RC-5 detected") != std::string::npos);
        CHECK(v.hint.find("RC-500") == std::string::npos);
    }
    // the gate is the table's: a profile's own read bit decides
    {
        const auto rc5 = presence::seen(profile::kRc5);
        CHECK(rc5.readable);
        CHECK_EQ(rc5.family, std::string("RC-5"));
        const auto rc500 = presence::seen(profile::kRc500);
        CHECK_EQ(rc500.family, std::string("RC-500"));
        CHECK_EQ(rc500.readable, profile::kRc500.allows(profile::Operation::read));
    }
    // a nameless entry is a bug, not a blank line
    CHECK_THROWS(presence::describe({ { "", true } }), "family name");

    return testkit::summary("pedal_presence_tests");
}
