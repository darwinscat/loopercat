// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Linux sender: ALSA rawmidi, written to directly.
//
// JUCE's MidiOutput is used on macOS and Windows and is not used here,
// because on Linux it does not reliably deliver. What was measured against a
// real RC-5, with the app's own send path:
//
//   - the device enumerates, openDevice succeeds, and the ALSA sequencer
//     shows our port correctly subscribed to the pedal (Connecting To: 20:0);
//   - sendMessageNow reports success and the pedal does not move;
//   - the same binary later delivered five times out of five, then zero out
//     of five again, with no change to the machine in between — neither
//     sysex nor a plain note-on;
//   - both JUCE 8.0.15 and 9.0.1 behave that way, so it is not a version;
//   - throughout all of it, `amidi` (rawmidi) and `aplaymidi` (sequencer)
//     delivered every single time, which is what rules out the pedal, the
//     port, the cable and the ALSA stack.
//
// A silent, intermittent failure to send is the worst possible shape for
// this particular message: Connect, Disconnect and quit-as-disconnect all
// ride on it, and a user sees "the pedal did not hand over its card" with
// nothing wrong anywhere. rawmidi is what `amidi` uses, it is ~40 lines, and
// it never missed once in a whole evening of measurement.
//
// It also reports honestly: a short write or a failed drain comes back as an
// error string, where the JUCE path could only ever say "sent".

#include "PedalLink.h"

#include <alsa/asoundlib.h>

#include <optional>
#include <string>

namespace loopercat::pedallink {

namespace {

    // "hw:<card>,<device>,0" for the first rawmidi OUTPUT whose port name
    // names the pedal — the same "RC-5" match findPedal() uses, so both
    // halves agree on what the pedal is. The walk mirrors what `amidi -l`
    // does: every card, every rawmidi device on it.
    std::optional<std::string> findPedalRawMidi()
    {
        int card = -1;
        while (snd_card_next(&card) == 0 && card >= 0) {
            snd_ctl_t* control = nullptr;
            const std::string cardName = "hw:" + std::to_string(card);
            if (snd_ctl_open(&control, cardName.c_str(), 0) < 0)
                continue;

            int device = -1;
            std::optional<std::string> found;
            while (!found && snd_ctl_rawmidi_next_device(control, &device) == 0 && device >= 0) {
                snd_rawmidi_info_t* info = nullptr;
                snd_rawmidi_info_alloca(&info);
                snd_rawmidi_info_set_device(info, static_cast<unsigned int>(device));
                snd_rawmidi_info_set_subdevice(info, 0);
                snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
                if (snd_ctl_rawmidi_info(control, info) < 0)
                    continue;
                const char* name = snd_rawmidi_info_get_name(info);
                const char* sub = snd_rawmidi_info_get_subdevice_name(info);
                const std::string haystack = std::string(name != nullptr ? name : "") + " "
                                           + std::string(sub != nullptr ? sub : "");
                if (haystack.find("RC-5") != std::string::npos)
                    found = cardName + "," + std::to_string(device) + ",0";
            }
            snd_ctl_close(control);
            if (found)
                return found;
        }
        return std::nullopt;
    }

} // namespace

juce::String requestStorageMode(bool enter)
{
    const auto port = findPedalRawMidi();
    if (!port)
        return "no RC-5 MIDI device on the bus";

    snd_rawmidi_t* out = nullptr;
    if (const int opened = snd_rawmidi_open(nullptr, &out, port->c_str(), 0); opened < 0)
        return "cannot open " + juce::String(*port) + ": " + snd_strerror(opened);

    // The frame from core/Sysex.hpp is complete, F0 through F7: rawmidi is a
    // byte stream, so it goes out exactly as written — no framing done for
    // us, and none to be undone.
    const auto frame = enter ? sysex::enterStorageMode() : sysex::exitStorageMode();
    const ssize_t written = snd_rawmidi_write(out, frame.data(), frame.size());
    const int drained = snd_rawmidi_drain(out);
    snd_rawmidi_close(out);

    if (written < 0)
        return "MIDI write failed: " + juce::String(snd_strerror(static_cast<int>(written)));
    if (static_cast<size_t>(written) != frame.size())
        return "MIDI write was cut short (" + juce::String(static_cast<int>(written)) + " of "
             + juce::String(static_cast<int>(frame.size())) + " bytes)";
    if (drained < 0)
        return "MIDI flush failed: " + juce::String(snd_strerror(drained));
    return {};
}

} // namespace loopercat::pedallink
