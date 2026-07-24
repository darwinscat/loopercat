// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The sysex builder against the wire truth: every golden frame below is a
// byte-for-byte copy from the 2026-07-24 live capture of BOSS Tone Studio
// talking to a real RC-5 (docs/midi-protocol/captures/, issue #22) — the
// builder must reproduce the hardware dialect exactly, not its own idea of it.

#include "support.hpp"

#include <loopercat/Sysex.hpp>

using namespace loopercat;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<int> values)
{
    std::vector<std::uint8_t> out;
    for (const int v : values)
        out.push_back(static_cast<std::uint8_t>(v));
    return out;
}

} // namespace

int main()
{
    // --- golden frames from the capture ---

    // Tone Studio asks for the storage-mode register (RQ1, size 1).
    CHECK(sysex::rq1(sysex::kStorageModeAddress, { 0x00, 0x00, 0x00, 0x01 })
          == bytes({ 0xF0, 0x41, 0x10, 0x00, 0x00, 0x00, 0x76, 0x11,
                     0x7F, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x10, 0xF7 }));

    // The write that puts the pedal into STORAGE mode ("Connecting").
    CHECK(sysex::enterStorageMode()
          == bytes({ 0xF0, 0x41, 0x10, 0x00, 0x00, 0x00, 0x76, 0x12,
                     0x7F, 0x70, 0x00, 0x00, 0x01, 0x10, 0xF7 }));

    // The write that returns it to the looper screen — identical to the
    // pedal's own "value is 0" reply frame in the capture.
    CHECK(sysex::exitStorageMode()
          == bytes({ 0xF0, 0x41, 0x10, 0x00, 0x00, 0x00, 0x76, 0x12,
                     0x7F, 0x70, 0x00, 0x00, 0x00, 0x11, 0xF7 }));

    // The pedal's ready announcement (checksum 0x0F on the wire).
    CHECK(sysex::dt1(sysex::kStorageReadyAddress, { 0x01 })
          == bytes({ 0xF0, 0x41, 0x10, 0x00, 0x00, 0x00, 0x76, 0x12,
                     0x7F, 0x70, 0x00, 0x01, 0x01, 0x0F, 0xF7 }));

    // --- checksum properties ---

    CHECK_EQ(static_cast<int>(sysex::checksum({ 0x7F, 0x70, 0x00, 0x00, 0x01 })), 0x10);
    CHECK_EQ(static_cast<int>(sysex::checksum({ 0x7F, 0x01 })), 0); // sum 128 wraps to 0
    CHECK_EQ(static_cast<int>(sysex::checksum({})), 0);             // empty sum wraps too
    CHECK_THROWS(sysex::checksum({ 0x80 }), "7-bit");               // sysex bodies are 7-bit

    // --- malformed requests are refused, not guessed at ---

    CHECK_THROWS(sysex::dt1(sysex::kStorageModeAddress, {}), "empty payload");
    CHECK_THROWS(sysex::dt1(sysex::kStorageModeAddress, { 0x80 }), "7-bit");

    return testkit::summary("sysex");
}
