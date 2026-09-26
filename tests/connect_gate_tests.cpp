// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The gate in front of Connect's first frame (issue #85), from what it
// promises rather than from its code:
//
//   - a busy pedal is refused with the sentence, and no frame goes out
//   - the sentence names BOTH causes the register cannot tell apart
//   - an idle pedal gets the frame
//   - a pedal already in storage gets no frame and no refusal
//   - no answer is not a refusal: the attempt (and its resend budget) runs

#include "support.hpp"

#include "../app/ConnectGate.h"

#include <cstring>

using namespace loopercat;

int main()
{
    // busy: refused, with the words, no frame
    {
        const auto d = connectgate::decide(storage::State::busy);
        CHECK(d.verdict == connectgate::Verdict::refuse);
        CHECK(!d.reason.empty());
        CHECK(d.reason.find("playing") != std::string::npos);
        CHECK(d.reason.find("unsaved take") != std::string::npos);
        CHECK(d.reason.find("WRITE") != std::string::npos);
        CHECK(d.reason.find("Connect") != std::string::npos);
        CHECK_EQ(d.reason, std::string(connectgate::kBusySentence));
    }
    // idle: the frame goes out, nothing to say
    {
        const auto d = connectgate::decide(storage::State::idle);
        CHECK(d.verdict == connectgate::Verdict::sendFrame);
        CHECK(d.reason.empty());
    }
    // already in storage: no frame, no refusal
    {
        const auto d = connectgate::decide(storage::State::inStorage);
        CHECK(d.verdict == connectgate::Verdict::alreadyInStorage);
        CHECK(d.reason.empty());
    }
    // silence is not a state: the attempt runs as before
    {
        const auto d = connectgate::decide(std::nullopt);
        CHECK(d.verdict == connectgate::Verdict::sendFrame);
        CHECK(d.reason.empty());
    }
    // the sentence is one line of plain text with a real dash, no placeholder
    {
        const std::string s = connectgate::kBusySentence;
        CHECK(s.find('\n') == std::string::npos);
        CHECK(s.find("\xe2\x80\x94") != std::string::npos);
        CHECK(s.find('?') == std::string::npos);
    }
    return testkit::summary("connect_gate_tests");
}
