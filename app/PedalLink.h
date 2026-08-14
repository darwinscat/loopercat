// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Sysex.hpp>

#include <juce_audio_devices/juce_audio_devices.h>

#include <optional>

//==============================================================================
// loopercat::pedallink — the pedal's MIDI face: find the RC-5 endpoint and
// flip its storage mode. The frames come from core/Sysex.hpp (wire-verified,
// issue #22); this layer only locates the device and sends. The pedal's MIDI
// endpoint is present in BOTH modes, so Connect works from the looper screen
// and Disconnect works from storage.
//
// Finding the pedal is JUCE's job everywhere: enumeration has never been in
// doubt on any platform. SENDING is a per-platform backend, because on Linux
// JUCE's is not dependable — see PedalLinkLinux.cpp for what was measured.
//==============================================================================
namespace loopercat::pedallink {

// The RC-5's MIDI output endpoint, if the pedal is on the bus. MESSAGE THREAD.
inline std::optional<juce::MidiDeviceInfo> findPedal()
{
    for (const auto& device : juce::MidiOutput::getAvailableDevices())
        if (device.name.containsIgnoreCase("RC-5"))
            return device;
    return std::nullopt;
}

// Send the storage-mode switch. Returns an error message; empty = sent.
// MESSAGE THREAD. Implemented once per platform.
juce::String requestStorageMode(bool enter);

} // namespace loopercat::pedallink
