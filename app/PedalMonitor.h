// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Catalog.hpp>
#include <loopercat/Volume.hpp>

#include <juce_events/juce_events.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

//==============================================================================
// loopercat::PedalMonitor — the background eye on the mounted pedal. A polling
// worker (owned juce::Thread, joined on destruction) detects the volume by
// content, reads MEMORY1.RC0 and the per-slot wav listings, and hands the
// message thread an immutable snapshot via AsyncUpdater — the same delivery
// discipline as the family's UpdateChecker. The listener fires only when the
// snapshot actually changed, so mount/unmount and edits arrive as discrete
// events and a quiet pedal stays quiet.
//
// All file I/O happens on the worker; the message thread only ever sees
// value copies. scanOnce() is public for the one deliberate exception: the
// headless --snapshot path, which scans synchronously before rendering.
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
    std::string volume;         // mount path; empty = no pedal found
    std::string error;          // non-empty = the volume is there but unreadable
    std::vector<SlotRow> slots; // all 99 when readable

    bool operator==(const PedalSnapshot&) const = default;
};

class PedalMonitor final : private juce::Thread,
                           private juce::AsyncUpdater
{
public:
    // `explicitVolume` pins the volume path (the --volume CLI override;
    // empty = autodetect). The listener is called on the message thread,
    // first delivery included.
    PedalMonitor(std::string explicitVolume, std::function<void(const PedalSnapshot&)> onChange)
        : juce::Thread("LooperCat PedalMonitor"),
          explicitVolume_(std::move(explicitVolume)),
          onChange_(std::move(onChange))
    {
    }

    ~PedalMonitor() override
    {
        stopThread(2000);
        cancelPendingUpdate();
    }

    void start() { startThread(); }

    // One full scan, on the calling thread. Used by the worker loop and by
    // the headless snapshot path.
    PedalSnapshot scanOnce() const
    {
        PedalSnapshot snapshot;

        const auto found = explicitVolume_.empty()
                             ? volume::detectVolume(volume::candidateVolumes())
                             : std::optional<volume::fs::path>(volume::fs::path(explicitVolume_));
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
            const std::string text = readFileBytes(volume::memoryPath(*found, 1));
            rc0::assertMemoryFile(text);
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
        return snapshot;
    }

private:
    static std::string readFileBytes(const volume::fs::path& path)
    {
        const juce::File file(juce::String(path.string()));
        juce::MemoryBlock block;
        if (!file.loadFileAsData(block))
            throw Error("cannot read " + path.string());
        return { static_cast<const char*>(block.getData()), block.getSize() };
    }

    void run() override
    {
        while (!threadShouldExit()) {
            PedalSnapshot snapshot = scanOnce();
            {
                const juce::ScopedLock sl(lock_);
                if (!last_ || snapshot != *last_) {
                    last_ = snapshot;
                    pending_ = std::move(snapshot);
                    triggerAsyncUpdate();
                }
            }
            wait(kPollIntervalMs);
        }
    }

    void handleAsyncUpdate() override
    {
        PedalSnapshot snapshot;
        {
            const juce::ScopedLock sl(lock_);
            snapshot = pending_;
        }
        if (onChange_)
            onChange_(snapshot);
    }

    // USB storage is mutated only by this computer while the pedal sits in
    // STORAGE mode, so a relaxed cadence loses nothing.
    static constexpr int kPollIntervalMs = 1500;

    const std::string explicitVolume_;
    const std::function<void(const PedalSnapshot&)> onChange_;

    juce::CriticalSection lock_;
    std::optional<PedalSnapshot> last_; // no value until the first scan, which always delivers
    PedalSnapshot pending_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalMonitor)
};

} // namespace loopercat
