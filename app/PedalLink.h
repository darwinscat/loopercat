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
inline juce::String requestStorageMode(bool enter)
{
    const auto device = findPedal();
    if (!device)
        return "no RC-5 MIDI device on the bus";
    const auto out = juce::MidiOutput::openDevice(device->identifier);
    if (out == nullptr)
        return "cannot open MIDI device " + device->name;
    const auto frame = enter ? sysex::enterStorageMode() : sysex::exitStorageMode();
    // JUCE wraps the payload in F0/F7 itself.
    out->sendMessageNow(juce::MidiMessage::createSysExMessage(
        frame.data() + 1, static_cast<int>(frame.size()) - 2));
    return {};
}

} // namespace loopercat::pedallink
