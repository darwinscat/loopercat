// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What the uncached-read probe is allowed to conclude (app/MountProbe.h),
// tested from the specification in issue #166 and from the invariants the
// device story has to hold — not from the decision's own shape.
//
// The stakes are the worst thing this app can do to a player: on 2026-10-27
// it force-unmounted a live card on three Connects in a row, because the
// first verdict on each fresh mount read the PREVIOUS mount's failed probe.
// So the suite is written to condemn a mount on the slightest excuse, and
// every case where it stays live is a promise:
//
//   - a mount nobody has probed is live (whatever is remembered about
//     another mount of the same path),
//   - one failed read on a mount that has only just come up is not enough,
//   - a read that never comes back IS enough (the FSKit wedge of
//     2026-07-22 is the reason the ghost path exists at all),
//   - and every ghost says in the journal why, with its numbers.

#include "support.hpp"

#include "../app/MountProbe.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>

using namespace loopercat::mountprobe;
using namespace std::chrono_literals;

namespace {

// The mount of the episode: one medium, one mount of it, in a world where
// nothing has been mounted or unmounted since.
const Incarnation kMount{ "disk10s1", 0x100000abc, 7 };

// What a backend fills in for a mount it HAS probed, with no probe out and
// nothing read yet. Every case below changes only what it is about.
Facts watched(std::chrono::steady_clock::duration seenFor)
{
    Facts facts;
    facts.ours = true;
    facts.seenFor = seenFor;
    facts.registryHasMedia = true;
    return facts;
}

Facts refusedOnce(std::chrono::steady_clock::duration seenFor)
{
    Facts facts = watched(seenFor);
    facts.haveResult = true;
    facts.read = { Answer::refused, 0, 5 }; // EIO, as a yanked medium answers
    facts.refusals = 1;
    return facts;
}

bool says(const std::string& line, const std::string& fragment)
{
    return line.find(fragment) != std::string::npos;
}

// The journal line of facts that are supposed to BE a ghost. A module that
// refuses to make one is a failure to report, not a crash that swallows the
// rest of the suite.
std::string lineFor(const Facts& facts)
{
    try {
        return ghostLine(facts);
    } catch (const loopercat::Error& error) {
        return std::string("refused to make a line: ") + error.what();
    }
}

} // namespace

int main()
{
    // ===== a mount incarnation owns its probe result =====

    // The identity has to be able to tell the three mounts of issue #166
    // apart: same path, same disk number, one after another.
    {
        CHECK((kMount == Incarnation{ "disk10s1", 0x100000abc, 7 }));
        // A fresh medium under a recycled disk number.
        CHECK((!(kMount == Incarnation{ "disk10s1", 0x100000def, 7 })));
        // The same medium, mounted again: the epoch moved.
        CHECK((!(kMount == Incarnation{ "disk10s1", 0x100000abc, 9 })));
        // Another disk entirely.
        CHECK((!(kMount == Incarnation{ "disk11s1", 0x100000abc, 7 })));
    }

    // A stored result may be read only about the very mount and the very
    // file it was taken on.
    {
        CHECK(speaksFor(kMount, "/Volumes/BOSS RC-5/ROLAND/DATA/MEMORY1.RC0", kMount,
                        "/Volumes/BOSS RC-5/ROLAND/DATA/MEMORY1.RC0"));
        // The mount of the previous Connect, at the same path, probed with
        // the same file: this is the exact shape that force-unmounted a live
        // card three times.
        CHECK((!speaksFor(Incarnation{ "disk10s1", 0x100000abc, 5 },
                          "/Volumes/BOSS RC-5/ROLAND/DATA/MEMORY1.RC0", kMount,
                          "/Volumes/BOSS RC-5/ROLAND/DATA/MEMORY1.RC0")));
        // Same mount, another probe file: says nothing about this one.
        CHECK(!speaksFor(kMount, "/Volumes/BOSS RC-5/ROLAND/DATA/MEMORY2.RC0", kMount,
                         "/Volumes/BOSS RC-5/ROLAND/DATA/MEMORY1.RC0"));
        // A watcher that has never probed anything.
        CHECK((!speaksFor(Incarnation{}, "", kMount, "/Volumes/BOSS RC-5")));
    }

    // "A fresh mount after a failed probe on the previous mount is live
    // until its own probe says otherwise." The failure is as damning as it
    // gets — two refusals, no medium in the registry, an hour of watching —
    // and it still decides nothing here, because it is not about this mount.
    {
        Facts inherited = refusedOnce(1h);
        inherited.refusals = 9;
        inherited.registryHasMedia = false;
        inherited.ours = false; // another incarnation's state

        const Decision decision = decide(inherited);
        CHECK(decision.call == Call::live);
        CHECK(decision.launchProbe); // ...and it is asked at once
    }

    // Nor does a mount with no probe result of its own become a ghost by
    // sitting there: the first scans of a fresh mount are live.
    {
        for (const auto age : { 0ms, 100ms, 2500ms, 4000ms, 30000ms }) {
            const Decision decision = decide(watched(age));
            CHECK(decision.call == Call::live);
            CHECK(decision.launchProbe);
        }
    }

    // ===== one failed read on a young mount is not a verdict =====

    // "A single failed read on a mount younger than the grace period does
    // not unmount it; a second one does."
    {
        CHECK(decide(refusedOnce(200ms)).call == Call::live);
        CHECK(decide(refusedOnce(200ms)).launchProbe); // asked again, which is the point

        Facts twice = refusedOnce(200ms);
        twice.refusals = 2;
        CHECK(decide(twice).call == Call::ghost);
    }

    // A refusal plus the platform agreeing there is no medium behind the
    // mount needs no second opinion.
    {
        Facts noMedium = refusedOnce(200ms);
        noMedium.registryHasMedia = false;
        CHECK(decide(noMedium).call == Call::ghost);
    }

    // The grace is the YOUNG mount's alone: a pedal that leaves STORAGE
    // mid-session has been watched for a while, and one refused read must
    // still take its mount down on the next scan.
    {
        CHECK(decide(refusedOnce(10s)).call == Call::ghost);
        // The boundary, both sides of it: at the settling period the mount
        // still gets its second chance, past it it does not.
        CHECK(decide(refusedOnce(kSettlingPeriod)).call == Call::live);
        CHECK(decide(refusedOnce(kSettlingPeriod + 1ms)).call == Call::ghost);
    }

    // A read that answered clears the slate, however old the mount: the
    // medium is there, and a refusal before it was a miss, not a death.
    {
        Facts answered = watched(1h);
        answered.haveResult = true;
        answered.read = { Answer::served, 4096, 0 };
        answered.refusals = 0;
        CHECK(decide(answered).call == Call::live);
        CHECK(decide(answered).launchProbe);
    }

    // A filesystem that cannot do an uncached read at all (Linux O_DIRECT
    // refused) gives no verdict: it must never condemn a mount, no matter
    // how old or how often it is asked.
    {
        Facts capability = watched(1h);
        capability.haveResult = true;
        capability.read = { Answer::noVerdict, 0, 22 }; // EINVAL from open
        capability.refusals = 0;
        CHECK(decide(capability).call == Call::live);

        capability.refusals = 7; // a backend that miscounted them still may not condemn
        CHECK(decide(capability).call == Call::live);
    }

    // ===== a read that never comes back =====

    // "A stuck probe on a mount older than the grace period is still a
    // ghost" — the FSKit wedge is what the whole ghost path is for.
    {
        Facts wedged = watched(30s);
        wedged.inFlight = true;
        wedged.outstanding = kProbeStuckAfter + 1ms;
        CHECK(decide(wedged).call == Call::ghost);
        CHECK(!decide(wedged).launchProbe); // no second thread onto a wedged volume

        wedged.outstanding = 10s;
        CHECK(decide(wedged).call == Call::ghost);
    }

    // A probe still within its time is no verdict at all: the last
    // completed read stands while it runs.
    {
        Facts running = watched(30s);
        running.inFlight = true;
        running.outstanding = kProbeStuckAfter; // not past it yet
        CHECK(decide(running).call == Call::live);
        CHECK(!decide(running).launchProbe);

        Facts afterRefusal = refusedOnce(30s);
        afterRefusal.inFlight = true;
        afterRefusal.outstanding = 100ms;
        CHECK(decide(afterRefusal).call == Call::ghost); // the old mount's refusal stands

        Facts youngAfterRefusal = refusedOnce(200ms);
        youngAfterRefusal.inFlight = true;
        youngAfterRefusal.outstanding = 100ms;
        CHECK(decide(youngAfterRefusal).call == Call::live); // ...but not on a young one
    }

    // A probe out on a mount this state does not speak for is abandoned,
    // not waited on: the new mount gets its own probe immediately.
    {
        Facts foreignProbe = watched(0ms);
        foreignProbe.ours = false;
        foreignProbe.inFlight = true;
        foreignProbe.outstanding = 10s;
        CHECK(decide(foreignProbe).call == Call::live);
        CHECK(decide(foreignProbe).launchProbe);
    }

    // ===== the journal =====

    // "Every forced unmount leaves a line in the log that names the
    // reason." The read's own numbers, the registry, and the age of the
    // mount, because all three went into the decision.
    {
        Facts twice = refusedOnce(7100ms);
        twice.refusals = 2;
        const std::string line = lineFor(twice);
        CHECK(says(line, "ghost mount: "));
        CHECK(says(line, "did not answer twice"));
        CHECK(says(line, "0 bytes"));
        CHECK(says(line, "error 5"));
        CHECK(says(line, "still lists the medium"));
        CHECK(says(line, "7.1 s ago"));
    }

    {
        Facts noMedium = refusedOnce(400ms);
        noMedium.registryHasMedia = false;
        const std::string line = lineFor(noMedium);
        CHECK(says(line, "did not answer once"));
        CHECK(says(line, "no longer lists the medium"));
        CHECK(says(line, "400 ms ago"));
    }

    {
        Facts wedged = watched(12s);
        wedged.inFlight = true;
        wedged.outstanding = 2143ms;
        const std::string line = lineFor(wedged);
        CHECK(says(line, "has not come back in 2.1 s"));
        CHECK(says(line, "wedged"));
        CHECK(says(line, "12.0 s ago"));
    }

    {
        Facts often = refusedOnce(10s);
        often.refusals = 4;
        CHECK(says(lineFor(often), "4 times in a row"));
    }

    // A ghost can have a probe out and still be a ghost on the refusal that
    // came BEFORE it. The line must then name the refusal: calling it a wedge
    // would put a sentence in the journal that never happened.
    {
        Facts standing = refusedOnce(10s);
        standing.inFlight = true;
        standing.outstanding = 100ms; // nowhere near stuck
        CHECK(decide(standing).call == Call::ghost);
        const std::string line = lineFor(standing);
        CHECK(says(line, "did not answer once"));
        CHECK(!says(line, "wedged"));
        CHECK(!says(line, "has not come back"));
    }

    // A line about a ghost for a mount that is not one would put a reason in
    // the journal for a decision nobody took.
    {
        CHECK_THROWS(ghostLine(watched(10s)), "not one");
        CHECK_THROWS(ghostLine(refusedOnce(200ms)), "not one");
        Facts uncounted = refusedOnce(10s);
        uncounted.refusals = 0; // a refusal the backend forgot to count
        CHECK_THROWS(ghostLine(uncounted), "must be counted");
    }

    // The ghosts the platform settles before the probe speaks still say what
    // was seen; a reason-less line is the silence this module exists to end.
    {
        CHECK_EQ(platformGhostLine("statfs no longer answers for /Volumes/BOSS RC-5"),
                 std::string("ghost mount: statfs no longer answers for /Volumes/BOSS RC-5"));
        CHECK_THROWS(platformGhostLine(""), "no reason");
    }

    // What came of clearing it, both ways, and the dissent named when there
    // was one.
    {
        CHECK_EQ(forcedUnmountLine(true, {}),
                 std::string("ghost mount: cleared by a forced unmount"));
        CHECK(says(forcedUnmountLine(true, "volume unmounted, but ejecting the disk was refused"),
                   "(volume unmounted, but ejecting the disk was refused)"));
        const std::string refused = forcedUnmountLine(false, "dissented by Finder");
        CHECK(says(refused, "did not go through"));
        CHECK(says(refused, "(dissented by Finder)"));
    }

    // Durations as the journal prints them, at the places a reader would
    // misread them.
    {
        CHECK_EQ(howLong(0ms), std::string("0 ms"));
        CHECK_EQ(howLong(999ms), std::string("999 ms"));
        CHECK_EQ(howLong(1000ms), std::string("1.0 s"));
        CHECK_EQ(howLong(1049ms), std::string("1.0 s"));
        CHECK_EQ(howLong(1050ms), std::string("1.1 s"));
        CHECK_EQ(howLong(65s), std::string("65.0 s"));
    }

    // ===== the lines reach the journal, not just this suite =====

    // The two call sites live in MainComponent, which no headless suite can
    // build (it wants a pedal, a worker thread and a screen), so they are
    // proved by reading it as text — the way hidden_features_tests proves
    // what this release hid. Silence here is the other half of issue #166:
    // the app force-unmounted the player's card and wrote nothing at all,
    // and only the system log knew who had done it.
    {
        std::ifstream in(std::string(LOOPERCAT_APP_DIR) + "/MainComponent.cpp");
        CHECK(in.good());
        const std::string owner((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        CHECK(owner.size() > 10000); // a file that failed to read would pass the rest

        // The forced unmount's outcome is traced, both ways, from the
        // completion that knows it.
        CHECK(says(owner, "trace(utf8(mountprobe::forcedUnmountLine("));

        // The watcher's reason is traced, not swallowed by the handler.
        const std::size_t handler = owner.find("onGhostVerdict");
        CHECK(handler != std::string::npos);
        if (handler != std::string::npos)
            CHECK(says(owner.substr(handler, 400), "trace("));
    }

    return testkit::summary("mount probe");
}
