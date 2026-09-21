// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Sqlite.h"

#include <loopercat/Commands.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//==============================================================================
// loopercat::history::HistoryStore — the operation journal and the archive of
// takes, in one SQLite file (issue #72; the tables, and why one file with a
// rollback journal: Schema.h).
//
// The store records; it does not decide. The core reports what a command did
// (commands::Archive, commands::Journal) and the recorder hands that here, one
// transaction per fact:
//
//   beginOp        the op exists, pending, before the command touches the card
//   keepAudio      a take about to be replaced: its bytes and the row naming
//                  them commit together, or neither does
//   recordBodies   the slot bodies the write is about to change, before/after
//   recordLanded   a take that landed: name, size and hash — not the bytes,
//                  which are on the card and are archived when replaced
//   finishOp       done or failed
//
// Opening the store turns every op still pending into `interrupted`: the app
// stopped between two of those steps, and the history says so rather than
// pretending the op finished or never began.
//
// One connection, used from one thread at a time (the pedal worker).
//==============================================================================
namespace loopercat::history
{

enum class OpStatus { done, failed };

class HistoryStore
{
public:
    // <dir>/history.db. Creates it when absent, verifies every storage
    // property it relies on, and refuses a store it cannot read correctly
    // rather than reading it on a guess.
    explicit HistoryStore(const std::filesystem::path& dir);

    // SHA-256 of the bytes, raw (32 bytes): the audio store's key.
    static std::string contentHash(std::string_view bytes);

    // --- where and when ---
    std::int64_t card(const std::string& model, const std::string& label, std::int64_t nowMs);
    std::int64_t openSession(std::int64_t card, std::int64_t nowMs);
    void closeSession(std::int64_t session, std::int64_t nowMs);

    // --- one operation ---
    std::int64_t beginOp(std::int64_t session, const std::string& opId, const std::string& kind,
                         std::int64_t atMs);
    void keepAudio(std::int64_t op, int slot, int track, const std::string& name,
                   std::string_view bytes, std::int64_t nowMs);
    void recordBodies(std::int64_t op, const std::vector<commands::SlotChange>& changes);
    void recordLanded(std::int64_t op, int slot, int track, const std::string& name,
                      std::string_view bytes);
    void finishOp(std::int64_t op, OpStatus status, const std::string& note);

    // --- reads: what the tests look at today, and what #50 builds on ---
    std::optional<std::string> takeBytes(const std::string& hash);
    std::string opStatus(std::int64_t op);

    sqlite::Db& db() { return db_; }

private:
    sqlite::Db db_;
};

} // namespace loopercat::history
