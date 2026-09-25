// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The macOS and Windows sender: JUCE's MidiOutput, which does the job on
// both. (Linux has its own backend — PedalLinkLinux.cpp says why.)

#include "PedalLink.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace loopercat::pedallink {

namespace {

    // Waits for the one frame that answers the storage query. Frames arrive
    // on JUCE's MIDI thread; the query thread sleeps on the condition until
    // an answer lands or its timeout runs out. The first verdict wins; every
    // later frame is ignored.
    class ReplyListener final : public juce::MidiInputCallback {
    public:
        void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override
        {
            if (!message.isSysEx())
                return;
            // JUCE hands over the whole frame, F0 through F7, as one message.
            const std::span<const std::uint8_t> frame(message.getRawData(),
                                                      static_cast<std::size_t>(message.getRawDataSize()));
            std::optional<storage::State> state;
            std::string problem;
            try {
                state = storage::parseReply(frame);
            } catch (const Error& e) {
                problem = e.what();
            }
            if (!state && problem.empty())
                return; // not the answer — keep listening
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (decidedFlag_)
                    return;
                decidedFlag_ = true;
                state_ = state;
                problem_ = problem;
            }
            signal_.notify_all();
        }

        // True when a verdict arrived before the timeout.
        bool waitFor(int timeoutMs)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return signal_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return decidedFlag_; });
        }

        std::optional<storage::State> state() const
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            return state_;
        }

        std::string problem() const
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            return problem_;
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable signal_;
        bool decidedFlag_ = false;
        std::optional<storage::State> state_;
        std::string problem_;
    };

} // namespace

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

StorageQuery readStorageState(const juce::MidiDeviceInfo& pedal, int timeoutMs)
{
    if (timeoutMs <= 0)
        throw Error("storage query: the timeout must be positive");
    const auto out = juce::MidiOutput::openDevice(pedal.identifier);
    if (out == nullptr)
        throw Error("cannot open MIDI device " + pedal.name.toStdString() + " ("
                    + pedal.identifier.toStdString() + ")");
    const auto inputs = findPedalInputs();
    if (inputs.empty())
        throw Error("no RC-5 MIDI input on the bus");

    // Every RC-5 input is opened: with two pedals the inputs are named alike
    // and nothing pairs an input with the output asked — but only the pedal
    // asked answers, so whichever input carries the answer is its.
    ReplyListener listener;
    std::vector<std::unique_ptr<juce::MidiInput>> opened;
    for (const auto& input : inputs)
        if (auto in = juce::MidiInput::openDevice(input.identifier, &listener)) {
            in->start();
            opened.push_back(std::move(in));
        }
    if (opened.empty())
        throw Error("cannot open any RC-5 MIDI input");

    const auto request = storage::readRequest();
    // JUCE wraps the payload in F0/F7 itself.
    out->sendMessageNow(juce::MidiMessage::createSysExMessage(
        request.data() + 1, static_cast<int>(request.size()) - 2));

    const bool decided = listener.waitFor(timeoutMs);
    for (auto& in : opened)
        in->stop();
    opened.clear(); // closed here, before anything is decided about the answer

    if (!decided)
        return StorageQuery::noAnswer;
    if (const std::string problem = listener.problem(); !problem.empty())
        throw Error(problem);
    return toQuery(*listener.state());
}

} // namespace loopercat::pedallink
