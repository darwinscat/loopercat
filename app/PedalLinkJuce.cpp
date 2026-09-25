// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The macOS and Windows sender: JUCE's MidiOutput, which does the job on
// both. (Linux has its own backend — PedalLinkLinux.cpp says why.)

#include "PedalLink.h"

namespace loopercat::pedallink {

juce::String requestStorageMode(bool enter, const juce::MidiDeviceInfo& pedal)
{
    // Opened by identifier, never by name: with two RC-5s the names are equal
    // and only the identifier says which pedal this is.
    const auto out = juce::MidiOutput::openDevice(pedal.identifier);
    if (out == nullptr)
        return "cannot open MIDI device " + pedal.name + " (" + pedal.identifier + ")";
    const auto frame = enter ? sysex::enterStorageMode() : sysex::exitStorageMode();
    // JUCE wraps the payload in F0/F7 itself.
    out->sendMessageNow(juce::MidiMessage::createSysExMessage(
        frame.data() + 1, static_cast<int>(frame.size()) - 2));
    return {};
}

juce::String requestStorageMode(bool enter)
{
    const auto device = findPedal();
    if (!device)
        return "no RC-5 MIDI device on the bus";
    return requestStorageMode(enter, *device);
}

} // namespace loopercat::pedallink
