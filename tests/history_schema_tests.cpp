// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The store's versions: a history written by an earlier LooperCat opens,
// gains what the current version adds, and loses nothing it held. The
// version-1 file is built here from version 1's own definition — a copy of
// the tables as they shipped, not a reference to whatever Schema.h says today
// — so an edit to a released step, rather than a new one, fails this suite.

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

std::vector<std::string> column(sqlite::Db& db, const std::string& sql)
{
    std::vector<std::string> out;
    sqlite::Statement read(db, sql);
    while (read.step())
        out.push_back(read.isNull(0) ? std::string("<null>") : read.text(0));
    return out;
}

// The tables exactly as version 1 shipped (PR #79 / #83).
constexpr const char* kVersion1 = R"sql(
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
)sql";

// Not text: a NUL and two high bytes inside, so a text-shaped path would show.
const std::string kTake = std::string("the take's bytes\0\xff\x01 with a NUL inside", 37);

// What version 2 added (PR #88): the legacy import's ledger.
constexpr const char* kVersion2Addition = R"sql(
CREATE TABLE legacy_files(
    path     TEXT    PRIMARY KEY,
    op       INTEGER NOT NULL REFERENCES ops(seq),
    kind     TEXT    NOT NULL CHECK (kind IN ('take', 'document')),
    hash     BLOB    NOT NULL REFERENCES blobs_meta(hash),
    imported INTEGER NOT NULL
) STRICT;
CREATE INDEX legacy_files_by_op ON legacy_files(op);
)sql";

// A version-1 history as LooperCat 0.9.x left it: the storage properties the
// store demands, version 1's tables, and a life in them — a card, a session,
// a finished op with its take and bodies, and an op the app never finished.
void writeVersion(const fs::path& dir, int version)
{
    fs::create_directories(dir);
    auto db = sqlite::Db::open(dir / "history.db");
    db.exec("PRAGMA page_size = 16384");
    db.exec("PRAGMA auto_vacuum = INCREMENTAL");
    db.exec("PRAGMA journal_mode = DELETE");
    db.exec("PRAGMA foreign_keys = ON");
    db.exec(kVersion1);
    if (version >= 2)
        db.exec(kVersion2Addition);
    db.exec("PRAGMA user_version = " + std::to_string(version));
    db.exec("INSERT INTO cards(id, model, label, first_seen, last_seen) "
            "VALUES (1, 'RC-5', 'BOSS RC-5', 1000, 2000)");
    db.exec("INSERT INTO sessions(id, card, connected_at, disconnected_at) VALUES (1, 1, 1000, 2000)");
    db.exec("INSERT INTO ops(seq, id, session, kind, actor, status, at, note) "
            "VALUES (1, '2026-09-23T21-54-51-233b-5', 1, 'trim', 'app', 'done', 1500, 'trimmed')");
    db.exec("INSERT INTO ops(seq, id, session, kind, actor, status, at) "
            "VALUES (2, '2026-09-23T21-58-23-233b-9', 1, 'clear', 'app', 'pending', 1600)");
    db.exec("INSERT INTO slot_changes(op, slot, before_body, after_body) VALUES (1, 42, x'01', x'02')");
    const std::string hash = HistoryStore::contentHash(kTake);
    sqlite::Statement meta(db, "INSERT INTO blobs_meta(hash, size, created, pinned) VALUES (?1, ?2, 1500, 0)");
    meta.bindBlob(1, hash).bind(2, static_cast<std::int64_t>(kTake.size())).run();
    sqlite::Statement bytes(db, "INSERT INTO blobs(hash, bytes) VALUES (?1, ?2)");
    bytes.bindBlob(1, hash).bindBlob(2, kTake).run();
    sqlite::Statement audio(db, "INSERT INTO slot_audio(op, slot, side, track, name, size, hash) "
                                "VALUES (1, 42, 'before', 1, 'take.wav', ?2, ?1)");
    audio.bindBlob(1, hash).bind(2, static_cast<std::int64_t>(kTake.size())).run();
    if (version >= 2) {
        sqlite::Statement ledger(db, "INSERT INTO legacy_files(path, op, kind, hash, imported) "
                                     "VALUES ('trash/2026-09-01T21-35-46/042_1/take.wav', 1, 'take', ?1, 1500)");
        ledger.bindBlob(1, hash).run();
    }
}

void writeVersion1(const fs::path& dir) { writeVersion(dir, 1); }

} // namespace

int main()
{
    // --- version 1 opens as version 2 with everything it held ---
    {
        TempDir tmp;
        writeVersion1(tmp.path / "history");
        {
            auto raw = sqlite::Db::open(tmp.path / "history" / "history.db");
            CHECK_EQ(schema::pragmaInteger(raw, "user_version"), 1);
            CHECK_EQ(count(raw, "SELECT count(*) FROM sqlite_master WHERE name = 'legacy_files'"), 0);
        }

        HistoryStore store(tmp.path / "history");
        sqlite::Db& db = store.db();
        CHECK_EQ(schema::pragmaInteger(db, "user_version"), 4);
        CHECK_EQ(schema::kVersion, 4);
        CHECK_EQ(count(db, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = 'system_changes'"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = 'legacy_files'"), 1);
        // version 3's column, unpinned on every row that was there
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE pinned = 0"), 2);
        CHECK_THROWS(db.exec("UPDATE ops SET pinned = 2 WHERE seq = 1"), "CHECK");

        // nothing lost
        CHECK_EQ(count(db, "SELECT count(*) FROM cards WHERE model = 'RC-5' AND label = 'BOSS RC-5'"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM sessions WHERE disconnected_at = 2000"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops"), 2);
        CHECK_EQ(store.opStatus(1), std::string("done"));
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE seq = 1 AND note = 'trimmed' AND at = 1500"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_changes WHERE op = 1 AND slot = 42 "
                           "AND before_body = x'01' AND after_body = x'02'"),
                 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE op = 1 AND slot = 42 AND name = 'take.wav'"), 1);
        CHECK(store.takeBytes(HistoryStore::contentHash(kTake)) == kTake);
        // and the open-time rule still ran after the migration
        CHECK_EQ(store.opStatus(2), std::string("interrupted"));

        // the new table holds what version 2 promises: a ledger keyed by path,
        // whose bytes must exist and whose op must exist. (Blobs are bound
        // without a copy — Sqlite.h — so the hashes live in named strings.)
        const std::string kept = HistoryStore::contentHash(kTake);
        const std::string unknown = HistoryStore::contentHash("never stored");
        sqlite::Statement stray(db, "INSERT INTO legacy_files(path, op, kind, hash, imported) "
                                    "VALUES ('trash/x/001_1/x.wav', 1, 'take', ?1, 1)");
        stray.bindBlob(1, unknown);
        CHECK_THROWS(stray.run(), "FOREIGN KEY");
        sqlite::Statement noOp(db, "INSERT INTO legacy_files(path, op, kind, hash, imported) "
                                   "VALUES ('trash/x/001_1/x.wav', 99, 'take', ?1, 1)");
        noOp.bindBlob(1, kept);
        CHECK_THROWS(noOp.run(), "FOREIGN KEY");
        sqlite::Statement badKind(db, "INSERT INTO legacy_files(path, op, kind, hash, imported) "
                                      "VALUES ('trash/x/001_1/x.wav', 1, 'other', ?1, 1)");
        badKind.bindBlob(1, kept);
        CHECK_THROWS(badKind.run(), "CHECK");
        sqlite::Statement fine(db, "INSERT INTO legacy_files(path, op, kind, hash, imported) "
                                   "VALUES ('trash/x/001_1/x.wav', 1, 'take', ?1, 1)");
        fine.bindBlob(1, kept);
        fine.run();
        sqlite::Statement twice(db, "INSERT INTO legacy_files(path, op, kind, hash, imported) "
                                    "VALUES ('trash/x/001_1/x.wav', 1, 'document', ?1, 2)");
        twice.bindBlob(1, kept);
        CHECK_THROWS(twice.run(), "UNIQUE");
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files"), 1);
    }

    // --- the migration is idempotent and survives a reopen ---
    {
        TempDir tmp;
        writeVersion1(tmp.path / "history");
        { HistoryStore first(tmp.path / "history"); }
        HistoryStore second(tmp.path / "history");
        CHECK_EQ(schema::pragmaInteger(second.db(), "user_version"), 4);
        CHECK_EQ(count(second.db(), "SELECT count(*) FROM ops"), 2);
        CHECK_EQ(count(second.db(), "SELECT count(*) FROM sqlite_master WHERE name = 'legacy_files'"), 1);
    }

    // --- a migrated store and a fresh store are the same store ---
    {
        TempDir tmp;
        writeVersion1(tmp.path / "migrated");
        HistoryStore migrated(tmp.path / "migrated");
        HistoryStore fresh(tmp.path / "fresh");
        const std::string sql = "SELECT type || ' ' || name || ' ' || coalesce(sql, '') "
                                "FROM sqlite_master ORDER BY type, name";
        const auto a = column(migrated.db(), sql);
        const auto b = column(fresh.db(), sql);
        CHECK(a == b);
        CHECK(!a.empty());
        CHECK_EQ(count(fresh.db(), "SELECT count(*) FROM sqlite_master WHERE type = 'table'"), 9);
        CHECK_EQ(schema::pragmaInteger(fresh.db(), "user_version"), 4);
    }

    // --- a version-2 store (the ledger, no pins) opens as version 3, rows intact ---
    {
        TempDir tmp;
        writeVersion(tmp.path / "history", 2);
        HistoryStore store(tmp.path / "history");
        sqlite::Db& db = store.db();
        CHECK_EQ(schema::pragmaInteger(db, "user_version"), 4);
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files WHERE kind = 'take'"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE pinned = 0"), 2);
        CHECK(store.takeBytes(HistoryStore::contentHash(kTake)) == kTake);
        store.pinOp(1, true);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE pinned = 1"), 1);
        // and it is the same store a fresh one is
        HistoryStore fresh(tmp.path / "fresh");
        const std::string sql = "SELECT type || ' ' || name || ' ' || coalesce(sql, '') "
                                "FROM sqlite_master ORDER BY type, name";
        CHECK(column(db, sql) == column(fresh.db(), sql));
    }

    // --- version 1's storage properties are still demanded of an upgraded store ---
    {
        TempDir tmp;
        fs::create_directories(tmp.path / "history");
        {
            auto db = sqlite::Db::open(tmp.path / "history" / "history.db");
            db.exec(kVersion1); // default pages, no incremental vacuum: a store that cannot give space back
            db.exec("PRAGMA user_version = 1");
        }
        CHECK_THROWS(HistoryStore(tmp.path / "history"), "auto_vacuum");
    }

    // --- a version-3 store (pins, no settings rows) opens as version 4, rows and pins intact ---
    {
        TempDir tmp;
        writeVersion(tmp.path / "history", 2);
        {
            auto raw = sqlite::Db::open(tmp.path / "history" / "history.db");
            raw.exec("ALTER TABLE ops ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0, 1))");
            raw.exec("UPDATE ops SET pinned = 1 WHERE seq = 1");
            raw.exec("PRAGMA user_version = 3");
        }
        HistoryStore store(tmp.path / "history");
        sqlite::Db& db = store.db();
        CHECK_EQ(schema::pragmaInteger(db, "user_version"), 4);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops"), 2);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE pinned = 1"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files"), 1);
        CHECK(store.takeBytes(HistoryStore::contentHash(kTake)) == kTake);
        CHECK_EQ(count(db, "SELECT count(*) FROM system_changes"), 0);
        // the new table holds what version 4 promises
        CHECK_THROWS(db.exec("INSERT INTO system_changes(op, section, before, after) VALUES (1, 'MEM', 'a', 'b')"), "CHECK");
        CHECK_THROWS(db.exec("INSERT INTO system_changes(op, section, before, after) VALUES (99, 'CTL', 'a', 'b')"), "FOREIGN KEY");
        db.exec("INSERT INTO system_changes(op, section, before, after) VALUES (1, 'CTL', 'a', 'b')");
        CHECK_THROWS(db.exec("INSERT INTO system_changes(op, section, before, after) VALUES (1, 'CTL', 'c', 'd')"), "UNIQUE");
        HistoryStore fresh(tmp.path / "fresh");
        const std::string sql = "SELECT type || ' ' || name || ' ' || coalesce(sql, '') "
                                "FROM sqlite_master ORDER BY type, name";
        CHECK(column(db, sql) == column(fresh.db(), sql));
    }

    // --- one step per version, and the released step is the one that shipped ---
    {
        CHECK_EQ(sizeof(schema::kSteps) / sizeof(schema::kSteps[0]), 4u);
        CHECK_EQ(std::string(schema::kSteps[0]), std::string(kVersion1));
        CHECK_EQ(std::string(schema::kSteps[1]), std::string(kVersion2Addition));
    }

    return testkit::summary("history_schema_tests");
}
