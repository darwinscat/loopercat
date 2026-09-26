// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The storage register, read: what the pedal says about itself before the
// app asks it to hand over the card (issue #85).
//
// Register 7F 70 00 00 is the same one Connect writes 01 into (Sysex.hpp).
// Read with RQ1, it answers with a DT1 carrying one byte. Measured on both
// RC-5s and the RC-500, fw 1.10, 2026-09-22..25:
//
//   00  idle, everything saved — enter-storage works, the ready flag
//       (7F 70 00 01 = 01) arrives inside 100 ms, the volume ~1 s later;
//   01  in storage mode already;
//   02  playing a loop, OR the current memory holds a take not yet written
//       — the register cannot tell those two apart. Enter-storage is refused
//       at once (a DT1 "= 00" comes straight back), the register stays 02,
//       no medium appears. The pedal is protecting the unsaved take.
//
// The reply comes 0–100 ms after the request. Silence is not a state: after
// the pedal leaves storage mode its MIDI side re-enumerates, and a request
// sent into that gap is simply lost — which is what Connect's resend budget
// (Connect.hpp) exists for. So the caller owns the timeout, and this header
// only says whether a frame IS the answer, and what it says.
//
// Frames come from the same dialect as Sysex.hpp; nothing here is guessed.

#pragma once

#include "Error.hpp"
#include "Sysex.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace loopercat::storage {

// What the register reads back — the three values the hardware has shown.
enum class State : std::uint8_t {
    idle = 0x00,
    inStorage = 0x01,
    busy = 0x02, // playing, or an unsaved take — the pedal does not say which
};

// RQ1 for one byte at the storage register, addressed to one model:
//   F0 41 10 00 00 00 76 11 7F 70 00 00 00 00 00 01 10 F7   (RC-5)
//   F0 41 10 00 00 00 77 11 7F 70 00 00 00 00 00 01 10 F7   (RC-500)
// — the model sits outside the checksummed body, so the sum is the same.
inline std::vector<std::uint8_t> readRequest(const sysex::ModelId& model = sysex::kModelRc5)
{
    return sysex::rq1(sysex::kStorageModeAddress, { 0x00, 0x00, 0x00, 0x01 }, model);
}

// The answer's exact shape: F0 41 10 <model:4> 12 <address:4> <value> <checksum> F7.
inline constexpr std::size_t kReplyLength = 15;

// The answer of the pedal with THIS model id to readRequest(model), or no
// value for any frame that is not it — another model (an RC-500 answers with
// 77 where the RC-5 says 76, and with two pedals on the bus the other one's
// answer is not this one's), another address (the ready flag lives next door
// at 7F 70 00 01), a request rather than an answer, a wrong checksum, a
// frame cut short or padded, not Roland at all. A bus carries other traffic;
// those frames are simply not the answer, and the caller keeps listening
// until its own timeout.
//
// A DT1 to the register with a value the hardware has never shown is an
// Error naming the byte: an unknown state is not "idle by default".
inline std::optional<State> parseReply(std::span<const std::uint8_t> frame,
                                       const sysex::ModelId& model = sysex::kModelRc5)
{
    if (frame.size() != kReplyLength)
        return std::nullopt;
    if (frame[0] != sysex::kSysexStart || frame[kReplyLength - 1] != sysex::kSysexEnd)
        return std::nullopt;
    if (frame[1] != sysex::kRolandId || frame[2] != sysex::kDeviceId)
        return std::nullopt;
    for (std::size_t i = 0; i < model.size(); ++i)
        if (frame[3 + i] != model[i])
            return std::nullopt;
    if (frame[7] != sysex::kCmdDt1)
        return std::nullopt;
    for (std::size_t i = 0; i < sysex::kStorageModeAddress.size(); ++i)
        if (frame[8 + i] != sysex::kStorageModeAddress[i])
            return std::nullopt;
    const std::uint8_t value = frame[12];
    if (value > 0x7F)
        return std::nullopt; // not a 7-bit body byte: not a sysex payload at all
    std::vector<std::uint8_t> body(sysex::kStorageModeAddress.begin(), sysex::kStorageModeAddress.end());
    body.push_back(value);
    if (frame[13] != sysex::checksum(body))
        return std::nullopt;
    switch (value) {
    case 0x00: return State::idle;
    case 0x01: return State::inStorage;
    case 0x02: return State::busy;
    default: break;
    }
    static constexpr char digits[] = "0123456789abcdef";
    throw Error(std::string("the pedal's storage register reads 0x") + digits[value >> 4] + digits[value & 0x0F]
                + " \xe2\x80\x94 a state this app does not know");
}

// For logs and probes — not the words a player sees (those are the window's).
inline const char* describe(State state)
{
    switch (state) {
    case State::idle: return "idle (00)";
    case State::inStorage: return "in storage mode (01)";
    case State::busy: return "busy (02): playing, or an unsaved take";
    }
    return "?";
}

} // namespace loopercat::storage
