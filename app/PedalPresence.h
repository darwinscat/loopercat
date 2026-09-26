// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/DeviceProfile.hpp>
#include <loopercat/Error.hpp>

#include <string>
#include <string_view>
#include <vector>

//==============================================================================
// loopercat::presence — what the empty window says about the pedals on the
// USB bus, and whether Connect is offered at all.
//
// A pedal outside storage mode is visible as a USB-MIDI device, so the app
// can tell "no pedal" from "pedal here, wrong mode" before anything is
// mounted. Which pedals count is the profile table's call, not a name's:
// Connect is offered only to a model whose card this build can read
// (profile::allows(Operation::read)) — entering storage mode is a write to
// the pedal, and walking a pedal into a mode whose card the app would then
// refuse is a trip for nothing. A model known to the table but closed for
// reading is named for what it is; a looper the table does not know at all
// is not counted here (PedalPortName decides what is a looper).
//
// Pure over the list, so the words are tested without a bus.
//==============================================================================
namespace loopercat::presence
{

struct Seen {
    std::string family; // profile::familyName
    bool readable;      // profile allows Operation::read
};

struct Verdict {
    bool connectable = false; // Connect makes sense: at least one readable pedal
    std::string hint;         // the empty-state line under the header
    std::string status;       // the status strip while nothing is mounted
};

inline Seen seen(const profile::DeviceProfile& family)
{
    return { std::string(family.familyName), family.allows(profile::Operation::read) };
}

inline Verdict describe(const std::vector<Seen>& pedals)
{
    Verdict out;
    std::vector<std::string> readable, closed;
    for (const Seen& p : pedals) {
        if (p.family.empty())
            throw Error("a pedal on the bus needs its family name");
        (p.readable ? readable : closed).push_back(p.family);
    }
    if (!readable.empty()) {
        out.connectable = true;
        if (readable.size() == 1) {
            out.hint = readable.front() + " detected \xe2\x80\x94 click Connect to browse loops";
            out.status = readable.front() + " on USB \xe2\x80\x94 ready to connect";
        } else {
            const std::string n = std::to_string(readable.size());
            out.hint = n + " loopers detected \xe2\x80\x94 click Connect and choose one";
            out.status = n + " loopers on USB \xe2\x80\x94 ready to connect";
        }
        return out;
    }
    if (!closed.empty()) {
        out.hint = closed.front() + " detected \xe2\x80\x94 " + profile::onlySpeaks();
        out.status = closed.front() + " on USB \xe2\x80\x94 not readable yet";
        return out;
    }
    out.hint = "Connect your looper via USB";
    out.status = "No looper found";
    return out;
}

} // namespace loopercat::presence
