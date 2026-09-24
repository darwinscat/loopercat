// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>

//==============================================================================
// loopercat::history::TakeAudition — an archived take as a file, so it can be
// listened to.
//
// The archive holds a take as bytes in the store; the audio engine plays
// files. This is the seam between them: materialize() writes the take named
// by a content hash into the audition folder and hands back its path. The
// same hash asks for the same file — a take auditioned twice is written once
// — and a hash the store has no bytes for (released under #74, or never kept)
// is a nullopt, not a file.
//
// The files are the object's: they go when it does, and whatever an earlier
// run left behind — a crash, a kill — is swept when the next one starts.
// Files are named by their hash, so two takes never collide and a file's name
// says what it holds; a take is written under a partial name and renamed
// into place only when whole, so a name that is there is a file that is
// complete. Only names of that shape are ever swept: a folder shared with
// something else loses nothing of its own.
//
// One instance per audition folder; the app is one process.
//==============================================================================
namespace loopercat::history
{

class TakeAudition
{
public:
    // The shape of an audition file: take-<64 hex>.wav, and .part while it is
    // being written.
    static constexpr const char* kPrefix = "take-";
    static constexpr const char* kSuffix = ".wav";
    static constexpr const char* kPartial = ".part";

    // `dir` is created if absent; what an earlier run left in it is removed.
    explicit TakeAudition(std::filesystem::path dir);
    ~TakeAudition();
    TakeAudition(const TakeAudition&) = delete;
    TakeAudition& operator=(const TakeAudition&) = delete;

    // The take's bytes as a file, by the store's content hash (32 raw bytes).
    std::optional<std::filesystem::path> materialize(HistoryStore& store, const std::string& hash);

    const std::filesystem::path& dir() const { return dir_; }

private:
    std::filesystem::path pathFor(const std::string& hash) const;
    void sweepLeftovers() const;

    std::filesystem::path dir_;
    std::set<std::string> made_; // hashes this instance has written
};

} // namespace loopercat::history
