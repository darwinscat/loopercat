// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PedalPortName.h"

#include <loopercat/DeviceProfile.hpp>
#include <loopercat/StorageRegister.hpp>
#include <loopercat/Sysex.hpp>

#include <juce_audio_devices/juce_audio_devices.h>

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

//==============================================================================
// loopercat::pedallink — the pedals' MIDI face: find the RC endpoints on the
// bus and flip one's storage mode. The frames come from core/Sysex.hpp
// (wire-verified, issue #22); this layer only locates the device and sends.
// A pedal's MIDI endpoint is present in BOTH modes, so Connect works from the
// looper screen and Disconnect works from storage.
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
//
// The family: every RC model announces itself by name ("BOSS_RC-5",
// "BOSS_RC-500"), and the profile table (DeviceProfile.hpp) says which of
// those this app has a profile for, and what each one's model id is. A
// Pedal carries its profile, so a frame goes out with THAT pedal's model
// id — the RC-500 ignores the RC-5's in silence, and the other way round —
// and an answer is judged against it. The RC-5-only forms remain for the
// callers that mean the RC-5.
//==============================================================================
namespace loopercat::pedallink {

// ---------------------------------------------------------------------------
// The RC-5 alone — the forms the app was written around.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// The family.
// ---------------------------------------------------------------------------

// One endpoint of a pedal this app has a profile for.
struct Pedal {
    juce::MidiDeviceInfo endpoint;          // the name the OS gave, and its identifier
    const profile::DeviceProfile* profile;  // never null: the model the name announces

    std::string_view family() const { return profile->familyName; }
    const sysex::ModelId& modelId() const { return profile->modelId; }
};

// Every endpoint among `devices` whose name announces a model in the profile
// table, in the order given, each with its profile. An RC model without a
// profile (the RC-600, say) is not here: it is announced to the player by
// the window, not spoken to. Pure over the list.
inline std::vector<Pedal> familyAmong(const juce::Array<juce::MidiDeviceInfo>& devices)
{
    std::vector<Pedal> pedals;
    for (const auto& device : devices)
        if (const auto* found = portname::familyProfile(device.name.toStdString()))
            pedals.push_back(Pedal { device, found });
    return pedals;
}

// The Pedal an endpoint is — or an Error, for an endpoint whose name
// announces no model this app has a profile for.
inline Pedal pedalOf(const juce::MidiDeviceInfo& endpoint)
{
    const auto* found = portname::familyProfile(endpoint.name.toStdString());
    if (found == nullptr)
        throw Error("MIDI device " + endpoint.name.toStdString() + " (" + endpoint.identifier.toStdString()
                    + ") is not an RC model this app has a profile for");
    return Pedal { endpoint, found };
}

namespace detail {

    // CoreMIDI can hand back an EMPTY endpoint list right after the client is
    // created — seen many times on the probe (2026-09-22): one re-enumeration
    // fixes it. So an empty first answer is asked once more, a moment later;
    // a bus that is honestly empty costs that moment and nothing else.
    inline constexpr int kEnumerationRetryMs = 100;

    template <typename Filter>
    auto enumerateTwice(const std::function<juce::Array<juce::MidiDeviceInfo>()>& list, Filter filter)
    {
        auto pedals = filter(list());
        if (pedals.empty()) {
            juce::Thread::sleep(kEnumerationRetryMs);
            pedals = filter(list());
        }
        return pedals;
    }

} // namespace detail

// The RC-5 OUTPUT endpoints on the bus right now — where frames are sent.
// MESSAGE THREAD or a worker; blocks for the retry moment when the bus looks empty.
inline std::vector<juce::MidiDeviceInfo> findPedals()
{
    return detail::enumerateTwice([] { return juce::MidiOutput::getAvailableDevices(); }, pedalsAmong);
}

// The RC-5 INPUT endpoints — where the pedal's answers arrive. An input's
// identifier is not its output's (they are different endpoints of one
// device), and nothing in the OS pairs them; the storage query below opens
// every input of the pedal's family and lets the answer tell.
inline std::vector<juce::MidiDeviceInfo> findPedalInputs()
{
    return detail::enumerateTwice([] { return juce::MidiInput::getAvailableDevices(); }, pedalsAmong);
}

// Every profiled RC OUTPUT endpoint on the bus, with its profile.
inline std::vector<Pedal> findFamily()
{
    return detail::enumerateTwice([] { return juce::MidiOutput::getAvailableDevices(); }, familyAmong);
}

// Every profiled RC INPUT endpoint on the bus, with its profile.
inline std::vector<Pedal> findFamilyInputs()
{
    return detail::enumerateTwice([] { return juce::MidiInput::getAvailableDevices(); }, familyAmong);
}

// The first RC-5, for callers that have not learned to choose. With two
// RC-5s this is whichever the OS listed first — on 2026-09-24 the new pedal,
// not the main one — which is why it is named for what it is. MESSAGE THREAD.
inline std::optional<juce::MidiDeviceInfo> findPedal()
{
    const auto pedals = findPedals();
    if (pedals.empty())
        return std::nullopt;
    return pedals.front();
}

// ---------------------------------------------------------------------------
// The storage switch. Entering is a WRITE to the pedal: it leaves the looper
// screen and hands its card to the computer; leaving returns it. Whether a
// pedal's CARD may then be read is the profile's word (allows(read)), not
// this layer's — a caller that sends the frame to a model whose card the app
// refuses gets a mounted volume the scan will turn away, and a pedal that
// stays in storage until Disconnect.
// ---------------------------------------------------------------------------

// Send the storage-mode switch to ONE pedal — one of findFamily(), chosen by
// the caller — with that pedal's own model id. Returns an error message;
// empty = sent. MESSAGE THREAD. Implemented once per platform.
juce::String requestStorageMode(bool enter, const Pedal& pedal);

// The same, to an endpoint of any profiled model (the profile is read from
// the name) — the form callers with a bare MidiDeviceInfo use.
inline juce::String requestStorageMode(bool enter, const juce::MidiDeviceInfo& endpoint)
{
    try {
        return requestStorageMode(enter, pedalOf(endpoint));
    } catch (const Error& e) {
        return e.what();
    }
}

// The same, to the first RC-5 on the bus (findPedal()).
juce::String requestStorageMode(bool enter);

//==============================================================================
// Asking the pedal why (issue #85). Before the first enter-storage frame the
// register 7F 70 00 00 says whether the pedal can hand over its card at all:
// 02 means it is playing or holds an unsaved take, and no resend will change
// that — the player has to stop and WRITE. StorageRegister.hpp owns the
// bytes; this is the exchange.
//==============================================================================

// What a query comes back with. noAnswer is its own thing, never busy: a
// pedal whose MIDI side is re-enumerating after leaving storage mode says
// nothing at all, and the right move there is Connect's resend budget, not
// a refusal on the screen.
enum class StorageQuery { idle, inStorage, busy, noAnswer };

// The pedal answers within 100 ms; 25 times that is a silence, not a slow pedal.
inline constexpr int kStorageQueryTimeoutMs = 2500;

inline StorageQuery toQuery(storage::State state)
{
    switch (state) {
    case storage::State::idle: return StorageQuery::idle;
    case storage::State::inStorage: return StorageQuery::inStorage;
    case storage::State::busy: return StorageQuery::busy;
    }
    return StorageQuery::noAnswer; // an enumerator added without a mapping here
}

// For logs and the probe — the player's words are the window's.
inline const char* describe(StorageQuery query)
{
    switch (query) {
    case StorageQuery::idle: return storage::describe(storage::State::idle);
    case StorageQuery::inStorage: return storage::describe(storage::State::inStorage);
    case StorageQuery::busy: return storage::describe(storage::State::busy);
    case StorageQuery::noAnswer: return "no answer";
    }
    return "?";
}

// Send the read request to ONE pedal, with its model id, and wait for its
// answer — judged against that model, so with two pedals on the bus the
// other one's frames are not it — at most timeoutMs. Short-lived: the inputs
// of the pedal's family are opened for the query and closed after it — no
// standing listener. BLOCKS for up to timeoutMs, so it belongs on a worker
// thread, never the message thread. Throws loopercat::Error when the
// endpoint cannot be opened, when no input of the pedal's family is on the
// bus, or when the register reads a value this app does not know
// (StorageRegister.hpp names the byte). Implemented once per platform.
StorageQuery readStorageState(const Pedal& pedal, int timeoutMs = kStorageQueryTimeoutMs);

// The same, for an endpoint of any profiled model.
inline StorageQuery readStorageState(const juce::MidiDeviceInfo& endpoint, int timeoutMs = kStorageQueryTimeoutMs)
{
    return readStorageState(pedalOf(endpoint), timeoutMs);
}

} // namespace loopercat::pedallink
