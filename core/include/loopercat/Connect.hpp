// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The connect attempt as a pure state machine (issue #2).
//
// Connect used to be fire-and-forget: one enter-storage sysex, then hope.
// Two observed silent failures (RC-5, fw 1.10): a frame sent while the
// pedal's MIDI side is still re-enumerating after a disconnect is simply
// lost — a second send after a short pause always works; and a pedal that is
// playing a loop (reasonably) refuses to surrender the medium — no retry can
// help, only the musician stopping the loop. The machine owns the
// retry-and-budget policy for the first case and the honest give-up for the
// second; the app tier owns clocks, timers and the actual MIDI send.
//
// Timeline: begin() counts the caller's first send; no disk-appeared within
// resendAfterMs -> sendFrame again, up to sendBudget sends total; still no
// disk one window after the last send -> giveUp, and the attempt is over.
// diskAppeared() ends the resending: the pedal surrendered the medium, and
// from there the mount + scan pipeline owns (and already reports) failures —
// the attempt merely stays `active` so the owner keeps Connect disabled
// until the volume is honestly up (or the mount reports its own error).

#pragma once

#include "Error.hpp"

#include <cstdint>

namespace loopercat::connect {

// What the owner must do after feeding the machine a clock tick.
enum class Action {
    none,      // keep waiting
    sendFrame, // (re)send the enter-storage request
    giveUp,    // budget exhausted, no disk — surface the honest failure
};

struct Config {
    std::int64_t resendAfterMs = 2000; // the observed re-enumeration gap
    int sendBudget = 3;                // sends per attempt, first included
};

class Attempt {
public:
    explicit Attempt(Config config = {}) : config_(config)
    {
        if (config_.resendAfterMs <= 0)
            throw Error("connect attempt: resendAfterMs must be positive");
        if (config_.sendBudget < 1)
            throw Error("connect attempt: sendBudget must be at least 1");
    }

    bool active() const { return phase_ != Phase::idle; }
    int sendsUsed() const { return sendsUsed_; }

    // Start the attempt; the caller sends the first frame, the machine
    // counts it. begin() while active is a caller bug — the owner gates the
    // Connect button on active().
    void begin(std::int64_t nowMs)
    {
        if (active())
            throw Error("connect attempt already in flight");
        phase_ = Phase::sending;
        sendsUsed_ = 1;
        lastSendAtMs_ = nowMs;
    }

    // One clock tick; the owner performs the returned action. The clock must
    // be monotonic — time flowing backwards would silently stall the attempt,
    // so it throws instead.
    [[nodiscard]] Action tick(std::int64_t nowMs)
    {
        if (phase_ != Phase::sending)
            return Action::none;
        if (nowMs < lastSendAtMs_)
            throw Error("connect attempt: the clock went backwards");
        if (nowMs - lastSendAtMs_ < config_.resendAfterMs)
            return Action::none;
        if (sendsUsed_ < config_.sendBudget) {
            ++sendsUsed_;
            lastSendAtMs_ = nowMs;
            return Action::sendFrame;
        }
        phase_ = Phase::idle;
        return Action::giveUp;
    }

    // A disk appeared. An event from the world, not a protocol call: while
    // sending it means the pedal heard us — stop resending; while idle (a
    // by-hand STORAGE entry, no attempt running) it is simply not ours.
    void diskAppeared()
    {
        if (phase_ == Phase::sending)
            phase_ = Phase::mounting;
    }

    // The attempt is over — the volume is honestly up, the mount reported
    // its own failure, or the app is closing. Idempotent.
    void finish() { phase_ = Phase::idle; }

private:
    enum class Phase {
        idle,     // no attempt
        sending,  // frames go out on the resend clock
        mounting, // the disk appeared; the mount + scan pipeline owns it now
    };

    Config config_;
    Phase phase_ = Phase::idle;
    int sendsUsed_ = 0;
    std::int64_t lastSendAtMs_ = 0;
};

} // namespace loopercat::connect
