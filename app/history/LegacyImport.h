// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//==============================================================================
// loopercat::history::legacy — the history before there was a history.
//
// Before the store (#72) every operation left two folders under the data home:
//
//   backups/<stamp>/MEMORY1.RC0 …   the card's documents before a write
//   trash/<stamp>/<NNN_1>/<file>    a take the operation replaced or removed
//
// The stamp is the wall clock as the app wrote it, YYYY-MM-DDTHH-MM-SS, and
// since #78 the operation's tag and counter follow it (2026-09-23T21-58-23-
// 233b-9): such a folder was named by opid::make, and its op is in the store.
//
// The import turns those folders into rows without inventing what they do
// not say. What a folder does say: the time, the slot a take came from (its
// folder is volume::slotDirName), the bytes. What it does not: which command
// ran, what the slot held afterwards, which card it was. So every stamp
// becomes one op — kind 'legacy', actor 'legacy', done, its note naming the
// folders — with a slot_audio 'before' row per take (a trashed take is what
// the slot held before, whatever the command was) and a legacy_files row per
// file; the bytes go through the store's content-addressed rule, so a take the
// store already holds costs nothing. The documents keep their whole bytes: a
// slot's body at that time is rc0::slotBody of them, when someone asks.
//
// Each file is one transaction and the ledger (legacy_files.path) says which
// are done, so the import can stop anywhere and be run again: a second run
// records nothing twice, and a run cut off resumes where it stopped. A folder
// the app itself recorded (its id names an op with another actor) is left
// alone; a folder that is not the app's shape is skipped by name, with its
// reason, and the others go on. Nothing is deleted or moved: the folders'
// fate is a separate decision (#74).
//
// Timezone: a stamp is local wall-clock time and is read back in this
// machine's zone. Folders written elsewhere land off by the zones' difference,
// which the folder cannot say either.
//==============================================================================
namespace loopercat::history::legacy
{

inline constexpr const char* kBackupsFolder = "backups";
inline constexpr const char* kTrashFolder = "trash";

// The card the legacy ops are booked on: the folders name no card, so the
// import says so rather than guessing one.
inline constexpr const char* kCardModel = "unknown";
inline constexpr const char* kCardLabel = "legacy folders";

// A folder name as the app wrote it.
struct Stamp {
    std::int64_t atMs; // the wall clock in the name, read as local time
    bool minted;       // carries opid::make's tag and counter after the clock
};
std::optional<Stamp> parseStamp(std::string_view name);

struct Skipped {
    std::string path; // under the data home, '/'-separated
    std::string reason;
};

struct Report {
    int operations = 0;      // legacy ops this run created
    int takes = 0;           // files out of trash/ recorded by this run
    int documents = 0;       // files out of backups/ recorded by this run
    int deduplicated = 0;    // of those, whose bytes the store already held
    int alreadyImported = 0; // files an earlier run recorded, left alone
    std::vector<Skipped> skipped;
};

// Records every file under <dataHome>/backups and <dataHome>/trash the store
// does not know yet. `nowMs` stamps the session, the ledger and the bytes.
Report importFolders(HistoryStore& store, const std::filesystem::path& dataHome,
                     std::int64_t nowMs);

} // namespace loopercat::history::legacy
