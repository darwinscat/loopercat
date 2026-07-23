// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pedal's connection lifecycle as a pure state machine.
//
// Born from a field incident (2026-07-22): macOS kept serving a pedal volume
// from page cache after the pedal was physically detached — path checks
// passed, writes "succeeded", playback continued, and none of it could ever
// reach the hardware. The lesson: path truth (a pedal-shaped directory tree)
// and device truth (a live external device behind it) are independent inputs,
// and the pedal is CONNECTED only while both agree. Everything platform-bound
// (IOKit/DiskArbitration on macOS) stays in the app tier and feeds events in;
// this machine owns the policy.
//
// Eject is a protocol, not a wish: a request is only legal while connected,
// a completion only while ejecting. Anything else is a caller bug and throws —
// the machine never guesses.

#pragma once

#include "Error.hpp"

#include <string>

namespace loopercat::lifecycle {

// What backs the pedal path, per the platform device watcher's verdict
// delivered with each scan.
enum class Backing {
    live,      // mounted volume with a live external device behind it
    gone,      // the mount still serves content but the device is detached
    untracked, // not an external device at all (synthetic/test volume) — trusted
};

enum class State {
    disconnected, // no pedal volume anywhere
    connected,    // path and device agree — the only state where I/O is honest
    ghost,        // path serves cached content, device is gone; nothing may touch it
    ejecting,     // unmount requested, completion pending
    ejected,      // unmounted cleanly; device may still be attached ("safe to disconnect")
};

inline const char* stateName(State s)
{
    switch (s) {
    case State::disconnected: return "disconnected";
    case State::connected: return "connected";
    case State::ghost: return "ghost";
    case State::ejecting: return "ejecting";
    case State::ejected: return "ejected";
    }
    throw Error("unknown lifecycle state");
}

class Machine {
public:
    State state() const { return state_; }

    // Mutations and playback are honest only while connected.
    bool writable() const { return state_ == State::connected; }
    bool playable() const { return state_ == State::connected; }

    // A ghost mount must be force-unmounted before the pedal can come back.
    bool needsCleanup() const { return state_ == State::ghost; }

    // One scan cycle found a pedal-shaped volume, with the watcher's verdict
    // on what backs it. While ejecting, scans are stale by definition (they
    // race the unmount) and are absorbed; the eject completion resolves.
    void pedalSeen(Backing backing)
    {
        if (state_ == State::ejecting)
            return;
        switch (backing) {
        case Backing::live:
        case Backing::untracked:
            state_ = State::connected;
            return;
        case Backing::gone:
            state_ = State::ghost;
            return;
        }
        throw Error("unknown backing verdict");
    }

    // One scan cycle found no pedal volume. After an eject the path being
    // gone is the expected condition, not a disconnect — "safe to disconnect"
    // persists until the device actually detaches or the volume returns.
    void pedalMissing()
    {
        switch (state_) {
        case State::disconnected:
        case State::ejecting:
        case State::ejected:
            return;
        case State::connected:
        case State::ghost:
            state_ = State::disconnected;
            return;
        }
        throw Error("unknown lifecycle state");
    }

    // Async device-termination notification for the device backing the
    // current volume; may arrive between scans. While ejecting it is only
    // recorded — the eject completion decides where to land.
    void deviceLost()
    {
        switch (state_) {
        case State::disconnected:
        case State::ghost:
            return;
        case State::connected:
            state_ = State::ghost;
            return;
        case State::ejecting:
            deviceLostWhileEjecting_ = true;
            return;
        case State::ejected:
            state_ = State::disconnected;
            return;
        }
        throw Error("unknown lifecycle state");
    }

    void ejectRequested()
    {
        if (state_ != State::connected)
            throw Error(std::string("eject requested while ") + stateName(state_)
                        + " — eject is only legal while connected");
        state_ = State::ejecting;
        deviceLostWhileEjecting_ = false;
    }

    void ejectFinished(bool unmounted)
    {
        if (state_ != State::ejecting)
            throw Error(std::string("eject completion while ") + stateName(state_)
                        + " — no eject was in flight");
        if (unmounted)
            state_ = deviceLostWhileEjecting_ ? State::disconnected : State::ejected;
        else
            state_ = deviceLostWhileEjecting_ ? State::ghost : State::connected;
    }

private:
    State state_ = State::disconnected;
    bool deviceLostWhileEjecting_ = false;
};

} // namespace loopercat::lifecycle
