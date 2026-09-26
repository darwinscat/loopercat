// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//==============================================================================
// loopercat::history::HistoryRecorder — between the pedal worker and the store.
// It owns the store, the open session and the operations in flight, and turns
// what the core reports (commands::Archive, commands::Journal) into rows.
//
// Worker thread only, every method: the worker opens an operation before its
// job touches the card (PedalWorker::Job::before), the core's hooks call in
// while it runs, and the worker closes it with the job's outcome (::after).
//
// The store opens on first use. If it cannot open — a newer LooperCat's store,
// a damaged file, a full disk — every operation is refused with that reason,
// and nothing is written to the card: the history is the undo, and a write
// without its undo is the thing this whole feature exists to prevent. Browsing
// the card does not go through here and keeps working.
//
// A card is, for now, its model and volume label; a session runs from the
// first operation on a volume until the next operation on another one, or the
// app's exit. The connect stage of #72 replaces both with real signals.
//==============================================================================
namespace loopercat::history
{

class HistoryRecorder
{
public:
    using Clock = std::function<std::int64_t()>; // milliseconds since the epoch

    HistoryRecorder(std::filesystem::path dir, std::string model, Clock clock);
    ~HistoryRecorder();
    HistoryRecorder(const HistoryRecorder&) = delete;
    HistoryRecorder& operator=(const HistoryRecorder&) = delete;

    void begin(const std::string& opId, const std::string& kind,
               const std::filesystem::path& volume);
    void keepAudio(const std::string& opId, int slot, const std::string& fileName,
                   std::string_view bytes);
    void bodies(const std::string& opId, const std::vector<commands::SlotChange>& changes);
    void landed(const std::string& opId, int slot, const std::string& fileName,
                std::string_view bytes);
    // `error` empty = the job succeeded, and `note` is the line the job wrote
    // about itself ("normalized -3.2 dB", "already at -18.0 LUFS") — the only
    // record of an operation that decided to change nothing, and empty for the
    // ones whose rows already say everything. A failed job keeps its error
    // there instead: the reason outranks the story. An operation that never
    // began (the worker's gate refused the job first) has nothing to close.
    void finish(const std::string& opId, const std::string& error, const std::string& note = {});
    // For an 'undo' or 'redo' operation that has begun: the operation it
    // reverts (#73). Called before the job touches the card, so the row
    // names its target even if the write is then cut off.
    void reverts(const std::string& opId, std::int64_t target);
    // What an operation is about to do to the pedal's own settings, per
    // section, reported before the settings pair is written — the core's
    // journal hook for SYSTEM*.RC0 lands here.
    void systemChanges(const std::string& opId, const std::vector<HistoryStore::SystemChange>& changes);

    HistoryStore& store();

private:
    // An operation in flight: its row, what kind it is (a swap moves audio
    // between two slots without writing a byte) and the volume it runs on.
    struct Operation {
        std::int64_t row = 0;
        std::string kind;
        std::filesystem::path volume;
    };

    std::int64_t opRow(const std::string& opId) const;
    // After a successful operation, write down what each slot it touched now
    // holds, read from the card. Without it a slot's timeline cannot be read
    // on its own: a swap would send the reader into the other slot's rows,
    // and a rename would leave no sign that the slot held a take at all.
    // Hashes are carried only where they are certain — from the slot's own
    // last state, or, for a swap, from the slot it exchanged with. A file the
    // store has never seen is recorded by name and size, with no hash.
    void recordWhatSlotsHold(const Operation& op);
    std::int64_t sessionFor(const std::filesystem::path& volume);

    // The RC-5 has one track per memory; slot_audio.track is there for the
    // pedals that have more.
    static constexpr int kTrack = 1;

    std::filesystem::path dir_;
    std::string model_;
    Clock clock_;
    std::optional<HistoryStore> store_;
    std::string openError_;
    std::optional<std::int64_t> session_;
    std::filesystem::path sessionVolume_;
    std::map<std::string, Operation> ops_;
};

// The one wiring from an operation's WriteOptions into the history, shared by
// the app and its tests so the tests exercise what ships: each replaced take
// is kept by the history FIRST and then handed to `alsoKeep` (the transitional
// trash folder; null for none), and the journal's bodies, landed takes and
// settings sections are recorded under the operation's id.
commands::WriteOptions withHistory(const std::shared_ptr<HistoryRecorder>& recorder,
                                   commands::WriteOptions options, commands::Archive alsoKeep);

} // namespace loopercat::history
