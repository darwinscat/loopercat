// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

//==============================================================================
// loopercat::ejectretry — what Disconnect does when the card is busy, and the
// words operations.log keeps about it (issue #169).
//
// The first Disconnect after a Connect was refused on 2026-10-19 (an RC-500,
// 57 s after Connect) and on 2026-11-02 (an RC-5, 25 s after). The system log
// of the second one says what happened: no DiskArbitration client dissented,
// the kernel refused the unmount with EBUSY — a file on the card was still
// open, and not one of ours: the app lets go of its own first. Spotlight was
// indexing the freshly mounted card (Spotlight.hpp), and a second press a
// moment later went through. A refusal like that is not the player's to
// handle: the eject waits it out, and the banner speaks only when the hold
// outlives the wait.
//
// All three DeviceWatcher backends drive their eject through here. Each one
// knows how to make ONE unmount try and how to tell a busy refusal from any
// other (DiskArbitration's status, udisks2's DeviceBusy, a refused volume
// lock); the policy — how many tries, how far apart, what is reported — is
// this file's, so it is the same on every platform and tested without a
// device in eject_retry_tests.
//
// Pure apart from waitBlocking, which sleeps: no mount, no device, no window.
//==============================================================================
namespace loopercat::ejectretry
{

// How many unmount tries one Disconnect makes while the volume is busy, and
// how long it waits between them. The hold this waits out is a reader
// finishing a file it has open, on a card that reads at about 13 MB/s
// (measured on the RC-5, 2026-10-16). Five tries a second apart wait about
// four seconds — the second press the player made by hand, made for them. A
// hold that outlives that is a program that keeps the card, and the player
// has to hear about it.
inline constexpr int kTries = 5;
inline constexpr std::chrono::milliseconds kPause { 1000 };

// What one try costs on top of the pauses: DiskArbitration answered a held
// file within 0.24-0.50 s (a FAT32 image through the card's FSKit module,
// 2026-11-03). Half a second each is what anything that waits for an eject
// to finish — the quit, in MainComponent.cpp — has to leave room for.
inline constexpr std::chrono::milliseconds kTryAnswersWithin { 500 };
inline constexpr std::chrono::milliseconds kLongestEject = (kTries - 1) * kPause + kTries * kTryAnswersWithin;

// What one unmount try came back with.
enum class Answer {
    unmounted, // the volume is let go
    busy,      // something still holds a file on it: worth waiting out
    refused,   // anything else — it will read the same in a second, so it is told at once
};

struct Try {
    Answer answer;
    // refused: the OS's words for the refusal, which the journal and the
    // banner carry. busy: the OS's words too, carried nowhere — the cause is
    // told in words instead of a status number (kBusyWords). unmounted: a
    // note about how the volume went (a whole-disk eject macOS refused after
    // it, or someone else's unmount between two tries), or empty.
    std::string detail;
};

// The eject as a whole: the last try, and how many tries it took.
struct Outcome {
    Try last;
    int tries = 0;
    bool unmounted() const { return last.answer == Answer::unmounted; }
};

// Make one unmount try; call the Report with its answer exactly once, on any thread.
using Report = std::function<void(Try)>;
using Attempt = std::function<void(Report)>;
// Run the continuation after the pause, on any thread.
using Wait = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

namespace detail {

    // One eject's state, shared by the continuations of its tries: the
    // backends answer from DiskArbitration's queue, a udisksctl thread or a
    // DeviceIoControl thread, so nothing here may live on a caller's stack.
    class Run : public std::enable_shared_from_this<Run> {
    public:
        Run(Attempt attempt, Wait wait, std::function<void(Outcome)> done)
            : attempt_(std::move(attempt)), wait_(std::move(wait)), done_(std::move(done))
        {
        }

        void next()
        {
            ++tries_;
            auto answered = std::make_shared<bool>(false);
            attempt_([self = shared_from_this(), answered](Try result) {
                if (*answered)
                    throw Error("an eject try was reported twice");
                *answered = true;
                self->settle(std::move(result));
            });
        }

    private:
        void settle(Try result)
        {
            if (result.answer == Answer::busy && tries_ < kTries) {
                wait_(kPause, [self = shared_from_this()] { self->next(); });
                return;
            }
            done_(Outcome { std::move(result), tries_ });
        }

        Attempt attempt_;
        Wait wait_;
        std::function<void(Outcome)> done_;
        int tries_ = 0;
    };

} // namespace detail

// Try, and while the answer is busy and tries are left, wait kPause and try
// again. `done` fires exactly once, with the try that settled it.
inline void drive(Attempt attempt, Wait wait, std::function<void(Outcome)> done)
{
    std::make_shared<detail::Run>(std::move(attempt), std::move(wait), std::move(done))->next();
}

// The Wait of a backend that makes its tries on a thread of its own.
inline void waitBlocking(std::chrono::milliseconds pause, const std::function<void()>& then)
{
    std::this_thread::sleep_for(pause);
    then();
}

// The cause of a busy refusal, in words. The status number it replaces
// (DiskArbitration's 49168) told a player nothing.
inline constexpr std::string_view kBusyWords = "busy: another program holds a file on the card";

namespace detail {

    inline std::string afterTries(int tries)
    {
        return tries == 1 ? std::string() : " after " + std::to_string(tries) + " tries";
    }

    inline std::string inParens(const std::string& text)
    {
        return text.empty() ? std::string() : " (" + text + ")";
    }

} // namespace detail

// The operations.log line for an eject.
inline std::string traceLine(const Outcome& outcome)
{
    switch (outcome.last.answer) {
    case Answer::unmounted:
        return "disconnect: ejected" + detail::afterTries(outcome.tries)
             + detail::inParens(outcome.last.detail);
    case Answer::busy:
        return "disconnect: eject refused" + detail::afterTries(outcome.tries) + " ("
             + std::string(kBusyWords) + ")";
    case Answer::refused:
        return "disconnect: eject refused" + detail::afterTries(outcome.tries)
             + detail::inParens(outcome.last.detail);
    }
    throw Error("an eject outcome with no answer");
}

// What the refusal banner adds after its sentence ("something is still using
// the card"), in parentheses: nothing for a busy card — the sentence already
// is the cause — and the OS's words for any other refusal.
inline std::string bannerDetail(const Outcome& outcome)
{
    return outcome.last.answer == Answer::refused ? outcome.last.detail : std::string();
}

} // namespace loopercat::ejectretry
