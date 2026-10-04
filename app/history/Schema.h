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
//   cards        a card by its marker id, with its name and model
//   sessions     one per connection
//   ops          one per operation, in timeline order (seq); `id` is the
//                operation id the app mints (app/OperationId.h)
//   slot_changes what an op did to a slot's body — before AND after in one
//                row, so every recorded op can be undone on its own
//   slot_audio   which takes a slot held on either side of an op: name, size,
//                and the content hash when the store knows those bytes.
//                For an operation the app finished, the `after` rows are the
//                whole truth about that slot: none means the slot holds no
//                audio, never "we did not look". A hash is absent only when
//                the bytes are strange to the store — a take the pedal
//                recorded while the app was away — and is never guessed.
//   blobs_meta   what the store keeps, and what is pinned or released
//   blobs        the bytes, by content hash — never without their blobs_meta
//                row, which the foreign key enforces
//   ops.pinned   a pinned operation holds every take its rows
//                name against release (#74)
//   system_changes what an operation did to the pedal's own
//                settings, per section — before and after, so it can be
//                undone like a slot change
//   op_subjects  the slots an operation was ABOUT, apart from what it
//                changed (#144): a normalize that found its slot at target
//                writes no slot_changes row, and still was about that slot.
//                A subject names no state, so it never makes an operation
//                restorable, nor an Undo target.
//   loudness_readings
//                what a take's bytes measure (#140), by their content hash
//                and nothing else: the raw BS.1770 numbers, never the
//                verdict, since the target they are judged against moves
//                in Settings. No foreign key on purpose — a reading holds
//                for bytes slot_audio only names, kept or released, and for
//                bytes read off the card that no operation ever archived.
//   slot_audio.modified
//                the take's modification time as the card's directory entry
//                carries it, on every `after` row from version 9 on: with
//                the name and the size it tells a later connect whether the
//                file in the slot is still the one that was measured,
//                without reading it (#141). NULL on `after` rows older than
//                the column, and on `before` rows: an archived take is
//                leaving its slot, and no connect will meet it there.
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

inline constexpr std::int64_t kVersion = 9;

// Version 5 is the first supported store. Future steps append to this array;
// kSteps[N] creates version kBaseVersion + N in the same transaction.
inline constexpr std::int64_t kBaseVersion = 5;
inline constexpr const char* kSteps[] = {
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
    actor   TEXT    NOT NULL CHECK (actor IN ('app', 'pedal')),
    status  TEXT    NOT NULL CHECK (status IN ('pending', 'done', 'failed', 'interrupted')),
    at      INTEGER NOT NULL,
    reverts INTEGER REFERENCES ops(seq),
    note    TEXT,
    pinned  INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0, 1))
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
CREATE TABLE system_changes(
    op      INTEGER NOT NULL REFERENCES ops(seq),
    section TEXT    NOT NULL CHECK (section IN ('SETUP', 'MIDI', 'CTL')),
    before  TEXT    NOT NULL,
    after   TEXT    NOT NULL,
    PRIMARY KEY (op, section)
) STRICT;
)sql",
R"sql(
-- Unidentified version 5 cards stay separate: a label cannot prove identity.
ALTER TABLE cards ADD COLUMN marker_id TEXT NOT NULL DEFAULT '';
UPDATE cards SET marker_id = 'unidentified-v5:' || id;
ALTER TABLE cards ADD COLUMN name TEXT;
ALTER TABLE cards ADD COLUMN snapshot_op INTEGER REFERENCES ops(seq);
CREATE UNIQUE INDEX cards_by_marker ON cards(marker_id);
CREATE TABLE slot_changes_v6(
    op INTEGER NOT NULL REFERENCES ops(seq),
    slot INTEGER NOT NULL CHECK (slot BETWEEN 1 AND 99),
    before_body BLOB,
    after_body BLOB,
    snapshot_error TEXT,
    CHECK (after_body IS NOT NULL OR snapshot_error IS NOT NULL),
    PRIMARY KEY (op, slot)
) STRICT;
INSERT INTO slot_changes_v6(op, slot, before_body, after_body) SELECT * FROM slot_changes;
DROP TABLE slot_changes;
ALTER TABLE slot_changes_v6 RENAME TO slot_changes;
CREATE INDEX slot_changes_by_slot ON slot_changes(slot, op);
)sql",
R"sql(
-- A forgotten part of an operation is an Undo boundary, even if its other
-- slots still have rows. Baseline omissions survive reconnects and resumption.
ALTER TABLE cards ADD COLUMN undo_floor INTEGER NOT NULL DEFAULT 0;
CREATE TABLE forgotten_slots(
    card INTEGER NOT NULL REFERENCES cards(id),
    slot INTEGER NOT NULL CHECK (slot BETWEEN 1 AND 99),
    PRIMARY KEY (card, slot)
) STRICT;
)sql",
R"sql(
-- The slots an operation was about, beside what it changed (#144). Apart
-- from slot_changes on purpose: a row there without bodies would look like
-- a state to restore, and an operation that changed nothing has none.
CREATE TABLE op_subjects(
    op   INTEGER NOT NULL REFERENCES ops(seq),
    slot INTEGER NOT NULL CHECK (slot BETWEEN 1 AND 99),
    PRIMARY KEY (op, slot)
) STRICT;
CREATE INDEX op_subjects_by_slot ON op_subjects(slot, op);
)sql",
R"sql(
-- A loudness reading is a fact about bytes (#140): keyed by their hash and by
-- nothing else, so it outlives the slot, the operation and the kept copy.
-- The raw reading, not the verdict. integrated_lufs is NULL for silence or
-- under one 400 ms gating block; true_peak_dbtp is -Inf for digital silence.
CREATE TABLE loudness_readings(
    hash            BLOB    PRIMARY KEY CHECK (length(hash) = 32),
    integrated_lufs REAL,
    sample_peak     REAL    NOT NULL CHECK (sample_peak >= 0),
    true_peak_dbtp  REAL    NOT NULL,
    wild_samples    INTEGER NOT NULL CHECK (wild_samples >= 0),
    measured        INTEGER NOT NULL
) STRICT;
-- The take's modification time off the card's directory entry (#141), on
-- the rows that say what a slot holds. NULL on those written before this
-- version, and on the rows of archived takes.
ALTER TABLE slot_audio ADD COLUMN modified INTEGER;
)sql",
};

static_assert(sizeof(kSteps) / sizeof(kSteps[0]) == kVersion - kBaseVersion + 1,
              "one step per supported version");

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

inline void requireSupportedVersion(sqlite::Db& db)
{
    const std::int64_t found = pragmaInteger(db, "main.user_version");
    if (found < 0)
        throw Error("the history has an invalid negative store version " + std::to_string(found));
    if (found > kVersion)
        throw Error("the history was written by a newer LooperCat (store version "
                    + std::to_string(found) + ", this one reads up to "
                    + std::to_string(kVersion) + ")");
    if (found > 0 && found < kBaseVersion)
        throw Error("This is a preview store. Delete "
                    + std::string(sqlite3_db_filename(db.raw(), "main"))
                    + " to start a new history.");
}

// Brings a freshly opened store to version kVersion, or refuses. Idempotent:
// an up-to-date store is left exactly as it is. The steps from the version
// found to kVersion run in one transaction with the version stamp, so a store
// is at a version it fully has, or untouched — never between two.
inline void migrate(sqlite::Db& db)
{
    requireSupportedVersion(db);
    const std::int64_t found = pragmaInteger(db, "main.user_version");
    if (found == kVersion)
        return;

    sqlite::Transaction tx(db);
    for (std::int64_t version = found == 0 ? kBaseVersion : found + 1;
         version <= kVersion; ++version)
        db.exec(kSteps[version - kBaseVersion]);
    db.exec("PRAGMA main.user_version = " + std::to_string(kVersion));
    tx.commit();
}

} // namespace loopercat::history::schema
