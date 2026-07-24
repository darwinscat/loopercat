// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The RC-5's MIDI sysex dialect: Roland DT1/RQ1 framing, and the one register
// we have cracked so far — the storage-mode switch.
//
// Knowledge source: a live capture of BOSS Tone Studio's handshake
// (2026-07-24, snoize MIDI Monitor; raw log in docs/midi-protocol/captures/),
// verified by replaying hand-built frames at the hardware: writing 01 to
// register 7F 70 00 00 puts the pedal into STORAGE mode ("Connecting" on the
// display, medium exported); writing 00 returns it to the looper screen. The
// pedal acks a write and then announces 7F 70 00 01 = 01 as a ready flag.
// Details: repo issue #22.
//
// Framing (classic Roland):
//   F0 41 <device> <model:4> <11=RQ1|12=DT1> <addr:4> <payload> <checksum> F7
// where RQ1's payload is a 4-byte size, DT1's payload is the data, and the
// checksum is 128 - (sum of address+payload bytes mod 128), with 128 -> 0.

#pragma once

#include "Error.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace loopercat::sysex {

inline constexpr std::uint8_t kSysexStart = 0xF0;
inline constexpr std::uint8_t kSysexEnd = 0xF7;
inline constexpr std::uint8_t kRolandId = 0x41;
inline constexpr std::uint8_t kDeviceId = 0x10; // factory default unit number
inline constexpr std::array<std::uint8_t, 4> kModelRc5 { 0x00, 0x00, 0x00, 0x76 };

inline constexpr std::uint8_t kCmdRq1 = 0x11; // read request
inline constexpr std::uint8_t kCmdDt1 = 0x12; // data set

// The storage-mode switch register (the "Connect" in Tone Studio's sense),
// and the ready flag the pedal raises next to it.
inline constexpr std::array<std::uint8_t, 4> kStorageModeAddress { 0x7F, 0x70, 0x00, 0x00 };
inline constexpr std::array<std::uint8_t, 4> kStorageReadyAddress { 0x7F, 0x70, 0x00, 0x01 };

// Roland checksum over address+payload: 128 - (sum mod 128), wrapping to 0.
inline std::uint8_t checksum(const std::vector<std::uint8_t>& addressAndPayload)
{
    unsigned sum = 0;
    for (const std::uint8_t byte : addressAndPayload) {
        if (byte > 0x7F)
            throw Error("sysex body byte out of 7-bit range: " + std::to_string(byte));
        sum += byte;
    }
    return static_cast<std::uint8_t>((128 - (sum % 128)) % 128);
}

namespace detail {

    inline std::vector<std::uint8_t> frame(std::uint8_t command,
                                           const std::array<std::uint8_t, 4>& address,
                                           const std::vector<std::uint8_t>& payload)
    {
        if (payload.empty())
            throw Error("sysex frame with an empty payload");
        std::vector<std::uint8_t> body(address.begin(), address.end());
        body.insert(body.end(), payload.begin(), payload.end());
        std::vector<std::uint8_t> out { kSysexStart, kRolandId, kDeviceId };
        out.insert(out.end(), kModelRc5.begin(), kModelRc5.end());
        out.push_back(command);
        out.insert(out.end(), body.begin(), body.end());
        out.push_back(checksum(body));
        out.push_back(kSysexEnd);
        return out;
    }

} // namespace detail

// DT1: write `data` starting at `address`.
inline std::vector<std::uint8_t> dt1(const std::array<std::uint8_t, 4>& address,
                                     const std::vector<std::uint8_t>& data)
{
    return detail::frame(kCmdDt1, address, data);
}

// RQ1: ask for `size` bytes starting at `address`.
inline std::vector<std::uint8_t> rq1(const std::array<std::uint8_t, 4>& address,
                                     const std::array<std::uint8_t, 4>& size)
{
    return detail::frame(kCmdRq1, address, { size.begin(), size.end() });
}

// The two frames behind the app's Connect / Disconnect.
inline std::vector<std::uint8_t> enterStorageMode() { return dt1(kStorageModeAddress, { 0x01 }); }
inline std::vector<std::uint8_t> exitStorageMode() { return dt1(kStorageModeAddress, { 0x00 }); }

} // namespace loopercat::sysex
