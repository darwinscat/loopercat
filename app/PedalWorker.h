// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Catalog.hpp>
#include <loopercat/Commands.hpp>
#include <loopercat/Lifecycle.hpp>
#include <loopercat/Volume.hpp>

#include <juce_events/juce_events.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

//==============================================================================
// loopercat::PedalWorker — ALL pedal I/O on one owned thread: the polling
// scans (volume detection, MEMORY1 + wav listings + doctor findings) and the
// mutation jobs the UI enqueues. One thread means a scan can never observe a
// half-written state from our own mutation, and mutations are serialized by
// construction.
//
// Delivery to the message thread goes through callAsync guarded by an alive
// token (the destructor flips it on the message thread after joining the
// worker, so a queued delivery can never touch a dead owner). Snapshots are
// change-gated; job results and busy transitions always arrive, in order:
// busy(true) -> snapshot -> result(description, error) -> busy(false).
//
// scanOnce() is public for the one deliberate exception: the headless
// --snapshot path, which scans synchronously before rendering.
//==============================================================================
namespace loopercat
{

struct SlotRow
{
    catalog::SlotInfo info;
    std::string wavFile; // on-pedal filename(s), comma-joined; empty when none
    std::string wavPath; // absolute path of the first wav — what playback opens

    bool operator==(const SlotRow&) const = default;
};

struct PedalSnapshot
{
    std::string volume;                      // mount path; empty = no pedal found
    std::string error;                       // non-empty = the volume is there but unreadable
    lifecycle::State state = lifecycle::State::disconnected; // the connection truth (issue #17)
    long long freeBytes = 0;                 // available space on the volume
    std::vector<SlotRow> slots;              // all 99 when readable
    std::vector<commands::Finding> findings; // doctor report (health banners)

    // freeBytes compares at the status line's display granularity (0.1 GB):
    // byte-level drift from unrelated disk activity is not a change, and on a
    // busy machine it turned every poll into a delivery (issue #16).
    bool operator==(const PedalSnapshot& other) const
    {
        constexpr long long kFreeBytesBucket = 100'000'000; // 0.1 GB, the shown precision
        return volume == other.volume && error == other.error && state == other.state
            && freeBytes / kFreeBytesBucket == other.freeBytes / kFreeBytesBucket
            && slots == other.slots && findings == other.findings;
    }
};

class PedalWorker final : private juce::Thread
{
public:
    struct Job
    {
        juce::String description;                          // for the result banner
        int slot = 0;                                      // affected slot (0 = none) — row busy indication
        std::function<void(const volume::fs::path&)> work; // runs on the worker, volume resolved fresh
        // A line the job may add to its own success story ("normalized
        // -3.2 dB…"): `work` writes it on the worker thread before returning,
        // the result path reads it after — never concurrently. Empty or
        // absent = the plain description.
        std::shared_ptr<juce::String> note = nullptr;
        // 0 = a standalone job. Jobs enqueued as one batch share an id, so
        // cancelPending(id) can drop the queued tail and results can be
        // credited to the batch's overlay (issue #61).
        int batch = 0;
        // A background job is read-only work the player did not sit down to
        // wait for (the loudness check, issue #61): any foreground job jumps
        // ahead of it in the queue, and it pulses its row without locking
        // the UI. Still serialized on this thread — it can never read a take
        // mid-rewrite.
        bool background = false;
    };

    // `explicitVolume` pins the volume path (the --volume CLI override;
    // empty = autodetect). All callbacks fire on the message thread.
    PedalWorker(std::string explicitVolume, std::function<void(const PedalSnapshot&)> onSnapshot)
        : juce::Thread("LooperCat PedalWorker"),
          explicitVolume_(std::move(explicitVolume)),
          onSnapshot_(std::move(onSnapshot))
    {
    }

    ~PedalWorker() override
    {
        // Generous join: a mutation mid-write must finish — killing it could
        // leave the pedal with one memory file written and one stale.
        stopThread(30000);
        *alive_ = false; // message thread; queued deliveries become no-ops
    }

    // (started/finished, affected slot or 0, background job)
    std::function<void(bool, int, bool)> onBusy;
    // (description, error — empty = ok, batch id — 0 = standalone, affected slot or 0)
    std::function<void(juce::String, juce::String, int, int)> onJobResult;

    // Wire the platform device-truth probe (DeviceWatcher::verdict) and the
    // eject starter (DeviceWatcher::eject) before start(). Unset probe means
    // "no device tracking on this platform" — every volume is trusted, which
    // is the deliberate policy for the headless/test paths, not a fallback.
    void setBackingProbe(std::function<lifecycle::Backing(const volume::fs::path&)> probe)
    {
        backingProbe_ = std::move(probe);
    }
    void setEjectStarter(std::function<void(const std::string& volumePath)> starter)
    {
        ejectStarter_ = std::move(starter);
    }

    // Lifecycle events from outside the worker. ANY THREAD. The machine
    // itself is touched only on the worker thread; these queue and wake it.
    void postDeviceLost() { postEvent(Event::deviceLost); }
    void requestEject() { postEvent(Event::ejectRequested); }
    void postEjectFinished(bool unmounted)
    {
        postEvent(unmounted ? Event::ejectFinishedOk : Event::ejectFinishedFail);
    }
    void pokeRescan() { notify(); } // e.g. a disk appeared — cut the poll wait short

    void start() { startThread(); }

    // Queue a mutation; the worker resolves the volume when the job runs and
    // rescans right after it. MESSAGE THREAD.
    void enqueue(Job job)
    {
        {
            const juce::ScopedLock sl(queueLock_);
            queue_.push_back(std::move(job));
        }
        notify(); // cut the poll wait short
    }

    // Drop this batch's not-yet-started jobs; the one in flight always
    // finishes — a per-slot write is atomic and cancel must not change that
    // (issue #61). Returns how many were dropped. MESSAGE THREAD.
    int cancelPending(int batch)
    {
        const juce::ScopedLock sl(queueLock_);
        const auto before = queue_.size();
        std::erase_if(queue_, [batch](const Job& job) { return job.batch == batch; });
        return static_cast<int>(before - queue_.size());
    }

    // One full scan, on the calling thread. Used by the worker loop and by
    // the headless snapshot path.
    PedalSnapshot scanOnce() const
    {
        PedalSnapshot snapshot;

        const auto found = resolveVolume();
        if (!found)
            return snapshot;
        if (!volume::looksLikePedal(*found)) {
            if (!explicitVolume_.empty()) {
                snapshot.volume = found->string();
                snapshot.error = "no pedal content at this path (expected ROLAND/DATA and ROLAND/WAVE)";
            }
            return snapshot;
        }

        snapshot.volume = found->string();
        // The single-observation truth: what backs this path right now. The
        // worker loop refines it with lifecycle history (eject in flight,
        // async device loss); the headless path renders it as-is.
        const lifecycle::Backing backing =
            backingProbe_ ? backingProbe_(*found) : lifecycle::Backing::untracked;
        snapshot.state = backing == lifecycle::Backing::gone ? lifecycle::State::ghost
                                                             : lifecycle::State::connected;
        {
            std::error_code ec;
            const auto space = volume::fs::space(*found, ec);
            if (!ec)
                snapshot.freeBytes = static_cast<long long>(space.available);
        }
        try {
            const std::string text = commands::readMemory(*found);
            for (auto& info : catalog::listSlots(text)) {
                std::string files, firstPath;
                for (const auto& name : volume::listSlotWavs(*found, info.slot)) {
                    if (files.empty())
                        firstPath = (volume::wavDir(*found, info.slot) / name).string();
                    else
                        files += ", ";
                    files += name;
                }
                snapshot.slots.push_back({ std::move(info), std::move(files), std::move(firstPath) });
            }
        } catch (const Error& e) {
            snapshot.error = e.what();
            snapshot.slots.clear();
        }
        snapshot.findings = commands::doctor(*found);
        return snapshot;
    }

private:
    std::optional<volume::fs::path> resolveVolume() const
    {
        if (!explicitVolume_.empty())
            return volume::fs::path(explicitVolume_);
        return volume::detectVolume(volume::candidateVolumes());
    }

    // Foreground first: a mutation the player just asked for never waits
    // behind a queue of background reads; background jobs run in order once
    // nothing else is pending.
    std::optional<Job> popJob()
    {
        const juce::ScopedLock sl(queueLock_);
        if (queue_.empty())
            return std::nullopt;
        auto pick = std::find_if(queue_.begin(), queue_.end(),
                                 [](const Job& job) { return !job.background; });
        if (pick == queue_.end())
            pick = queue_.begin();
        Job job = std::move(*pick);
        queue_.erase(pick);
        return job;
    }

    // Post to the message thread; dropped silently once the owner is gone.
    void deliver(std::function<void()> fn)
    {
        juce::MessageManager::callAsync([alive = alive_, f = std::move(fn)] {
            if (*alive)
                f();
        });
    }

    void maybeDeliverSnapshot(PedalSnapshot snapshot)
    {
        if (last_ && snapshot == *last_)
            return;
        last_ = snapshot;
        deliver([cb = onSnapshot_, s = std::move(snapshot)] {
            if (cb)
                cb(s);
        });
    }

    void run() override
    {
        while (!threadShouldExit()) {
            applyPendingEvents();
            if (auto job = popJob()) {
                deliver([cb = onBusy, slot = job->slot, bg = job->background] {
                    if (cb)
                        cb(true, slot, bg);
                });
                juce::String error;
                try {
                    // The lifecycle gate: a ghost mount happily accepts writes
                    // into page cache that can never reach the pedal — the
                    // 2026-07-22 phantom "saved" toast. Only connected is honest.
                    if (!machine_.writable())
                        throw Error(std::string("pedal is ") + lifecycle::stateName(machine_.state())
                                    + " — refusing to touch the volume");
                    const auto found = resolveVolume();
                    if (!found || !volume::looksLikePedal(*found))
                        throw Error("no pedal volume mounted");
                    job->work(*found);
                } catch (const std::exception& e) {
                    error = juce::String::fromUTF8(e.what()); // core messages carry typographic dashes
                }
                maybeDeliverSnapshot(scanAdvanced());
                juce::String described = job->description;
                if (error.isEmpty() && job->note != nullptr && job->note->isNotEmpty())
                    described << juce::String::fromUTF8(" \xe2\x80\x94 ") << *job->note;
                deliver([cb = onJobResult, d = described, error, b = job->batch, s = job->slot] {
                    if (cb)
                        cb(d, error, b, s);
                });
                deliver([cb = onBusy, slot = job->slot, bg = job->background] {
                    if (cb)
                        cb(false, slot, bg);
                });
                continue; // more queued work before the next poll
            }
            maybeDeliverSnapshot(scanAdvanced());
            wait(kPollIntervalMs); // enqueue()/postEvent()/pokeRescan() notify
        }
    }

    // One scan folded into the lifecycle machine. WORKER THREAD ONLY.
    PedalSnapshot scanAdvanced()
    {
        PedalSnapshot snapshot = scanOnce();
        if (snapshot.volume.empty())
            machine_.pedalMissing();
        else
            // live-vs-untracked is already collapsed here; the machine treats
            // them identically, so `live` stands in for both.
            machine_.pedalSeen(snapshot.state == lifecycle::State::ghost
                                   ? lifecycle::Backing::gone
                                   : lifecycle::Backing::live);
        snapshot.state = machine_.state();
        lastVolume_ = snapshot.volume;
        return snapshot;
    }

    enum class Event { deviceLost, ejectRequested, ejectFinishedOk, ejectFinishedFail };

    void postEvent(Event event)
    {
        {
            const juce::ScopedLock sl(queueLock_);
            events_.push_back(event);
        }
        notify();
    }

    // Fold queued lifecycle events into the machine, in arrival order.
    // WORKER THREAD ONLY. A protocol violation (e.g. an eject request racing
    // a device loss) is surfaced on the banner line, not swallowed.
    void applyPendingEvents()
    {
        std::deque<Event> events;
        {
            const juce::ScopedLock sl(queueLock_);
            events.swap(events_);
        }
        for (const Event event : events) {
            try {
                switch (event) {
                case Event::deviceLost:
                    machine_.deviceLost();
                    break;
                case Event::ejectRequested:
                    machine_.ejectRequested();
                    if (!ejectStarter_)
                        throw Error("eject requested but no eject starter is wired");
                    ejectStarter_(lastVolume_);
                    break;
                case Event::ejectFinishedOk:
                    machine_.ejectFinished(true);
                    break;
                case Event::ejectFinishedFail:
                    machine_.ejectFinished(false);
                    break;
                }
            } catch (const std::exception& e) {
                deliver([cb = onJobResult, msg = juce::String::fromUTF8(e.what())] {
                    if (cb)
                        cb("Eject", msg, 0, 0);
                });
            }
        }
    }

    // USB storage is mutated only by this computer while the pedal sits in
    // STORAGE mode, so a relaxed cadence loses nothing.
    static constexpr int kPollIntervalMs = 1500;

    const std::string explicitVolume_;
    const std::function<void(const PedalSnapshot&)> onSnapshot_;
    std::function<lifecycle::Backing(const volume::fs::path&)> backingProbe_; // set before start()
    std::function<void(const std::string&)> ejectStarter_;                    // set before start()

    juce::CriticalSection queueLock_;
    std::deque<Job> queue_;             // guarded by queueLock_
    std::deque<Event> events_;          // guarded by queueLock_
    lifecycle::Machine machine_;        // worker thread only
    std::string lastVolume_;            // worker thread only
    std::optional<PedalSnapshot> last_; // worker thread only
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalWorker)
};

} // namespace loopercat
