// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The connection lifecycle machine, tested from the theory of the 2026-07-22
// field incident and the eject protocol — not from the implementation. The
// crown-jewel invariant: once the device is gone, NO sequence of path-side
// observations may ever make the machine writable again — only a real
// remount (a scan backed by a live device) can.

#include "support.hpp"

#include <loopercat/Lifecycle.hpp>

using namespace loopercat;
using lifecycle::Backing;
using lifecycle::Machine;
using lifecycle::State;

int main()
{
    // --- the incident replay: detach without eject ---

    {
        // The pedal is connected and honest.
        Machine m;
        m.pedalSeen(Backing::live);
        CHECK_EQ(lifecycle::stateName(m.state()), "connected");
        CHECK(m.writable());
        CHECK(m.playable());

        // USB device terminates. The mount keeps serving cached content —
        // that is exactly the lie the machine exists to catch.
        m.deviceLost();
        CHECK_EQ(lifecycle::stateName(m.state()), "ghost");
        CHECK(!m.writable());
        CHECK(!m.playable());
        CHECK(m.needsCleanup());

        // Scans keep finding a pedal-shaped tree (page cache). Still a ghost:
        // a phantom "one-shot saved" toast must be impossible from here.
        m.pedalSeen(Backing::gone);
        m.pedalSeen(Backing::gone);
        CHECK_EQ(lifecycle::stateName(m.state()), "ghost");
        CHECK(!m.writable());

        // Cleanup unmounts the ghost; the path finally vanishes.
        m.pedalMissing();
        CHECK_EQ(lifecycle::stateName(m.state()), "disconnected");
        CHECK(!m.needsCleanup());
    }

    // --- ghost discovered at startup (app launched against a stale mount) ---

    {
        Machine m;
        m.pedalSeen(Backing::gone);
        CHECK_EQ(lifecycle::stateName(m.state()), "ghost");
        CHECK(!m.writable());
        CHECK(m.needsCleanup());

        // Only a live-backed remount heals a ghost into connected.
        m.pedalSeen(Backing::live);
        CHECK_EQ(lifecycle::stateName(m.state()), "connected");
        CHECK(m.writable());
    }

    // --- clean teardown paths ---

    {
        // Unplug where the OS teardown works: device dies, then the path goes.
        Machine m;
        m.pedalSeen(Backing::live);
        m.deviceLost();
        CHECK_EQ(lifecycle::stateName(m.state()), "ghost");
        m.pedalMissing();
        CHECK_EQ(lifecycle::stateName(m.state()), "disconnected");

        // Finder eject: path vanishes first, device (still attached) dies later.
        m.pedalSeen(Backing::live);
        m.pedalMissing();
        CHECK_EQ(lifecycle::stateName(m.state()), "disconnected");
        m.deviceLost(); // stray notification — absorbed
        CHECK_EQ(lifecycle::stateName(m.state()), "disconnected");
    }

    // --- in-app eject protocol ---

    {
        // Happy path: eject, safe-to-disconnect persists through empty scans,
        // then the physical detach completes the cycle.
        Machine m;
        m.pedalSeen(Backing::live);
        m.ejectRequested();
        CHECK_EQ(lifecycle::stateName(m.state()), "ejecting");
        CHECK(!m.writable());
        CHECK(!m.playable());

        // Scans race the unmount in both directions; the decision stands.
        m.pedalSeen(Backing::live);
        CHECK_EQ(lifecycle::stateName(m.state()), "ejecting");
        m.pedalMissing();
        CHECK_EQ(lifecycle::stateName(m.state()), "ejecting");

        m.ejectFinished(true);
        CHECK_EQ(lifecycle::stateName(m.state()), "ejected");
        CHECK(!m.writable());
        m.pedalMissing(); // post-eject scans find nothing — still "safe to disconnect"
        CHECK_EQ(lifecycle::stateName(m.state()), "ejected");
        m.deviceLost(); // user finally pulls the cable
        CHECK_EQ(lifecycle::stateName(m.state()), "disconnected");

        // Replug after an eject.
        m.pedalSeen(Backing::live);
        CHECK_EQ(lifecycle::stateName(m.state()), "connected");
    }

    {
        // Eject refused (volume busy): the device never left — back to
        // connected, writable again.
        Machine m;
        m.pedalSeen(Backing::live);
        m.ejectRequested();
        m.ejectFinished(false);
        CHECK_EQ(lifecycle::stateName(m.state()), "connected");
        CHECK(m.writable());
    }

    {
        // Yank mid-eject, unmount still lands: the device is already gone, so
        // "safe to disconnect" would be a lie — straight to disconnected.
        Machine m;
        m.pedalSeen(Backing::live);
        m.ejectRequested();
        m.deviceLost();
        CHECK_EQ(lifecycle::stateName(m.state()), "ejecting"); // recorded, not resolved
        m.ejectFinished(true);
        CHECK_EQ(lifecycle::stateName(m.state()), "disconnected");
    }

    {
        // Yank mid-eject and the unmount fails: cached mount + dead device —
        // the classic ghost, needs cleanup.
        Machine m;
        m.pedalSeen(Backing::live);
        m.ejectRequested();
        m.deviceLost();
        m.ejectFinished(false);
        CHECK_EQ(lifecycle::stateName(m.state()), "ghost");
        CHECK(m.needsCleanup());
    }

    {
        // A device lost during one eject must not leak into the next one.
        Machine m;
        m.pedalSeen(Backing::live);
        m.ejectRequested();
        m.deviceLost();
        m.ejectFinished(false); // ghost
        m.pedalSeen(Backing::live); // remounted properly
        m.ejectRequested();
        m.ejectFinished(true);
        CHECK_EQ(lifecycle::stateName(m.state()), "ejected"); // NOT disconnected
    }

    // --- eject protocol violations throw, loudly and typed ---

    {
        Machine m;
        CHECK_THROWS(m.ejectRequested(), "only legal while connected"); // disconnected
        m.pedalSeen(Backing::gone);
        CHECK_THROWS(m.ejectRequested(), "only legal while connected"); // ghost
        CHECK_THROWS(m.ejectFinished(true), "no eject was in flight");
        m.pedalSeen(Backing::live);
        m.ejectRequested();
        CHECK_THROWS(m.ejectRequested(), "only legal while connected"); // already ejecting
        m.ejectFinished(true);
        CHECK_THROWS(m.ejectRequested(), "only legal while connected"); // ejected
        CHECK_THROWS(m.ejectFinished(true), "no eject was in flight");  // completed twice
    }

    // --- synthetic volumes (headless tests, --volume pin) are trusted ---

    {
        Machine m;
        m.pedalSeen(Backing::untracked);
        CHECK_EQ(lifecycle::stateName(m.state()), "connected");
        CHECK(m.writable());

        // But a ghost verdict still wins over trust on the next scan.
        m.pedalSeen(Backing::gone);
        CHECK(!m.writable());
    }

    // --- a volume reappearing as a ghost right after eject stays untouchable ---

    {
        Machine m;
        m.pedalSeen(Backing::live);
        m.ejectRequested();
        m.ejectFinished(true); // ejected
        m.pedalSeen(Backing::gone);
        CHECK_EQ(lifecycle::stateName(m.state()), "ghost");
        CHECK(!m.writable());
    }

    return testkit::summary("lifecycle");
}
