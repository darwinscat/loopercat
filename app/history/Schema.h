// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Sqlite.h"

#include <cstdint>
#include <string>

//==============================================================================
// loopercat::history::schema — the store's tables, and the one rule for its
// version: a store written by a newer LooperCat is refused, never read on a
// guess.
//
// One file, history.db, holds the timeline and the bytes:
//   cards        a pedal's card, by model; identity signals arrive in #72's
//                connect stage, until then a card is its volume label
//   sessions     one per connection
//   ops          one per operation, in timeline order (seq); `id` is the
//                operation id the app mints (app/OperationId.h)
//   slot_changes what an op did to a slot's body — before AND after in one
//                row, so every recorded op can be undone on its own
//   slot_audio   which takes a slot held on either side of an op: name, size,
//                and the content hash when the bytes passed through the app
//   blobs_meta   what the store keeps, and what is pinned or released
//   blobs        the bytes, by content hash — never without their blobs_meta
//                row, which the foreign key enforces
//   legacy_files (version 2) the files the legacy import took out of the
//                backups/ and trash/ folders, by path: the import's ledger,
//                so a folder is recorded once however often it is offered
//
// A rollback journal (DELETE), not WAL, and one file rather than two. A row
// and the bytes it names must land together or not at all; inside one file
// every transaction is atomic. Across two files it holds only through a
// super-journal, in which a WAL file takes no part: a crash injected mid-commit
// with a WAL timeline and a DELETE audio file left the row without its take.
// WAL for the bytes was measured out too — a permanent +24% on disk, twice
// the write time, and a VACUUM that does not give space back. What one file
// costs: a second connection cannot read while a take is being written. The
// store has one connection, on the pedal worker, so nothing waits on it.
//
// The file is created with 16 KB pages and auto_vacuum=INCREMENTAL, both set
// before the first table: the page size suits multi-megabyte takes, and
// incremental vacuum cannot be switched on once a table exists — freeing
// space (#74) depends on it.
//
// `track` is reserved for multi-track pedals (RC-500, RC-600: NNN_1..NNN_n);
// the RC-5 writes 1. The body is stored verbatim, so a multi-track <mem>
// block needs nothing new.
//==============================================================================
namespace loopercat::history::schema
{

inline constexpr std::int64_t kVersion = 2;

// The schema as the sequence of its versions: step N takes a store at version
// N to version N+1. A fresh store runs every step in order, so a store created
// today and one migrated from version 1 hold the same tables by construction —
// there is no separate "current schema" for the upgrades to drift from. A
// step, once released, is never edited: stores in the field sit at some
// version, and the only way forward for them is the next step.
inline constexpr const char* kSteps[] = {
// 0 -> 1: the timeline and the bytes.
R"sql(
CREATE TABLE cards(
    id          INTEGER PRIMARY KEY,
    model       TEXT    NOT NULL,
    label       TEXT    NOT NULL,
    volume_uuid TEXT,
    first_seen  INTEGER NOT NULL,
    last_seen   INTEGER NOT NULL
) STRICT;

CREATE TABLE sessions(
    id              INTEGER PRIMARY KEY,
    card            INTEGER NOT NULL REFERENCES cards(id),
    connected_at    INTEGER NOT NULL,
    disconnected_at INTEGER
) STRICT;

CREATE TABLE ops(
    seq     INTEGER PRIMARY KEY,
    id      TEXT    NOT NULL UNIQUE,
    session INTEGER NOT NULL REFERENCES sessions(id),
    kind    TEXT    NOT NULL,
    actor   TEXT    NOT NULL CHECK (actor IN ('app', 'pedal', 'legacy')),
    status  TEXT    NOT NULL CHECK (status IN ('pending', 'done', 'failed', 'interrupted')),
    at      INTEGER NOT NULL,
    reverts INTEGER REFERENCES ops(seq),
    note    TEXT
) STRICT;

CREATE TABLE slot_changes(
    op          INTEGER NOT NULL REFERENCES ops(seq),
    slot        INTEGER NOT NULL CHECK (slot BETWEEN 1 AND 99),
    before_body BLOB    NOT NULL,
    after_body  BLOB    NOT NULL,
    PRIMARY KEY (op, slot)
) STRICT;
CREATE INDEX slot_changes_by_slot ON slot_changes(slot, op);

CREATE TABLE slot_audio(
    op    INTEGER NOT NULL REFERENCES ops(seq),
    slot  INTEGER NOT NULL CHECK (slot BETWEEN 1 AND 99),
    side  TEXT    NOT NULL CHECK (side IN ('before', 'after')),
    track INTEGER NOT NULL CHECK (track >= 1),
    name  TEXT    NOT NULL,
    size  INTEGER NOT NULL CHECK (size >= 0),
    hash  BLOB    CHECK (hash IS NULL OR length(hash) = 32),
    PRIMARY KEY (op, slot, side, track, name)
) STRICT;
CREATE INDEX slot_audio_by_slot ON slot_audio(slot, op);
CREATE INDEX slot_audio_by_hash ON slot_audio(hash);

CREATE TABLE blobs_meta(
    hash     BLOB    PRIMARY KEY CHECK (length(hash) = 32),
    size     INTEGER NOT NULL CHECK (size >= 0),
    created  INTEGER NOT NULL,
    pinned   INTEGER NOT NULL CHECK (pinned IN (0, 1)),
    released INTEGER
) STRICT;

CREATE TABLE blobs(
    hash  BLOB PRIMARY KEY REFERENCES blobs_meta(hash),
    bytes BLOB NOT NULL
) STRICT;
)sql",

// 1 -> 2: the legacy import's ledger (LegacyImport.h). `path` is the file's
// path under the data home, '/'-separated; `op` the legacy op it was recorded
// under; `kind` what the file was — a take out of trash/ or a document out of
// backups/; `hash` the bytes it put through the store, so the folder's fate
// (#74) can be decided by comparing, never by trusting.
R"sql(
CREATE TABLE legacy_files(
    path     TEXT    PRIMARY KEY,
    op       INTEGER NOT NULL REFERENCES ops(seq),
    kind     TEXT    NOT NULL CHECK (kind IN ('take', 'document')),
    hash     BLOB    NOT NULL REFERENCES blobs_meta(hash),
    imported INTEGER NOT NULL
) STRICT;
CREATE INDEX legacy_files_by_op ON legacy_files(op);
)sql",
};

static_assert(sizeof(kSteps) / sizeof(kSteps[0]) == kVersion,
              "one step per version: kSteps[N] takes a store from version N to N + 1");

inline std::int64_t pragmaInteger(sqlite::Db& db, const std::string& pragma)
{
    sqlite::Statement read(db, "PRAGMA " + pragma);
    if (!read.step())
        throw Error("PRAGMA " + pragma + " returned nothing");
    return read.integer(0);
}

inline std::string pragmaText(sqlite::Db& db, const std::string& pragma)
{
    sqlite::Statement read(db, "PRAGMA " + pragma);
    if (!read.step())
        throw Error("PRAGMA " + pragma + " returned nothing");
    return read.text(0);
}

// Brings a freshly opened store to version kVersion, or refuses. Idempotent:
// an up-to-date store is left exactly as it is. The steps from the version
// found to kVersion run in one transaction with the version stamp, so a store
// is at a version it fully has, or untouched — never between two.
inline void migrate(sqlite::Db& db)
{
    const std::int64_t found = pragmaInteger(db, "main.user_version");
    if (found > kVersion)
        throw Error("the history was written by a newer LooperCat (store version "
                    + std::to_string(found) + ", this one reads up to "
                    + std::to_string(kVersion) + ")");
    if (found == kVersion)
        return;

    sqlite::Transaction tx(db);
    for (std::int64_t version = found; version < kVersion; ++version)
        db.exec(kSteps[version]);
    db.exec("PRAGMA main.user_version = " + std::to_string(kVersion));
    tx.commit();
}

} // namespace loopercat::history::schema
