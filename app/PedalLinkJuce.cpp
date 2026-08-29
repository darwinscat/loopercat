// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The macOS and Windows sender: JUCE's MidiOutput, which does the job on
// both. (Linux has its own backend — PedalLinkLinux.cpp says why.)

#include "PedalLink.h"

namespace loopercat::pedallink {

juce::String requestStorageMode(bool enter)
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
