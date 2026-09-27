// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Preview stores are refused intact; fresh stores use the supported base.

#include "support.hpp"

#include "../app/history/HistoryStore.h"
#include "../app/history/Schema.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace loopercat;
using history::HistoryStore;
namespace fs = std::filesystem;
namespace schema = history::schema;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-schema-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

std::int64_t count(sqlite::Db& db, const std::string& sql)
{
    sqlite::Statement read(db, sql);
    if (!read.step())
        throw Error("count returned no row: " + sql);
    return read.integer(0);
}

// The preview version 4 definition, frozen independently of Schema.h.
constexpr const char* kPreview4 = R"sql(
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


CREATE TABLE legacy_files(
    path     TEXT    PRIMARY KEY,
    op       INTEGER NOT NULL REFERENCES ops(seq),
    kind     TEXT    NOT NULL CHECK (kind IN ('take', 'document')),
    hash     BLOB    NOT NULL REFERENCES blobs_meta(hash),
    imported INTEGER NOT NULL
) STRICT;
CREATE INDEX legacy_files_by_op ON legacy_files(op);


ALTER TABLE ops ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0, 1));


CREATE TABLE system_changes(
    op      INTEGER NOT NULL REFERENCES ops(seq),
    section TEXT    NOT NULL CHECK (section IN ('SETUP', 'MIDI', 'CTL')),
    before  TEXT    NOT NULL,
    after   TEXT    NOT NULL,
    PRIMARY KEY (op, section)
) STRICT;
)sql";

} // namespace

int main()
{
    for (const int version : { -1, -5, -2147483647 - 1 }) {
        TempDir tmp;
        const auto file = tmp.path / "history.db";
        {
            auto db = sqlite::Db::open(file);
            db.exec("PRAGMA user_version = " + std::to_string(version));
        }
        const auto before = commands::readFileBytes(file);
        CHECK_THROWS(HistoryStore(tmp.path), "invalid negative store version " + std::to_string(version));
        CHECK(commands::readFileBytes(file) == before);
        auto db = sqlite::Db::open(file);
        CHECK_THROWS(schema::migrate(db), "invalid negative store version");
        CHECK_EQ(schema::pragmaInteger(db, "user_version"), version);
    }

    for (const int version : { 1, 2, 3, 4 }) {
        TempDir tmp;
        const fs::path file = tmp.path / "history.db";
        {
            auto db = sqlite::Db::open(file);
            db.exec("PRAGMA page_size = 16384");
            db.exec("PRAGMA auto_vacuum = INCREMENTAL");
            db.exec(kPreview4);
            db.exec("PRAGMA user_version = " + std::to_string(version));
            db.exec("INSERT INTO cards VALUES (1, 'RC-5', 'Card', NULL, 1000, 1000)");
            db.exec("INSERT INTO sessions VALUES (1, 1, 1000, NULL)");
            db.exec("INSERT INTO ops(id, session, kind, actor, status, at) "
                    "VALUES ('pending', 1, 'clear', 'app', 'pending', 1000)");
        }
        const std::string before = commands::readFileBytes(file);
        CHECK_THROWS(HistoryStore(tmp.path),
                     "This is a preview store. Delete " + fs::canonical(file).string()
                         + " to start a new history.");
        CHECK(commands::readFileBytes(file) == before);
        auto db = sqlite::Db::open(file);
        CHECK_EQ(schema::pragmaInteger(db, "user_version"), version);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE status = 'pending'"), 1);
    }
    {
        TempDir tmp;
        {
            HistoryStore fresh(tmp.path);
            auto& db = fresh.db();
            CHECK_EQ(schema::kVersion, 5);
            CHECK_EQ(schema::pragmaInteger(db, "user_version"), 5);
            CHECK_EQ(count(db, "SELECT count(*) FROM sqlite_master WHERE type = 'table'"), 8);
            CHECK_EQ(count(db, "SELECT count(*) FROM sqlite_master WHERE name = 'legacy_files'"), 0);
            const auto session = fresh.openSession(fresh.card("RC-5", "Card", 1000), 1000);
            const auto op = fresh.beginOp(session, "first", "clear", 1000);
            fresh.keepAudio(op, 1, 1, "take.wav", "take bytes", 1000);
            fresh.recordBodies(op, { { 1, "before", "after" } });
            fresh.finishOp(op, history::OpStatus::done, "");
            fresh.pinOp(op, true);
            CHECK_THROWS(db.exec("UPDATE ops SET pinned = 2"), "CHECK");
            CHECK_THROWS(db.exec("UPDATE ops SET actor = 'unknown'"), "CHECK");
            CHECK_THROWS(db.exec("INSERT INTO system_changes VALUES (1, 'MEM', 'a', 'b')"), "CHECK");
            CHECK_THROWS(db.exec("INSERT INTO system_changes VALUES (99, 'CTL', 'a', 'b')"), "FOREIGN KEY");
            db.exec("INSERT INTO system_changes VALUES (1, 'CTL', 'a', 'b')");
            CHECK_THROWS(db.exec("INSERT INTO system_changes VALUES (1, 'CTL', 'c', 'd')"), "UNIQUE");
        }
        HistoryStore reopened(tmp.path);
        CHECK_EQ(schema::pragmaInteger(reopened.db(), "user_version"), 5);
        CHECK_EQ(count(reopened.db(), "SELECT count(*) FROM ops WHERE pinned = 1"), 1);
        CHECK(reopened.takeBytes(HistoryStore::contentHash("take bytes")) == "take bytes");
        CHECK_EQ(count(reopened.db(), "SELECT count(*) FROM system_changes"), 1);
    }
    {
        TempDir tmp;
        {
            HistoryStore fresh(tmp.path);
            fresh.db().exec("PRAGMA user_version = 6");
        }
        const auto before = commands::readFileBytes(tmp.path / "history.db");
        CHECK_THROWS(HistoryStore(tmp.path), "newer LooperCat");
        CHECK(commands::readFileBytes(tmp.path / "history.db") == before);
    }
    return testkit::summary("history_schema_tests");
}
