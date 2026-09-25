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
#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace loopercat::pedallink {

namespace {

    // "hw:<card>,<device>,0" for the first rawmidi OUTPUT whose port name
    // names the pedal — the same portname::isRc5 rule findPedals() uses, so
    // both halves agree on what the pedal is. The walk mirrors what
    // `amidi -l` does: every card, every rawmidi device on it — or, with a
    // card given, that card alone.
    std::optional<std::string> findPedalRawMidi(std::optional<int> onCard = std::nullopt)
    {
        int card = -1;
        while (snd_card_next(&card) == 0 && card >= 0) {
            if (onCard && *onCard != card)
                continue;
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
                if (portname::isRc5(haystack))
                    found = cardName + "," + std::to_string(device) + ",0";
            }
            snd_ctl_close(control);
            if (found)
                return found;
        }
        return std::nullopt;
    }

    // JUCE names an ALSA endpoint "<client>-<port>" (sequencer ids). The
    // sequencer knows which sound card a kernel client belongs to — and the
    // card is what rawmidi addresses. That is the bridge from "the pedal the
    // caller chose" to "the port this backend writes": with two RC-5s, both
    // named alike, the card number is what keeps the choice.
    std::optional<int> cardOfSequencerClient(const juce::String& identifier)
    {
        const int dash = identifier.indexOfChar('-');
        if (dash <= 0)
            return std::nullopt;
        const int clientId = identifier.substring(0, dash).getIntValue();
        snd_seq_t* seq = nullptr;
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0)
            return std::nullopt;
        snd_seq_client_info_t* info = nullptr;
        snd_seq_client_info_alloca(&info);
        std::optional<int> card;
        if (snd_seq_get_any_client_info(seq, clientId, info) == 0) {
            const int number = snd_seq_client_info_get_card(info);
            if (number >= 0)
                card = number;
        }
        snd_seq_close(seq);
        return card;
    }

    juce::String sendFrame(const std::string& port, bool enter)
    {
        snd_rawmidi_t* out = nullptr;
        if (const int opened = snd_rawmidi_open(nullptr, &out, port.c_str(), 0); opened < 0)
            return "cannot open " + juce::String(port) + ": " + snd_strerror(opened);

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

} // namespace

juce::String requestStorageMode(bool enter, const juce::MidiDeviceInfo& pedal)
{
    const auto card = cardOfSequencerClient(pedal.identifier);
    if (!card)
        return "cannot tell which sound card MIDI endpoint " + pedal.identifier + " (" + pedal.name
             + ") belongs to";
    const auto port = findPedalRawMidi(card);
    if (!port)
        return "no RC-5 rawmidi port on sound card " + juce::String(*card) + " (" + pedal.name + ")";
    return sendFrame(*port, enter);
}

juce::String requestStorageMode(bool enter)
{
    const auto port = findPedalRawMidi();
    if (!port)
        return "no RC-5 MIDI device on the bus";
    return sendFrame(*port, enter);
}

StorageQuery readStorageState(const juce::MidiDeviceInfo& pedal, int timeoutMs)
{
    if (timeoutMs <= 0)
        throw Error("storage query: the timeout must be positive");
    const auto card = cardOfSequencerClient(pedal.identifier);
    if (!card)
        throw Error("cannot tell which sound card MIDI endpoint " + pedal.identifier.toStdString() + " ("
                    + pedal.name.toStdString() + ") belongs to");
    const auto port = findPedalRawMidi(card);
    if (!port)
        throw Error("no RC-5 rawmidi port on sound card " + std::to_string(*card) + " ("
                    + pedal.name.toStdString() + ")");

    // The rawmidi port is bidirectional: the same hw:card,device carries the
    // pedal's answer back, so there is nothing to pair. Non-blocking, so the
    // wait below is ours to time.
    snd_rawmidi_t* in = nullptr;
    snd_rawmidi_t* out = nullptr;
    if (const int opened = snd_rawmidi_open(&in, &out, port->c_str(), SND_RAWMIDI_NONBLOCK); opened < 0)
        throw Error("cannot open " + *port + ": " + snd_strerror(opened));
    struct Closer {
        snd_rawmidi_t* input;
        snd_rawmidi_t* output;
        ~Closer()
        {
            snd_rawmidi_close(input);
            snd_rawmidi_close(output);
        }
    } const closer { in, out };

    const auto request = storage::readRequest();
    const ssize_t written = snd_rawmidi_write(out, request.data(), request.size());
    if (written < 0)
        throw Error(std::string("MIDI write failed: ") + snd_strerror(static_cast<int>(written)));
    if (static_cast<size_t>(written) != request.size())
        throw Error("MIDI write was cut short (" + std::to_string(written) + " of "
                    + std::to_string(request.size()) + " bytes)");
    if (const int drained = snd_rawmidi_drain(out); drained < 0)
        throw Error(std::string("MIDI flush failed: ") + snd_strerror(drained));

    const int count = snd_rawmidi_poll_descriptors_count(in);
    if (count <= 0)
        throw Error("cannot poll " + *port);
    std::vector<pollfd> fds(static_cast<size_t>(count));
    snd_rawmidi_poll_descriptors(in, fds.data(), static_cast<unsigned int>(count));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::vector<std::uint8_t> frame;
    bool inFrame = false;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0)
            return StorageQuery::noAnswer;
        const int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), static_cast<int>(left));
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            throw Error(std::string("poll failed: ") + std::strerror(errno));
        }
        if (ready == 0)
            return StorageQuery::noAnswer;
        unsigned short revents = 0;
        snd_rawmidi_poll_descriptors_revents(in, fds.data(), static_cast<unsigned int>(count), &revents);
        if ((revents & POLLIN) == 0)
            continue;
        std::uint8_t buffer[256];
        const ssize_t n = snd_rawmidi_read(in, buffer, sizeof buffer);
        if (n < 0) {
            if (n == -EAGAIN)
                continue;
            throw Error(std::string("MIDI read failed: ") + snd_strerror(static_cast<int>(n)));
        }
        // A byte stream: frames are cut at F0..F7 and judged one by one; a
        // frame that is not the answer is dropped and the wait goes on.
        for (ssize_t i = 0; i < n; ++i) {
            const std::uint8_t byte = buffer[i];
            if (byte == sysex::kSysexStart) {
                frame.clear();
                inFrame = true;
            }
            if (!inFrame)
                continue;
            frame.push_back(byte);
            if (byte == sysex::kSysexEnd) {
                inFrame = false;
                if (const auto state = storage::parseReply(frame))
                    return toQuery(*state);
            }
        }
    }
}

} // namespace loopercat::pedallink
