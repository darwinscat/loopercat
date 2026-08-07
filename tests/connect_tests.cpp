// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The connect-attempt machine, tested from the theory of issue #2 — the two
// observed silent failures replayed against the policy, not against the
// implementation: a lost frame must be re-sent on the resend clock, a pedal
// that never surrenders the medium must produce exactly one honest give-up,
// and a disk-appeared hands the story to the mount pipeline for good.

#include "support.hpp"

#include <loopercat/Connect.hpp>

using namespace loopercat;
using connect::Action;
using connect::Attempt;

int main()
{
    // --- the happy path: the pedal answers before the resend clock fires ---

    {
        Attempt a;
        CHECK(!a.active());
        a.begin(0);
        CHECK(a.active());
        CHECK_EQ(a.sendsUsed(), 1);
        CHECK(a.tick(1000) == Action::none);
        a.diskAppeared();
        // The attempt stays active (Connect stays gated) but never resends
        // and never gives up — the mount + scan pipeline owns it now.
        CHECK(a.active());
        CHECK(a.tick(3000) == Action::none);
        CHECK(a.tick(600000) == Action::none);
        CHECK_EQ(a.sendsUsed(), 1);
        a.finish(); // the volume came up
        CHECK(!a.active());
    }

    // --- the lost frame (clicked into the re-enumeration window): the
    // resend clock fires at exactly resendAfterMs, not a tick before ---

    {
        Attempt a;
        a.begin(0);
        CHECK(a.tick(1999) == Action::none);
        CHECK(a.tick(2000) == Action::sendFrame);
        CHECK_EQ(a.sendsUsed(), 2);
        // The resend window restarts from the resend, not from begin().
        CHECK(a.tick(3999) == Action::none);
        a.diskAppeared(); // second frame landed
        CHECK(a.tick(4000) == Action::none);
        CHECK_EQ(a.sendsUsed(), 2);
    }

    // --- the playing-loop refusal: the full budget goes out, then exactly
    // one give-up, one resend window after the LAST send ---

    {
        Attempt a;
        a.begin(0);
        CHECK(a.tick(2000) == Action::sendFrame);
        CHECK(a.tick(4000) == Action::sendFrame);
        CHECK_EQ(a.sendsUsed(), 3); // the default budget, spent
        CHECK(a.tick(5999) == Action::none); // the last send gets its full window
        CHECK(a.tick(6000) == Action::giveUp);
        CHECK(!a.active()); // over — Connect comes back
        CHECK(a.tick(8000) == Action::none); // and it stays over
    }

    // --- a slow tick must not skip the give-up: budget spent, one late tick ---

    {
        Attempt a({ .resendAfterMs = 2000, .sendBudget = 1 });
        a.begin(0);
        CHECK(a.tick(60000) == Action::giveUp);
        CHECK(!a.active());
    }

    // --- the attempt is restartable: after a give-up or a finish, a new
    // begin starts a fresh budget ---

    {
        Attempt a({ .resendAfterMs = 2000, .sendBudget = 1 });
        a.begin(0);
        CHECK(a.tick(2000) == Action::giveUp);
        a.begin(10000);
        CHECK_EQ(a.sendsUsed(), 1);
        CHECK(a.tick(11000) == Action::none);
        a.finish();
        a.begin(20000);
        CHECK(a.active());
    }

    // --- events from the world are not protocol calls ---

    {
        Attempt a;
        a.diskAppeared(); // a by-hand STORAGE entry, no attempt running
        CHECK(!a.active());
        CHECK(a.tick(1000) == Action::none);
        a.finish(); // idempotent, even when idle
        CHECK(!a.active());

        a.begin(0);
        a.diskAppeared();
        a.diskAppeared(); // a second partition — still just mounting
        CHECK(a.active());
        CHECK(a.tick(100000) == Action::none);
    }

    // --- caller bugs and bad config throw, loudly and typed ---

    {
        Attempt a;
        a.begin(0);
        CHECK_THROWS(a.begin(100), "already in flight");
        CHECK_THROWS(a.tick(-1), "clock went backwards");
        CHECK_THROWS(Attempt({ .resendAfterMs = 0, .sendBudget = 3 }), "must be positive");
        CHECK_THROWS(Attempt({ .resendAfterMs = -5, .sendBudget = 3 }), "must be positive");
        CHECK_THROWS(Attempt({ .resendAfterMs = 2000, .sendBudget = 0 }), "at least 1");
    }

    return testkit::summary("connect");
}
