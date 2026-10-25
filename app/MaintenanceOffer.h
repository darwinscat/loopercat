// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "CardPermissions.h"

#include <loopercat/Lifecycle.hpp>

#include <string>

//==============================================================================
// loopercat::maintenance — whether the card sweep is on offer, and why not.
//
// The sweep used to be a menu item whose gates were two lambdas in the window:
// the pedal connected and idle, and the card of a model this app writes to.
// The menu went in 0.9.6 and the button moved to a Settings tab, so the gates
// became this — one function of what the window knows, with the sentence the
// tab shows when it says no. The tab is a face over it and judges nothing, so
// a screen cannot drift from the gate the core refuses by.
//
// A greyed button with no reason beside it is a support thread waiting to
// happen, so every no names its own cause, in the order a player meets them:
// no pedal first (the card's model is not even known then), then a pedal with
// its hands full, then a card this app only reads.
//==============================================================================
namespace loopercat::maintenance
{

// What the window knows when the tab is drawn or refreshed.
struct Pedal {
    bool connected = false; // an honest volume is up: lifecycle connected, no error
    bool busy = false;      // a job of ours is running on it
    std::string family;     // the card's own name for its model; empty until one is read
};

struct Offer {
    bool enabled = false;
    std::string why; // empty when enabled, and only then
};

// What the window knows, read off the snapshot it holds. A lifecycle state
// of connected is not enough on its own: a volume that is there and
// unreadable carries an error, and no write may be offered on it (the gate
// the Maintenance menu carried, MainComponent::updateToolbar).
inline Pedal pedalFrom(lifecycle::State state, const std::string& error, bool busy,
                       std::string family)
{
    return { state == lifecycle::State::connected && error.empty(), busy, std::move(family) };
}

inline Offer offer(const Pedal& pedal)
{
    if (!pedal.connected)
        return { false, "Connect the pedal to clean its card." };
    if (pedal.busy)
        return { false, "The pedal is busy. This waits for the job to finish." };
    // The sweep writes to the card, so a card this app only reads keeps its
    // junk; the words are the ones every read-only screen uses.
    if (!CardPermissions::of(pedal.family).anyWrite())
        return { false, CardPermissions::writesOnlyTo() + ". This card keeps its junk." };
    return { true, {} };
}

} // namespace loopercat::maintenance
