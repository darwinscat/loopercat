// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "NormalizePlan.h"
#include "history/HistoryRecorder.h"
#include "history/HistoryStore.h"
#include "history/SlotLoudness.h"

#include <loopercat/Commands.hpp>
#include <loopercat/Error.hpp>
#include <loopercat/Volume.hpp>

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

//==============================================================================
// loopercat::normalizestep — the single-slot Normalize in two steps (#142):
// a read-only first step that knows what the command would do before anyone
// is asked, and a write that touches only the bytes that step measured.
//
//   run             the first step, on the worker: the take's reading — the
//                   history's for exactly these bytes, or a fresh one — and
//                   the plan made of it. No operation, no archive, no journal.
//   guardedBefore   the write's first move, ahead of the history opening its
//                   operation: a take that changed since the step measured
//                   it is refused with nothing written and no row opened.
//   StepsInFlight   one first step per slot at a time, on the message thread:
//                   a second request while the first still reads would open
//                   a second window over the same take.
//==============================================================================
namespace loopercat::normalizestep
{

struct Step {
    history::SlotLoudness read; // the reading, and the hash of the bytes it is of
    normalizeplan::Plan plan;
};

// Throws for whatever no plan can be made of: a target that is not one
// (before a byte is read), no take, bytes that are not the pedal's stereo
// float, a card that cannot be read. Worker thread.
inline Step run(const volume::fs::path& volume, int slot, history::HistoryRecorder& recorder,
                double targetLufs)
{
    commands::requireNormalizeTarget(targetLufs);
    // A stored reading serves only if a plan can be made of it; one that
    // cannot is measured over once, and the plan is made of the fresh one.
    history::SlotLoudness read = history::recallOrReadSlotLoudness(
        volume, slot, recorder, [slot, targetLufs](const wav::LoudnessReading& known) {
            try {
                (void) normalizeplan::decide(slot, known, targetLufs);
                return true;
            } catch (const Error&) {
                return false;
            }
        });
    normalizeplan::Plan plan = normalizeplan::decide(slot, read.reading, targetLufs);
    return { std::move(read), std::move(plan) };
}

inline std::string changedSinceMeasured(int slot)
{
    return "slot " + std::to_string(slot) + " changed since it was measured — Normalize again";
}

// The slot still holds the very bytes the first step measured — the hash is
// the step's, of the whole file. A slot emptied since is a change too.
inline void requireMeasuredTake(const volume::fs::path& volume, int slot, const std::string& hash)
{
    const std::vector<std::string> files = volume::listSlotWavs(volume, slot);
    if (files.empty()
        || history::HistoryStore::contentHash(
               commands::readFileBytes(volume::wavDir(volume, slot) / files.front()))
               != hash)
        throw Error(changedSinceMeasured(slot));
}

// The window was answered for the reading the step took; the take may have
// been replaced, pushed over or cleared since. The check runs ahead of
// `before` — the history opening the operation — so a changed take leaves
// no row, no archive copy and no write; the job fails with the sentence, and
// the operation that never began has nothing to close. Worker thread.
inline std::function<void(const volume::fs::path&)> guardedBefore(
    int slot, std::string hash, std::function<void(const volume::fs::path&)> before)
{
    if (!before)
        throw Error("internal: the Normalize guard goes ahead of a job's history step, and this "
                    "job has none");
    return [slot, measured = std::move(hash), opens = std::move(before)](const volume::fs::path& volume) {
        requireMeasuredTake(volume, slot, measured);
        opens(volume);
    };
}

class StepsInFlight
{
public:
    // False when a step for this slot is already in flight: the request is dropped.
    bool start(int slot) { return slots_.insert(slot).second; }

    // Every step ends exactly once — offered, refused, or stopped at the
    // worker's gate — so ending one that never started is a bug.
    void end(int slot)
    {
        if (slots_.erase(slot) == 0)
            throw Error("internal: no Normalize step for slot " + std::to_string(slot)
                        + " is in flight");
    }

    bool contains(int slot) const { return slots_.contains(slot); }

private:
    std::set<int> slots_;
};

} // namespace loopercat::normalizestep
