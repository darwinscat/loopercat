// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PedalPortName.h"

#include <loopercat/Sysex.hpp>

#include <juce_audio_devices/juce_audio_devices.h>

#include <optional>
#include <vector>

//==============================================================================
// loopercat::pedallink — the pedal's MIDI face: find the RC-5 endpoints and
// flip one's storage mode. The frames come from core/Sysex.hpp (wire-verified,
// issue #22); this layer only locates the device and sends. The pedal's MIDI
// endpoint is present in BOTH modes, so Connect works from the looper screen
// and Disconnect works from storage.
//
// Finding the pedal is JUCE's job everywhere: enumeration has never been in
// doubt on any platform. SENDING is a per-platform backend, because on Linux
// JUCE's is not dependable — see PedalLinkLinux.cpp for what was measured.
//
// Two RC-5s on one bus are named identically by the OS ("BOSS_RC-5" and
// "BOSS_RC-5", measured 2026-09-24), and nothing the pedal reports tells
// them apart — USB serial, MIDI identity and volume label are all the same.
// What differs is `identifier`: the OS's own handle for the endpoint (the
// CoreMIDI uniqueID on macOS, ALSA's client-port on Linux), the one
// per-pedal signal there is before either is in STORAGE. So this layer hands
// out ALL the pedals with their identifiers and sends to the one it is given;
// choosing between them is the caller's (issue #98).
//==============================================================================
namespace loopercat::pedallink {

// Every RC-5 output endpoint among `devices`, in the order given — never
// another RC model that shares the prefix (PedalPortName.h). Pure over the
// list, so a test can hand it a bus.
inline std::vector<juce::MidiDeviceInfo> pedalsAmong(const juce::Array<juce::MidiDeviceInfo>& devices)
{
    std::vector<juce::MidiDeviceInfo> pedals;
    for (const auto& device : devices)
        if (portname::isRc5(device.name.toStdString()))
            pedals.push_back(device);
    return pedals;
}

// The RC-5 endpoints on the bus right now. MESSAGE THREAD.
inline std::vector<juce::MidiDeviceInfo> findPedals()
{
    return pedalsAmong(juce::MidiOutput::getAvailableDevices());
}

// The first of them, for callers that have not learned to choose. With two
// RC-5s this is whichever the OS listed first — on 2026-09-24 the new pedal,
// not the main one — which is why it is named for what it is. MESSAGE THREAD.
inline std::optional<juce::MidiDeviceInfo> findPedal()
{
    const auto pedals = findPedals();
    if (pedals.empty())
        return std::nullopt;
    return pedals.front();
}

// Send the storage-mode switch to ONE endpoint — one of findPedals(), chosen
// by the caller. Returns an error message; empty = sent. MESSAGE THREAD.
// Implemented once per platform.
juce::String requestStorageMode(bool enter, const juce::MidiDeviceInfo& pedal);

// The same, to the first RC-5 on the bus (findPedal()).
juce::String requestStorageMode(bool enter);

} // namespace loopercat::pedallink
