// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The read-only slot view derived from a memory file: what the browser table
// shows. Pure — memory text in, model out; file I/O stays with the caller.
//
// Field semantics source: rc5cat lib/commands.js listSlots (WavStat=1 means
// indexed audio, WavLen is the frame count, Tempo is tenths of BPM, One is
// the One Shot flag), plus the 2026-07-24 hardware analysis (#10): MeasLen is
// the true bar count the pedal displays; the stored Measure field is
// MeasLen + 7 (the first seven values are the UI's special modes).

#pragma once

#include "Rc0.hpp"

#include <string>
#include <vector>

namespace loopercat::catalog {

struct SlotInfo {
    int slot;               // 1-based, as shown on the pedal display
    std::string name;       // 12 chars, space-padded as stored
    bool hasAudio;          // WavStat == 1: the pedal has indexed a loop here
    long long frames;       // WavLen: sample frames at 44.1 kHz
    bool oneShot;           // One == 1
    long long tempoTenths;  // Tempo: tenths of BPM
    long long measures;     // MeasLen: whole bars, as the pedal displays them

    bool operator==(const SlotInfo&) const = default;
};

inline SlotInfo readSlot(std::string_view memoryText, int slot)
{
    const std::string body = rc0::slotBody(memoryText, slot);
    return { slot,
             rc0::decodeName(body),
             rc0::field(body, "WavStat") == 1,
             rc0::field(body, "WavLen"),
             rc0::field(body, "One") == 1,
             rc0::field(body, "Tempo"),
             rc0::field(body, "MeasLen") };
}

inline std::vector<SlotInfo> listSlots(std::string_view memoryText)
{
    std::vector<SlotInfo> out;
    out.reserve(rc0::kSlotCount);
    for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
        out.push_back(readSlot(memoryText, slot));
    return out;
}

} // namespace loopercat::catalog
