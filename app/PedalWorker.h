// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Catalog.hpp>
#include <loopercat/Commands.hpp>
#include <loopercat/Volume.hpp>

#include <juce_events/juce_events.h>

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
    std::vector<SlotRow> slots;              // all 99 when readable
    std::vector<commands::Finding> findings; // doctor report (health banners)

    bool operator==(const PedalSnapshot&) const = default;
};

class PedalWorker final : private juce::Thread
{
public:
    struct Job
    {
        juce::String description;                          // for the result banner
        std::function<void(const volume::fs::path&)> work; // runs on the worker, volume resolved fresh
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

    std::function<void(bool)> onBusy;                            // a job started/finished
    std::function<void(juce::String, juce::String)> onJobResult; // (description, error; empty = ok)

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

    std::optional<Job> popJob()
    {
        const juce::ScopedLock sl(queueLock_);
        if (queue_.empty())
            return std::nullopt;
        Job job = std::move(queue_.front());
        queue_.pop_front();
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
            if (auto job = popJob()) {
                deliver([cb = onBusy] { if (cb) cb(true); });
                juce::String error;
                try {
                    const auto found = resolveVolume();
                    if (!found || !volume::looksLikePedal(*found))
                        throw Error("no pedal volume mounted");
                    job->work(*found);
                } catch (const std::exception& e) {
                    error = juce::String::fromUTF8(e.what()); // core messages carry typographic dashes
                }
                maybeDeliverSnapshot(scanOnce());
                deliver([cb = onJobResult, d = job->description, error] {
                    if (cb)
                        cb(d, error);
                });
                deliver([cb = onBusy] { if (cb) cb(false); });
                continue; // more queued work before the next poll
            }
            maybeDeliverSnapshot(scanOnce());
            wait(kPollIntervalMs); // enqueue() notifies
        }
    }

    // USB storage is mutated only by this computer while the pedal sits in
    // STORAGE mode, so a relaxed cadence loses nothing.
    static constexpr int kPollIntervalMs = 1500;

    const std::string explicitVolume_;
    const std::function<void(const PedalSnapshot&)> onSnapshot_;

    juce::CriticalSection queueLock_;
    std::deque<Job> queue_;             // guarded by queueLock_
    std::optional<PedalSnapshot> last_; // worker thread only
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalWorker)
};

} // namespace loopercat
