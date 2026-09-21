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
    // `error` empty = the job succeeded. An operation that never began (the
    // worker's gate refused the job first) has nothing to close.
    void finish(const std::string& opId, const std::string& error);

    HistoryStore& store();

private:
    std::int64_t opRow(const std::string& opId) const;
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
    std::map<std::string, std::int64_t> ops_;
};

// The one wiring from an operation's WriteOptions into the history, shared by
// the app and its tests so the tests exercise what ships: each replaced take
// is kept by the history FIRST and then handed to `alsoKeep` (the transitional
// trash folder; null for none), and the journal's bodies and landed takes are
// recorded under the operation's id.
commands::WriteOptions withHistory(const std::shared_ptr<HistoryRecorder>& recorder,
                                   commands::WriteOptions options, commands::Archive alsoKeep);

} // namespace loopercat::history
