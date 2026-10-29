// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <chrono>
#include <string>

//==============================================================================
// loopercat::mountprobe — what the uncached-read probe is allowed to conclude,
// and the words operations.log carries when it goes against the mount.
//
// All three DeviceWatcher backends end their verdict the same way: the
// registry (or sysfs, or the drive table) says a device is behind the mount,
// and then ONE read past the page cache decides whether the medium actually
// answers — the RC-5 leaving STORAGE surrenders its medium while its whole
// USB stack stays registered (2026-07-23), and only a real device can serve
// bytes the cache cannot fake. The probe runs on a sacrificial thread with a
// timeout, so the verdict always reads a result that is one scan old.
//
// Two rules make that stored result safe to read, both paid for by issue #166
// (a live card force-unmounted on three Connects in a row, 2026-10-27):
//
//  - a result belongs to ONE mount incarnation. The backends key their probe
//    state by what is mounted right now, and a verdict about a mount the
//    stored result does not speak for ignores it and starts a fresh probe: a
//    mount nobody has probed yet is live, never a ghost on inherited data.
//  - a mount that has only just come under our eye is not condemned by a
//    single failed read. The first uncached read right after a mount comes up
//    can miss while the card is perfectly alive, and the answer to one missed
//    read is to ask again, not to pull the volume out from under the player.
//
// The wedge case is deliberately untouched: a read that never comes back at
// all is why the ghost path exists (the FSKit spin of 2026-07-22).
//
// Pure — no mount, no device, no window — so the rules and their sentences
// are tested from the spec in mount_probe_tests.
//==============================================================================
namespace loopercat::mountprobe
{

using Clock = std::chrono::steady_clock;

// An I/O probe outstanding longer than this means the filesystem is wedged
// (the FSKit spin of 2026-07-22): the device story is over even though
// nothing ever returns an error.
inline constexpr auto kProbeStuckAfter = std::chrono::seconds(2);

// How long a mount is given before ONE failed read may condemn it. Three
// seconds covers a mount still coming up (issue #166: the first uncached read
// after the FSKit mount appeared returned nothing while the card was alive)
// and costs a truly dead mount two more scans of the worker's 1.5 s poll —
// the stale mount is still cleared, just after asking twice.
inline constexpr auto kSettlingPeriod = std::chrono::seconds(3);

// What one completed read did.
enum class Answer {
    served,   // the medium answered with bytes from past the cache
    refused,  // the read failed: the medium is gone, or wedged
    noVerdict // the probe could not run at all (a filesystem with no uncached
              // read — Linux O_DIRECT on a filesystem that refuses it). It
              // says nothing either way, so it never condemns a mount.
};

// One completed read, with the numbers. They are kept because "why did the
// app decide the card was gone" had no answer at all before issue #166:
// nothing recorded them and the journal stayed silent.
struct Read {
    Answer answer = Answer::noVerdict;
    long long bytes = 0;
    int error = 0; // errno (POSIX) or GetLastError (Windows) of a read that did not answer
};

// The identity of ONE mount incarnation, as its backend can name it: the
// device behind the mount, plus whatever the platform offers that a REMOUNT
// cannot repeat. A disk number on its own is not it — the three mounts of
// issue #166 all came up as disk10s1.
//
//   macOS   device = the BSD name, instance = the IOMedia object's registry
//           entry ID (never reused while the system runs), epoch = how many
//           mount/unmount events DiskArbitration has reported.
//   Linux   device = major:minor, instance = the mount id from
//           /proc/self/mountinfo (the kernel issues a fresh one per mount);
//           no epoch, the mount id already changes per mount.
//   Windows device = the drive letter, epoch = how many volume arrivals and
//           removals WM_DEVICECHANGE has reported; no instance, since
//           Windows offers no per-mount number from the mount manager's
//           side.
//
// Every part of it comes from the system's own bookkeeping, never from the
// medium. A number read OUT of the medium (its serial, its label) stops
// being readable exactly when the medium dies — the identity would change
// at the moment a ghost appears, and the mount would stay "one we have not
// probed yet" forever instead of ever being called gone.
struct Incarnation {
    std::string device;
    unsigned long long instance = 0;
    long long epoch = 0;
    bool operator==(const Incarnation&) const = default;
};

// May the stored probe state be read as a statement about the mount in front
// of us? Only when it was taken on this very incarnation, probing this very
// file. Everything else — an earlier mount of the same path, another volume,
// another probe file — says nothing about this mount.
inline bool speaksFor(const Incarnation& stored, const std::string& storedFile,
                      const Incarnation& here, const std::string& file)
{
    return stored == here && storedFile == file;
}

// What the backend knows when it asks, as it reads its own probe state under
// its own lock. Everything but `registryHasMedia` is about the stored probe;
// when it does not speak for this mount (see speaksFor) the rest is not ours
// to read, and `ours` false is the whole truth the decision needs.
struct Facts {
    bool ours = false;
    bool inFlight = false;         // a probe thread is out
    Clock::duration outstanding{}; // ...and has been running this long
    bool haveResult = false;       // a probe has completed for this incarnation
    Read read{};                   // what it read
    int refusals = 0;              // reads in a row that did not answer, this incarnation
    Clock::duration seenFor{};     // how long this incarnation has been under our eye
    bool registryHasMedia = true;  // the platform still lists a medium behind the mount
};

enum class Call {
    live, // the mount may be used
    ghost // the mount serves page-cache fiction: nothing may be read from it or written to it
};

struct Decision {
    Call call = Call::live;
    bool launchProbe = false; // ask the medium again, on a fresh sacrificial thread
};

// The answer the last COMPLETED read leaves standing.
inline Call settled(const Facts& facts)
{
    if (!facts.haveResult || facts.read.answer != Answer::refused)
        return Call::live;
    // A read that did not answer. One is enough on a mount we have been
    // watching for a while — that is how a pedal leaving STORAGE mid-session
    // reads. On a mount we have only just met it takes a second refusal, or
    // the platform agreeing there is no medium behind it (issue #166).
    if (facts.seenFor > kSettlingPeriod || facts.refusals >= 2 || !facts.registryHasMedia)
        return Call::ghost;
    return Call::live;
}

inline Decision decide(const Facts& facts)
{
    // A result about another mount incarnation says nothing about this one,
    // and this mount has not been asked yet: live, and ask it.
    if (!facts.ours)
        return { Call::live, true };
    if (facts.inFlight) {
        // Wedged on this mount is the one case where no answer IS the answer.
        // Short of that the last completed read stands while this one runs; a
        // second thread would only pile onto a volume that is already busy.
        if (facts.outstanding > kProbeStuckAfter)
            return { Call::ghost, false };
        return { settled(facts), false };
    }
    return { settled(facts), true }; // keep the verdict fresh (and recover if the medium returns)
}

//==============================================================================
// The journal's words. Before issue #166 this whole path wrote nothing: the
// app force-unmounted the player's card and the only trace was in the system
// log. A line here is read by a musician trying to understand why the card
// went away, so it names the mechanism AND its numbers.
//==============================================================================

// A duration as the journal prints it: milliseconds below a second, then
// seconds with one decimal.
inline std::string howLong(Clock::duration span)
{
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(span).count();
    if (ms < 1000)
        return std::to_string(ms) + " ms";
    const long long tenths = (ms + 50) / 100;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " s";
}

inline std::string howOften(int times)
{
    if (times < 1)
        throw Error("mountprobe: a read that did not answer must be counted");
    if (times == 1)
        return "once";
    if (times == 2)
        return "twice";
    return std::to_string(times) + " times in a row";
}

// Why the probe's own verdict went against the mount: the read, what the
// platform's registry still says, and how long this mount has been in front
// of us. Only a ghost has this line — asking for one about a live mount is a
// caller bug, the kind that would put a reason in the log for a decision that
// was never taken.
inline std::string ghostLine(const Facts& facts)
{
    if (decide(facts).call != Call::ghost)
        throw Error("mountprobe: a ghost's line asked for a mount that is not one");
    std::string line = "ghost mount: ";
    if (facts.inFlight) {
        line += "the uncached read has not come back in " + howLong(facts.outstanding)
              + " (the filesystem is wedged)";
    } else {
        line += "the uncached read did not answer " + howOften(facts.refusals) + " ("
              + std::to_string(facts.read.bytes) + " bytes, error "
              + std::to_string(facts.read.error) + ")";
    }
    line += facts.registryHasMedia ? "; the system still lists the medium"
                                   : "; the system no longer lists the medium";
    return line + "; this mount first seen " + howLong(facts.seenFor) + " ago";
}

// The ghosts the platform settles before the probe gets a word in: the mount
// going away under us, and a device registry with no medium behind it. Each
// backend names its own mechanism (statfs, sysfs, the drive root), so the
// sentence comes from there and only the journal's shape is kept here. A
// reason-less line is the silent-log bug this module exists to end.
inline std::string platformGhostLine(const std::string& whatTheBackendSaw)
{
    if (whatTheBackendSaw.empty())
        throw Error("mountprobe: a ghost's line with no reason in it");
    return "ghost mount: " + whatTheBackendSaw;
}

// What came of clearing the ghost. The forced unmount is the app pulling a
// volume out from under the player — the one thing in this story the player
// did not ask for — so it is written down whether it worked or not.
inline std::string forcedUnmountLine(bool ok, const std::string& message)
{
    const std::string note = message.empty() ? std::string() : " (" + message + ")";
    if (ok)
        return "ghost mount: cleared by a forced unmount" + note;
    return "ghost mount: the forced unmount did not go through" + note;
}

} // namespace loopercat::mountprobe
