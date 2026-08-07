// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The quit gate, tested from the theory of issue #1: only a held volume may
// delay the exit (connected starts a disconnect, ejecting joins the one in
// flight), and however the wait ends — eject completion, time bound, or both
// racing — the app must leave exactly once.

#include "support.hpp"

#include "../app/QuitGate.h"

using namespace loopercat;
using lifecycle::State;

int main()
{
    // --- the decision table: exactly two states hold the quit ---

    {
        int quits = 0;
        const auto quit = [&quits] { ++quits; };

        QuitGate gate;
        CHECK(gate.request(State::disconnected, quit) == QuitGate::Plan::quitNow);
        CHECK(gate.request(State::ghost, quit) == QuitGate::Plan::quitNow);
        CHECK(gate.request(State::ejected, quit) == QuitGate::Plan::quitNow);
        CHECK(!gate.pending()); // quitNow never arms — the caller exits itself
        CHECK_EQ(quits, 0);     // and the gate never fires for it

        CHECK(gate.request(State::connected, quit) == QuitGate::Plan::startDisconnect);
        CHECK(gate.pending());
        gate.finish();
        CHECK_EQ(quits, 1);

        CHECK(gate.request(State::ejecting, quit) == QuitGate::Plan::joinEject);
        CHECK(gate.pending());
        gate.finish();
        CHECK_EQ(quits, 2);
    }

    // --- exactly once: the eject completion and the time bound race, and
    // both may land — the second is a no-op ---

    {
        int quits = 0;
        QuitGate gate;
        gate.request(State::connected, [&quits] { ++quits; });
        gate.finish(); // the eject completion
        gate.finish(); // the bound fires later anyway
        gate.finish(); // and a stray extra for good measure
        CHECK_EQ(quits, 1);
        CHECK(!gate.pending());
    }

    // --- a second quit request (user hits Cmd-Q again, or logout follows a
    // manual quit) joins the pending one instead of restarting the release ---

    {
        int first = 0;
        int second = 0;
        QuitGate gate;
        CHECK(gate.request(State::connected, [&first] { ++first; })
              == QuitGate::Plan::startDisconnect);
        CHECK(gate.request(State::connected, [&second] { ++second; })
              == QuitGate::Plan::alreadyPending);
        CHECK(gate.request(State::disconnected, [&second] { ++second; })
              == QuitGate::Plan::alreadyPending); // pending outranks any state
        gate.finish();
        CHECK_EQ(first, 1);
        CHECK_EQ(second, 0); // the original continuation is the one that runs
    }

    // --- finish when idle is a no-op, not a crash: the time bound can fire
    // after a quitNow exit path never armed the gate ---

    {
        QuitGate gate;
        gate.finish();
        CHECK(!gate.pending());
    }

    // --- fail-fast: arming without a continuation is a caller bug ---

    {
        QuitGate gate;
        CHECK_THROWS(gate.request(State::connected, nullptr), "without a continuation");
        CHECK(!gate.pending()); // the bad request armed nothing
        // A holding state without a continuation is exactly as broken…
        CHECK_THROWS(gate.request(State::ejecting, {}), "without a continuation");
        // …but quitNow states never needed one, so an empty function is moot
        // there — the gate must still answer quitNow, not throw.
        CHECK(gate.request(State::disconnected, nullptr) == QuitGate::Plan::quitNow);
    }

    return testkit::summary("quitgate");
}
