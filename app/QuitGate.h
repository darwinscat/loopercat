// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>
#include <loopercat/Lifecycle.hpp>

#include <functional>
#include <utility>

//==============================================================================
// loopercat::QuitGate — quit is Disconnect (issue #1).
//
// Quitting while the app holds the pedal's volume used to strand the pedal
// in STORAGE: the companion exited, the mount stayed, the looper never came
// back. The gate owns the two decisions that make quit behave like the
// Disconnect button: WHICH lifecycle states hold the quit (only connected —
// start a disconnect — and ejecting — join the one in flight), and the
// exactly-once continuation, because the eject completion and the time bound
// race and both may land. Pure policy, JUCE-free: the owner wires the actual
// disconnect, the timer and the app exit.
//==============================================================================
namespace loopercat
{

class QuitGate {
public:
    enum class Plan {
        quitNow,         // nothing holds the quit — exit immediately
        startDisconnect, // connected: run the Disconnect path, then finish()
        joinEject,       // an eject is already in flight: its completion finishes
        alreadyPending,  // a second quit request while one is under way
    };

    // Decide from the lifecycle state; arms the gate unless the answer is
    // quitNow. The continuation is the whole point — requesting without one
    // is a caller bug.
    Plan request(lifecycle::State state, std::function<void()> done)
    {
        if (done_)
            return Plan::alreadyPending;
        switch (state) {
        case lifecycle::State::connected:
            done_ = takeContinuation(std::move(done));
            return Plan::startDisconnect;
        case lifecycle::State::ejecting:
            done_ = takeContinuation(std::move(done));
            return Plan::joinEject;
        case lifecycle::State::disconnected:
        case lifecycle::State::ghost:
        case lifecycle::State::ejected:
            return Plan::quitNow;
        }
        throw Error("unknown lifecycle state");
    }

    bool pending() const { return static_cast<bool>(done_); }

    // The release finished — eject completed (either way) or the time bound
    // fired. First caller wins, the rest are no-ops; when idle, a no-op too.
    void finish()
    {
        if (!done_)
            return;
        const std::function<void()> done = std::move(done_);
        done_ = nullptr;
        done();
    }

private:
    static std::function<void()> takeContinuation(std::function<void()> done)
    {
        if (!done)
            throw Error("quit gate armed without a continuation");
        return done;
    }

    std::function<void()> done_;
};

} // namespace loopercat
