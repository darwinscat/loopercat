// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The legacy import (issue #72, the folders from before the store), attacked
// from what it promises:
//
//   - a folder is recorded once however often the import runs, and a run cut
//     off mid-way is finished by the next one, not doubled
//   - every byte comes back: a take and a document read out of the store are
//     the file, byte for byte
//   - bytes the store already holds are not stored again — not for a take the
//     app kept, not for two legacy files with the same content
//   - a folder the app itself recorded is left alone
//   - what is not the app's shape is skipped by name, with a reason, and the
//     neighbours are imported anyway
//   - nothing under the folders is touched
//   - the rows say only what the folder said: 'legacy', the stamp's time, the
//     folder's name — no command, no after-state

#include "support.hpp"

#include "../app/history/HistoryStore.h"
#include "../app/history/LegacyImport.h"

#include <loopercat/Commands.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>

using namespace loopercat;
using history::HistoryStore;
using history::OpStatus;
namespace legacy = history::legacy;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-legacy-" + std::to_string(stamp));
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

std::string text(sqlite::Db& db, const std::string& sql)
{
    sqlite::Statement read(db, sql);
    if (!read.step())
        throw Error("query returned no row: " + sql);
    return read.isNull(0) ? std::string("<null>") : read.text(0);
}

// Deterministic bytes that are not text — every value 0..255 appears.
std::string take(std::size_t size, unsigned seed)
{
    std::string out(size, '\0');
    std::uint32_t x = 2463534242u ^ seed;
    for (auto& c : out) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = static_cast<char>(x & 0xFF);
    }
    return out;
}

void put(const fs::path& file, std::string_view bytes)
{
    fs::create_directories(file.parent_path());
    commands::writeFileBytes(file, bytes);
}

// A data home as the old app left it: backups/<stamp>/MEMORY1+2.RC0 and
// trash/<stamp>/<slot dir>/<take>. Returns the bytes it wrote, by path.
struct Home {
    fs::path root;
    std::map<std::string, std::string> written; // '/'-separated path -> bytes

    explicit Home(const fs::path& dir) : root(dir) {}

    // Two backups never hold the same document: slot 1 carries the seed.
    void backup(const std::string& stamp, unsigned seed)
    {
        const std::string document = rc0::replaceSlotBody(
            testkit::syntheticMemoryText(), 1,
            testkit::syntheticSlotBody("Backup " + std::to_string(seed)));
        file("backups/" + stamp + "/MEMORY1.RC0", rc0::setTailMarker(document, 1));
        file("backups/" + stamp + "/MEMORY2.RC0", rc0::setTailMarker(document, 2));
    }

    void trash(const std::string& stamp, int slot, const std::string& name, const std::string& bytes)
    {
        file("trash/" + stamp + "/" + volume::slotDirName(slot) + "/" + name, bytes);
    }

    void file(const std::string& path, const std::string& bytes)
    {
        put(root / fs::path(path), bytes);
        written[path] = bytes;
    }
};

// Every regular file under the home, by relative path — what a run must leave
// exactly as it found.
std::map<std::string, std::string> snapshot(const fs::path& root)
{
    std::map<std::string, std::string> map;
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it)
        if (it->is_regular_file())
            map[fs::relative(it->path(), root).generic_string()] = commands::readFileBytes(it->path());
    return map;
}

std::int64_t localMs(int year, int month, int day, int hour, int minute, int second)
{
    std::tm t {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = second;
    t.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&t)) * 1000;
}

bool skippedWith(const legacy::Report& report, const std::string& path, const std::string& reason)
{
    for (const auto& s : report.skipped)
        if (s.path == path && s.reason.find(reason) != std::string::npos)
            return true;
    return false;
}

} // namespace

int main()
{
    // --- the stamp: what the app wrote, and nothing that merely looks like it ---
    {
        const auto old = legacy::parseStamp("2026-09-01T21-35-46");
        CHECK(old.has_value());
        CHECK(old && !old->minted);
        CHECK(old && old->atMs == localMs(2026, 9, 1, 21, 35, 46));

        const auto minted = legacy::parseStamp("2026-09-23T21-58-23-233b-9");
        CHECK(minted.has_value());
        CHECK(minted && minted->minted);
        CHECK(minted && minted->atMs == localMs(2026, 9, 23, 21, 58, 23));
        CHECK(legacy::parseStamp("2026-09-23T21-58-23-0f0f-1234").has_value());

        CHECK(!legacy::parseStamp("").has_value());
        CHECK(!legacy::parseStamp("2026-09-01").has_value());
        CHECK(!legacy::parseStamp("2026-09-01 21-35-46").has_value());
        CHECK(!legacy::parseStamp("2026-09-01T21:35:46").has_value());
        CHECK(!legacy::parseStamp("2026-13-01T21-35-46").has_value());
        CHECK(!legacy::parseStamp("2026-00-01T21-35-46").has_value());
        CHECK(!legacy::parseStamp("2026-09-01T24-35-46").has_value());
        CHECK(!legacy::parseStamp("2026-09-01T21-60-46").has_value());
        CHECK(!legacy::parseStamp("2026-09-01T21-35-46x").has_value());
        CHECK(!legacy::parseStamp("2026-09-01T21-35-46-233b").has_value());   // tag, no counter
        CHECK(!legacy::parseStamp("2026-09-01T21-35-46-233b-").has_value());
        CHECK(!legacy::parseStamp("2026-09-01T21-35-46-ZZZZ-1").has_value()); // not hex
        CHECK(!legacy::parseStamp("2026-09-01T21-35-46-233B-1").has_value()); // opid writes lowercase
        CHECK(!legacy::parseStamp("2026-09-01T21-35-46-233b-1x").has_value());
        CHECK(!legacy::parseStamp("2026-09-01T21-35-46-23-1").has_value());
        CHECK(!legacy::parseStamp("1969-12-31T23-59-59").has_value());
    }

    // --- a home as the old app left it: one op per stamp, the takes as the
    //     slots' 'before', the documents whole, every byte back ---
    {
        TempDir tmp;
        Home home(tmp.path / "home");
        home.backup("2026-09-01T21-35-46", 1);
        home.trash("2026-09-01T21-35-46", 32, "032_1.WAV", take(70000, 1));
        home.trash("2026-09-01T21-35-46", 33, "033_1.WAV", take(50000, 2));
        home.backup("2026-09-03T18-32-54", 2);                            // a backup alone
        home.trash("2026-09-08T20-00-34", 14, "loop 14.wav", take(30000, 3)); // a trash alone
        const auto before = snapshot(home.root);

        HistoryStore store(tmp.path / "history");
        const legacy::Report report = legacy::importFolders(store, home.root, 5000);
        CHECK_EQ(report.operations, 3);
        CHECK_EQ(report.takes, 3);
        CHECK_EQ(report.documents, 4);
        CHECK_EQ(report.deduplicated, 0);
        CHECK_EQ(report.alreadyImported, 0);
        CHECK_EQ(report.skipped.size(), 0u);

        sqlite::Db& db = store.db();
        CHECK_EQ(count(db, "SELECT count(*) FROM ops"), 3);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE actor = 'legacy' AND kind = 'legacy' "
                           "AND status = 'done'"),
                 3);
        CHECK_EQ(count(db, "SELECT at FROM ops WHERE id = '2026-09-01T21-35-46'"),
                 localMs(2026, 9, 1, 21, 35, 46));
        CHECK_EQ(text(db, "SELECT note FROM ops WHERE id = '2026-09-01T21-35-46'"),
                 std::string("backups/2026-09-01T21-35-46, trash/2026-09-01T21-35-46"));
        CHECK_EQ(text(db, "SELECT note FROM ops WHERE id = '2026-09-03T18-32-54'"),
                 std::string("backups/2026-09-03T18-32-54"));
        CHECK_EQ(text(db, "SELECT note FROM ops WHERE id = '2026-09-08T20-00-34'"),
                 std::string("trash/2026-09-08T20-00-34"));
        // the folders are in time order in the timeline
        CHECK(count(db, "SELECT seq FROM ops WHERE id = '2026-09-01T21-35-46'")
              < count(db, "SELECT seq FROM ops WHERE id = '2026-09-08T20-00-34'"));

        // a trashed take is what the slot held before — no 'after' is claimed
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio"), 3);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE side = 'before' AND track = 1"), 3);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE slot = 32 AND name = '032_1.WAV' "
                           "AND size = 70000"),
                 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE slot = 14 AND name = 'loop 14.wav'"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_changes"), 0); // no after-state invented
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files"), 7);
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files WHERE kind = 'take'"), 3);
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files WHERE kind = 'document'"), 4);
        CHECK_EQ(count(db, "SELECT count(*) FROM blobs"), 7);
        CHECK_EQ(count(db, "SELECT count(*) FROM blobs_meta WHERE pinned = 0 AND released IS NULL"), 7);

        // every file, byte for byte, by the hash its ledger row names
        for (const auto& [path, bytes] : home.written) {
            sqlite::Statement row(db, "SELECT hash FROM legacy_files WHERE path = ?1");
            row.bindText(1, path);
            CHECK(row.step());
            CHECK(row.blob(0) == HistoryStore::contentHash(bytes));
            CHECK(store.takeBytes(HistoryStore::contentHash(bytes)) == bytes);
        }
        // the take's row and its ledger row name the same bytes
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files l JOIN slot_audio a ON a.op = l.op "
                           "AND a.hash = l.hash WHERE l.kind = 'take'"),
                 3);

        // booked on a card the import does not pretend to know, in one closed session
        CHECK_EQ(count(db, "SELECT count(*) FROM cards"), 1);
        CHECK_EQ(text(db, "SELECT model FROM cards"), std::string("unknown"));
        CHECK_EQ(count(db, "SELECT count(*) FROM sessions"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM sessions WHERE disconnected_at IS NOT NULL"), 1);
        CHECK_EQ(count(db, "SELECT count(DISTINCT session) FROM ops"), 1);

        // the folders are exactly as they were
        CHECK(snapshot(home.root) == before);
        CHECK_EQ(before.size(), 7u);

        // --- a second run records nothing twice ---
        const legacy::Report again = legacy::importFolders(store, home.root, 6000);
        CHECK_EQ(again.operations, 0);
        CHECK_EQ(again.takes, 0);
        CHECK_EQ(again.documents, 0);
        CHECK_EQ(again.alreadyImported, 7);
        CHECK_EQ(again.skipped.size(), 0u);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops"), 3);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio"), 3);
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files"), 7);
        CHECK_EQ(count(db, "SELECT count(*) FROM blobs"), 7);
        CHECK_EQ(count(db, "SELECT count(*) FROM sessions"), 1); // no session for nothing
        CHECK(snapshot(home.root) == before);

        // --- and after a reopen, the same ---
        HistoryStore reopened(tmp.path / "history");
        const legacy::Report third = legacy::importFolders(reopened, home.root, 7000);
        CHECK_EQ(third.alreadyImported, 7);
        CHECK_EQ(count(reopened.db(), "SELECT count(*) FROM legacy_files"), 7);
    }

    // --- bytes the store holds are not stored again ---
    {
        TempDir tmp;
        Home home(tmp.path / "home");
        const std::string known = take(40000, 7);
        const std::string twice = take(20000, 8);
        home.trash("2026-08-01T17-33-44", 5, "005_1.WAV", known);     // the app kept this one already
        home.trash("2026-08-01T17-34-18", 6, "006_1.WAV", twice);     // and these two are one take
        home.trash("2026-08-01T17-34-18", 7, "007_1.WAV", twice);

        HistoryStore store(tmp.path / "history");
        const auto session = store.openSession(store.card("RC-5", "BOSS RC-5", 1000), 1000);
        const auto op = store.beginOp(session, "op-app", "trim", 2000);
        store.keepAudio(op, 5, 1, "005_1.WAV", known, 2000);
        store.finishOp(op, OpStatus::done, "");
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM blobs"), 1);

        const legacy::Report report = legacy::importFolders(store, home.root, 5000);
        CHECK_EQ(report.takes, 3);
        CHECK_EQ(report.deduplicated, 2);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM blobs"), 2);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM blobs_meta"), 2);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM slot_audio"), 4); // the app's row and three legacy rows
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM legacy_files"), 3);
        CHECK(store.takeBytes(HistoryStore::contentHash(known)) == known);
        CHECK(store.takeBytes(HistoryStore::contentHash(twice)) == twice);
    }

    // --- a take released earlier (#74) gets its bytes back from the folder ---
    {
        TempDir tmp;
        Home home(tmp.path / "home");
        const std::string bytes = take(15000, 9);
        const std::string hash = HistoryStore::contentHash(bytes);
        home.trash("2026-08-01T17-33-44", 5, "005_1.WAV", bytes);

        HistoryStore store(tmp.path / "history");
        const auto session = store.openSession(store.card("RC-5", "BOSS RC-5", 1000), 1000);
        const auto op = store.beginOp(session, "op-app", "trim", 2000);
        store.keepAudio(op, 5, 1, "005_1.WAV", bytes, 2000);
        store.finishOp(op, OpStatus::done, "");
        {
            sqlite::Transaction tx(store.db());
            sqlite::Statement drop(store.db(), "DELETE FROM blobs WHERE hash = ?1");
            drop.bindBlob(1, hash).run();
            sqlite::Statement mark(store.db(), "UPDATE blobs_meta SET released = 3000 WHERE hash = ?1");
            mark.bindBlob(1, hash).run();
            tx.commit();
        }
        CHECK(!store.takeBytes(hash).has_value());
        const legacy::Report report = legacy::importFolders(store, home.root, 5000);
        CHECK_EQ(report.takes, 1);
        CHECK_EQ(report.deduplicated, 0);
        CHECK(store.takeBytes(hash) == bytes);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM blobs_meta WHERE released IS NULL"), 1);
    }

    // --- a folder the app recorded itself is left alone ---
    {
        TempDir tmp;
        Home home(tmp.path / "home");
        home.backup("2026-09-23T21-58-23-233b-9", 1);
        home.trash("2026-09-23T21-58-23-233b-9", 43, "take.wav", take(10000, 4));
        home.backup("2026-09-23T21-58-06-233b-8", 2); // minted, but the history lost it
        const auto before = snapshot(home.root);

        HistoryStore store(tmp.path / "history");
        const auto session = store.openSession(store.card("RC-5", "BOSS RC-5", 1000), 1000);
        const auto op = store.beginOp(session, "2026-09-23T21-58-23-233b-9", "clear", 2000);
        store.finishOp(op, OpStatus::done, "");

        const legacy::Report report = legacy::importFolders(store, home.root, 5000);
        CHECK(skippedWith(report, "backups/2026-09-23T21-58-23-233b-9", "recorded by the app"));
        CHECK(skippedWith(report, "trash/2026-09-23T21-58-23-233b-9", "recorded by the app"));
        CHECK_EQ(report.skipped.size(), 2u);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM ops"), 2); // the app's, and one adopted
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM ops WHERE id = '2026-09-23T21-58-23-233b-9' "
                                   "AND actor = 'app' AND kind = 'clear'"),
                 1);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM slot_audio"), 0);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM legacy_files WHERE path LIKE '%233b-9%'"), 0);
        // the one whose op the history does not have is a folder like any other
        CHECK_EQ(report.operations, 1);
        CHECK_EQ(report.documents, 2);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM ops WHERE id = '2026-09-23T21-58-06-233b-8' "
                                   "AND actor = 'legacy'"),
                 1);
        CHECK(snapshot(home.root) == before);
    }

    // --- a run that stopped mid-stamp is finished, not doubled ---
    {
        TempDir tmp;
        Home home(tmp.path / "home");
        home.backup("2026-09-01T21-35-46", 1);
        home.trash("2026-09-01T21-35-46", 32, "032_1.WAV", take(7000, 1));
        home.trash("2026-09-01T21-35-46", 33, "033_1.WAV", take(7000, 2));

        HistoryStore store(tmp.path / "history");
        // the op row and one of its files landed, then the app stopped
        const auto session = store.openSession(store.card("unknown", "legacy folders", 100), 100);
        const auto op = store.recordLegacyOp(session, "2026-09-01T21-35-46",
                                             localMs(2026, 9, 1, 21, 35, 46), "trash/2026-09-01T21-35-46");
        store.keepLegacyTake(op, "trash/2026-09-01T21-35-46/032_1/032_1.WAV", 32, 1, "032_1.WAV",
                             take(7000, 1), 100);
        store.closeSession(session, 100);

        const legacy::Report report = legacy::importFolders(store, home.root, 5000);
        CHECK_EQ(report.operations, 0);
        CHECK_EQ(report.alreadyImported, 1);
        CHECK_EQ(report.takes, 1);
        CHECK_EQ(report.documents, 2);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM ops"), 1);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM slot_audio"), 2);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM legacy_files WHERE op = " + std::to_string(op)), 4);
    }

    // --- the ledger refuses a path twice on its own ---
    {
        TempDir tmp;
        HistoryStore store(tmp.path / "history");
        const auto session = store.openSession(store.card("unknown", "legacy folders", 100), 100);
        const auto op = store.recordLegacyOp(session, "2026-09-01T21-35-46", 1000, "x");
        store.keepLegacyDocument(op, "backups/2026-09-01T21-35-46/MEMORY1.RC0", "doc", 100);
        CHECK(store.legacyFileImported("backups/2026-09-01T21-35-46/MEMORY1.RC0"));
        CHECK(!store.legacyFileImported("backups/2026-09-01T21-35-46/MEMORY2.RC0"));
        CHECK_THROWS(store.keepLegacyDocument(op, "backups/2026-09-01T21-35-46/MEMORY1.RC0", "other", 100),
                     "");
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM blobs"), 1); // the refused bytes did not stay
        CHECK_THROWS(store.keepLegacyTake(op, "trash/2026-09-01T21-35-46/100_1/x", 100, 1, "x", "b", 100),
                     "");
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM legacy_files"), 1);
        CHECK_THROWS(store.recordLegacyOp(session, "2026-09-01T21-35-46", 1000, "x"), ""); // one op per stamp
    }

    // --- what is not the app's shape is skipped by name; the rest goes on ---
    {
        TempDir tmp;
        Home home(tmp.path / "home");
        home.backup("2026-07-22T17-36-55", 1);                                         // fine
        home.trash("2026-07-22T17-36-55", 14, "014_1.WAV", take(5000, 1));             // fine
        home.file("backups/notes.txt", "a file where folders go");
        home.file("backups/some-other-folder/MEMORY1.RC0", "not a stamp");
        home.file("backups/2026-07-22T17-37-37/nested/MEMORY1.RC0", "a folder inside a backup");
        home.file("backups/2026-07-22T17-37-37/MEMORY1.RC0", "the good neighbour");
        home.file("trash/2026-07-22T17-39-58/README", "a file where slot folders go");
        home.file("trash/2026-07-22T17-39-58/100_1/x.wav", "slot out of range");
        home.file("trash/2026-07-22T17-39-58/7_1/x.wav", "not the app's spelling");
        home.file("trash/2026-07-22T17-39-58/007_01/x.wav", "not the app's spelling either");
        home.file("trash/2026-07-22T17-39-58/007_1/deeper/x.wav", "a folder inside a slot folder");
        home.file("trash/2026-07-22T17-39-58/008_1/loop.wav", "the good neighbour");
        home.file("trash/2026-07-22T17-44-51/.DS_Store", "junk is not reported");
        home.file("trash/2026-07-22T17-44-51/009_1/._loop.wav", "junk is not reported either");
        home.file("trash/2026-07-22T17-44-51/009_1/loop.wav", "the good neighbour");
        home.file("backups/.DS_Store", "junk at the root");
        fs::create_directories(home.root / "backups" / "2026-07-22T17-44-52"); // an empty stamp
        fs::create_directories(home.root / "trash" / "2026-07-22T17-44-53" / "010_1"); // an empty slot
        const auto before = snapshot(home.root);

        HistoryStore store(tmp.path / "history");
        const legacy::Report report = legacy::importFolders(store, home.root, 5000);

        CHECK(skippedWith(report, "backups/notes.txt", "not a folder"));
        CHECK(skippedWith(report, "backups/some-other-folder", "not an operation folder"));
        CHECK(skippedWith(report, "backups/2026-07-22T17-37-37/nested", "not a file"));
        CHECK(skippedWith(report, "trash/2026-07-22T17-39-58/README", "not a slot folder"));
        CHECK(skippedWith(report, "trash/2026-07-22T17-39-58/100_1", "not a slot folder"));
        CHECK(skippedWith(report, "trash/2026-07-22T17-39-58/7_1", "not a slot folder"));
        CHECK(skippedWith(report, "trash/2026-07-22T17-39-58/007_01", "not a slot folder"));
        CHECK(skippedWith(report, "trash/2026-07-22T17-39-58/007_1/deeper", "not a file"));
        CHECK(skippedWith(report, "backups/2026-07-22T17-44-52", "nothing to import"));
        CHECK(skippedWith(report, "trash/2026-07-22T17-44-53", "nothing to import"));
        CHECK_EQ(report.skipped.size(), 10u);
        for (const auto& s : report.skipped) {
            CHECK(!s.reason.empty());
            CHECK(s.path.find(".DS_Store") == std::string::npos);
            CHECK(s.path.find("._") == std::string::npos);
        }

        // the neighbours: four stamps with something in them
        CHECK_EQ(report.operations, 4);
        CHECK_EQ(report.documents, 3);
        CHECK_EQ(report.takes, 3);
        sqlite::Db& db = store.db();
        CHECK_EQ(count(db, "SELECT count(*) FROM ops"), 4);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE id IN ('2026-07-22T17-44-52', "
                           "'2026-07-22T17-44-53', 'some-other-folder')"),
                 0);
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files"), 6);
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files WHERE path IN ("
                           "'backups/2026-07-22T17-36-55/MEMORY1.RC0', "
                           "'backups/2026-07-22T17-36-55/MEMORY2.RC0', "
                           "'trash/2026-07-22T17-36-55/014_1/014_1.WAV', "
                           "'backups/2026-07-22T17-37-37/MEMORY1.RC0', "
                           "'trash/2026-07-22T17-39-58/008_1/loop.wav', "
                           "'trash/2026-07-22T17-44-51/009_1/loop.wav')"),
                 6);
        CHECK_EQ(count(db, "SELECT count(*) FROM legacy_files WHERE path LIKE '%.DS_Store%' "
                           "OR path LIKE '%._loop%' OR path LIKE '%notes.txt%'"),
                 0);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE slot = 8 AND name = 'loop.wav'"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE slot = 100 OR slot = 7"), 0);
        CHECK(snapshot(home.root) == before);
        CHECK_EQ(before.size(), 17u);
    }

#ifndef _WIN32
    // --- a file that cannot be read is named, the run goes on, and a later
    //     run picks it up once it can be read ---
    {
        TempDir tmp;
        Home home(tmp.path / "home");
        home.trash("2026-08-01T17-33-44", 5, "locked.wav", take(3000, 1));
        home.trash("2026-08-01T17-33-44", 6, "open.wav", take(3000, 2));
        const fs::path locked = home.root / "trash" / "2026-08-01T17-33-44" / "005_1" / "locked.wav";
        fs::permissions(locked, fs::perms::none);

        HistoryStore store(tmp.path / "history");
        const legacy::Report first = legacy::importFolders(store, home.root, 5000);
        CHECK(skippedWith(first, "trash/2026-08-01T17-33-44/005_1/locked.wav", "cannot read"));
        CHECK_EQ(first.takes, 1);
        CHECK_EQ(first.operations, 1);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM legacy_files"), 1);

        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_write);
        const legacy::Report second = legacy::importFolders(store, home.root, 6000);
        CHECK_EQ(second.skipped.size(), 0u);
        CHECK_EQ(second.takes, 1);
        CHECK_EQ(second.operations, 0);
        CHECK_EQ(second.alreadyImported, 1);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM ops"), 1);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM legacy_files"), 2);
        CHECK(store.takeBytes(HistoryStore::contentHash(take(3000, 1))) == take(3000, 1));
    }
#endif

    // --- a home without the folders has nothing to import, and says so quietly ---
    {
        TempDir tmp;
        HistoryStore store(tmp.path / "history");
        const legacy::Report report = legacy::importFolders(store, tmp.path / "nowhere", 5000);
        CHECK_EQ(report.operations, 0);
        CHECK_EQ(report.takes + report.documents + report.alreadyImported, 0);
        CHECK_EQ(report.skipped.size(), 0u);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM sessions"), 0);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM cards"), 0);
    }

    // --- a large take goes through whole ---
    {
        TempDir tmp;
        Home home(tmp.path / "home");
        const std::string big = take(24u * 1024u * 1024u, 5);
        home.trash("2026-08-01T17-33-44", 1, "big.wav", big);
        HistoryStore store(tmp.path / "history");
        const legacy::Report report = legacy::importFolders(store, home.root, 5000);
        CHECK_EQ(report.takes, 1);
        CHECK(store.takeBytes(HistoryStore::contentHash(big)) == big);
        CHECK_EQ(count(store.db(), "SELECT size FROM slot_audio WHERE slot = 1"),
                 static_cast<std::int64_t>(big.size()));
    }

    // --- the run in one sentence: what happened, never zeros dressed as news ---
    {
        legacy::Report none;
        CHECK_EQ(legacy::describe(none), std::string("No folders from before the history were found."));

        legacy::Report first;
        first.operations = 360;
        first.takes = 47;
        first.documents = 1406;
        first.deduplicated = 658;
        first.skipped = std::vector<legacy::Skipped>(11, { "backups/x", "already in the history" });
        CHECK_EQ(legacy::describe(first),
                 std::string("Imported 360 operations: 47 takes and 1406 documents "
                             "(658 already kept, counted once). "
                             "11 folders skipped, the reasons are in operations.log."));

        legacy::Report again;
        again.alreadyImported = 1453;
        again.skipped = std::vector<legacy::Skipped>(11, { "backups/x", "already in the history" });
        CHECK_EQ(legacy::describe(again),
                 std::string("Nothing new to import. 1453 files were already in the history. "
                             "11 folders skipped, the reasons are in operations.log."));

        legacy::Report one;
        one.operations = 1;
        one.takes = 1;
        one.documents = 0;
        one.alreadyImported = 1;
        one.skipped = { { "trash/notes.txt", "not a folder" } };
        CHECK_EQ(legacy::describe(one),
                 std::string("Imported 1 operation: 1 take and 0 documents. "
                             "1 file was already in the history. "
                             "1 folder skipped, the reasons are in operations.log."));

        legacy::Report onlySkipped;
        onlySkipped.skipped = { { "backups/random", "not an operation folder" } };
        CHECK_EQ(legacy::describe(onlySkipped),
                 std::string("Nothing new to import. 1 folder skipped, the reasons are in operations.log."));
        // the sentence is for a toast: skipped paths and reasons stay out of it
        CHECK(legacy::describe(onlySkipped).find("random") == std::string::npos);
        CHECK(legacy::describe(first).find("backups/x") == std::string::npos);
    }

    return testkit::summary("legacy_import_tests");
}
