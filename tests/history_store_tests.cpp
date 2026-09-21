// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The history store (issue #72), attacked from what it promises rather than
// from how it is written:
//
//   - the storage properties it rests on are real, read back from the files:
//     a rollback journal on BOTH (a WAL file takes no part in an atomic commit
//     over attached databases), and an audio store that can return space
//   - a take and the row naming it land together or not at all — a failed row
//     leaves no bytes behind
//   - a kept take comes back byte-exact, is kept once however often it
//     arrives, and survives reopening
//   - the lifecycle cannot lie: an op the app never finished reads as
//     interrupted, never as done; nothing finishes twice
//   - the store refuses what it cannot read correctly: a newer version, an
//     audio file without incremental vacuum, two files that are not a pair

#include "support.hpp"

#include "../app/history/HistoryStore.h"
#include "../app/history/Schema.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

using namespace loopercat;
using history::HistoryStore;
using history::OpStatus;
namespace fs = std::filesystem;
namespace schema = history::schema;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-history-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

std::string hex(std::string_view raw)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        out += digits[b >> 4];
        out += digits[b & 0xF];
    }
    return out;
}

std::int64_t count(sqlite::Db& db, const std::string& sql)
{
    sqlite::Statement read(db, sql);
    if (!read.step())
        throw Error("count returned no row: " + sql);
    return read.integer(0);
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

// A store with one open session, ready for operations.
struct Ready {
    HistoryStore store;
    std::int64_t session;
    explicit Ready(const fs::path& dir) : store(dir)
    {
        session = store.openSession(store.card("RC-5", "BOSS RC-5", 1000), 1000);
    }
};

} // namespace

int main()
{
    // --- a fresh directory becomes a verified pair ---
    {
        TempDir tmp;
        HistoryStore store(tmp.path / "history");
        CHECK(fs::exists(tmp.path / "history" / "history.db"));
        CHECK(fs::exists(tmp.path / "history" / "audio.db"));
        CHECK_EQ(schema::pragmaText(store.db(), "main.journal_mode"), std::string("delete"));
        CHECK_EQ(schema::pragmaText(store.db(), "audio.journal_mode"), std::string("delete"));
        CHECK_EQ(schema::pragmaInteger(store.db(), "audio.auto_vacuum"), 2); // INCREMENTAL
        CHECK_EQ(schema::pragmaInteger(store.db(), "audio.page_size"), 16384);
        CHECK_EQ(schema::pragmaInteger(store.db(), "foreign_keys"), 1);
        CHECK_EQ(schema::pragmaInteger(store.db(), "main.user_version"), schema::kVersion);
    }

    // --- the key is SHA-256, checked against the published vectors ---
    {
        CHECK_EQ(hex(HistoryStore::contentHash("")),
                 std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        CHECK_EQ(hex(HistoryStore::contentHash("abc")),
                 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        CHECK_EQ(HistoryStore::contentHash(take(4096, 1)).size(), 32u);
    }

    // --- a kept take comes back byte-exact, named by its row, after a reopen ---
    {
        TempDir tmp;
        const std::string bytes = take(100000, 7);
        const std::string hash = HistoryStore::contentHash(bytes);
        {
            Ready r(tmp.path);
            const auto op = r.store.beginOp(r.session, "op-a", "trim", 2000);
            r.store.keepAudio(op, 5, 1, "005_1.WAV", bytes, 2000);
            r.store.finishOp(op, OpStatus::done, "");
        }
        HistoryStore reopened(tmp.path);
        CHECK(reopened.takeBytes(hash) == bytes);
        CHECK_EQ(count(reopened.db(), "SELECT count(*) FROM slot_audio WHERE slot = 5 AND "
                                      "side = 'before' AND name = '005_1.WAV' AND size = 100000"),
                 1);
        CHECK(!reopened.takeBytes(HistoryStore::contentHash("never kept")).has_value());
    }

    // --- a take is kept once, however often it arrives ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string same = take(50000, 3);
        const auto op1 = r.store.beginOp(r.session, "op-1", "normalize", 2000);
        r.store.keepAudio(op1, 32, 1, "032_1.WAV", same, 2000);
        const auto op2 = r.store.beginOp(r.session, "op-2", "normalize", 2001);
        r.store.keepAudio(op2, 33, 1, "033_1.WAV", same, 2001);
        r.store.keepAudio(op2, 34, 1, "034_1.WAV", take(50000, 4), 2001);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM audio.blobs"), 2);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs_meta"), 2);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM slot_audio"), 3);
    }

    // --- the bytes and the row land together or not at all ---
    {
        // An op that does not exist: the row's foreign key fails AFTER the
        // bytes were inserted in the same transaction. Nothing may remain —
        // not the bytes, not their metadata.
        TempDir tmp;
        Ready r(tmp.path);
        CHECK_THROWS(r.store.keepAudio(9999, 5, 1, "orphan.wav", take(20000, 9), 2000), "");
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM audio.blobs"), 0);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs_meta"), 0);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM slot_audio"), 0);

        // The same for a row the schema refuses on its own terms.
        const auto op = r.store.beginOp(r.session, "op-bad-slot", "clear", 2000);
        CHECK_THROWS(r.store.keepAudio(op, 100, 1, "x.wav", take(1000, 1), 2000), "");
        CHECK_THROWS(r.store.keepAudio(op, 0, 1, "x.wav", take(1000, 1), 2000), "");
        CHECK_THROWS(r.store.keepAudio(op, 5, 0, "x.wav", take(1000, 1), 2000), "");
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM audio.blobs"), 0);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs_meta"), 0);
    }

    // --- bodies are kept byte for byte, before and after ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string before = std::string("<mem id=\"4\">", 12) + std::string("\0\xff\x01", 3);
        const std::string after = std::string("<mem id=\"4\">", 12) + std::string("\xfe\0\0", 3);
        const auto op = r.store.beginOp(r.session, "op-rename", "rename", 2000);
        r.store.recordBodies(op, { { 5, before, after }, { 6, "b6", "a6" } });
        sqlite::Statement read(r.store.db(), "SELECT before_body, after_body FROM slot_changes "
                                             "WHERE op = ?1 AND slot = 5");
        read.bind(1, op);
        CHECK(read.step());
        CHECK(read.blob(0) == before);
        CHECK(read.blob(1) == after);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM slot_changes"), 2);
        // one slot twice in one op is not a change the core can report
        CHECK_THROWS(r.store.recordBodies(op, { { 7, "x", "y" }, { 7, "y", "z" } }), "");
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM slot_changes WHERE slot = 7"), 0);
    }

    // --- a landed take is named and hashed, never copied ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string landed = take(30000, 11);
        const auto op = r.store.beginOp(r.session, "op-push", "push", 2000);
        r.store.recordLanded(op, 9, 1, "new.wav", landed);
        sqlite::Statement read(r.store.db(), "SELECT side, size, hash FROM slot_audio WHERE op = ?1");
        read.bind(1, op);
        CHECK(read.step());
        CHECK_EQ(read.text(0), std::string("after"));
        CHECK_EQ(read.integer(1), 30000);
        CHECK(read.blob(2) == HistoryStore::contentHash(landed));
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM audio.blobs"), 0);
    }

    // --- the lifecycle cannot lie ---
    {
        TempDir tmp;
        std::int64_t finished = 0;
        std::int64_t abandoned = 0;
        std::int64_t failed = 0;
        {
            Ready r(tmp.path);
            finished = r.store.beginOp(r.session, "op-done", "rename", 2000);
            CHECK_EQ(r.store.opStatus(finished), std::string("pending"));
            r.store.finishOp(finished, OpStatus::done, "renamed");
            CHECK_THROWS(r.store.finishOp(finished, OpStatus::failed, ""), "not pending");
            failed = r.store.beginOp(r.session, "op-failed", "trim", 2001);
            r.store.finishOp(failed, OpStatus::failed, "cannot write MEMORY1.RC0");
            abandoned = r.store.beginOp(r.session, "op-abandoned", "clear", 2002);
            // the app stops here: no finishOp
        }
        HistoryStore reopened(tmp.path);
        CHECK_EQ(reopened.opStatus(finished), std::string("done"));
        CHECK_EQ(reopened.opStatus(failed), std::string("failed"));
        CHECK_EQ(reopened.opStatus(abandoned), std::string("interrupted"));
        CHECK_THROWS(reopened.finishOp(abandoned, OpStatus::done, ""), "not pending");
    }

    // --- an operation id names one operation (the #78 contract, held here too) ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        r.store.beginOp(r.session, "2026-09-01T21-35-46-a3f1-0", "normalize", 2000);
        CHECK_THROWS(r.store.beginOp(r.session, "2026-09-01T21-35-46-a3f1-0", "normalize", 2000),
                     "");
        CHECK_THROWS(r.store.beginOp(424242, "op-no-session", "rename", 2000), "");
    }

    // --- cards and sessions ---
    {
        TempDir tmp;
        HistoryStore store(tmp.path);
        const auto a = store.card("RC-5", "BOSS RC-5", 1000);
        CHECK_EQ(store.card("RC-5", "BOSS RC-5", 5000), a); // found again, not duplicated
        CHECK(store.card("RC-500", "BOSS RC-5", 5000) != a); // another model is another card
        CHECK_EQ(count(store.db(), "SELECT last_seen FROM cards WHERE id = " + std::to_string(a)),
                 5000);
        const auto s = store.openSession(a, 6000);
        store.closeSession(s, 7000);
        CHECK_THROWS(store.closeSession(s, 8000), "not open");
    }

    // --- a released take comes back when it is kept again ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string bytes = take(10000, 21);
        const std::string hash = HistoryStore::contentHash(bytes);
        const auto op1 = r.store.beginOp(r.session, "op-r1", "trim", 2000);
        r.store.keepAudio(op1, 3, 1, "003_1.WAV", bytes, 2000);
        {
            // what #74 will do: drop the bytes, mark the metadata
            sqlite::Transaction tx(r.store.db());
            sqlite::Statement drop(r.store.db(), "DELETE FROM audio.blobs WHERE hash = ?1");
            drop.bindBlob(1, hash).run();
            sqlite::Statement mark(r.store.db(), "UPDATE blobs_meta SET released = 3000 WHERE hash = ?1");
            mark.bindBlob(1, hash).run();
            tx.commit();
        }
        CHECK(!r.store.takeBytes(hash).has_value());
        const auto op2 = r.store.beginOp(r.session, "op-r2", "trim", 4000);
        r.store.keepAudio(op2, 3, 1, "003_1.WAV", bytes, 4000);
        CHECK(r.store.takeBytes(hash) == bytes);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs_meta WHERE released IS NULL"), 1);
    }

    // --- a large take round-trips exactly (nothing truncated at the binding) ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string big = take(20u * 1024u * 1024u, 5);
        const auto op = r.store.beginOp(r.session, "op-big", "normalize", 2000);
        r.store.keepAudio(op, 1, 1, "001_1.WAV", big, 2000);
        CHECK(r.store.takeBytes(HistoryStore::contentHash(big)) == big);
    }

    // --- refusals: what the store cannot read correctly, it does not read ---
    {
        // written by a newer LooperCat
        TempDir tmp;
        {
            HistoryStore store(tmp.path);
            store.db().exec("PRAGMA main.user_version = " + std::to_string(schema::kVersion + 1));
        }
        CHECK_THROWS(HistoryStore(tmp.path), "newer LooperCat");
    }
    {
        // an audio store that could never give space back
        TempDir tmp;
        { HistoryStore store(tmp.path); }
        fs::remove(tmp.path / "audio.db");
        {
            auto raw = sqlite::Db::open(tmp.path / "audio.db");
            raw.exec("CREATE TABLE blobs(hash BLOB PRIMARY KEY, bytes BLOB NOT NULL)");
        }
        CHECK_THROWS(HistoryStore(tmp.path), "audio.auto_vacuum");
    }
    {
        // a journal that is new next to an audio store that already holds takes
        TempDir tmp;
        {
            auto raw = sqlite::Db::open(tmp.path / "audio.db");
            raw.exec("CREATE TABLE blobs(hash BLOB PRIMARY KEY, bytes BLOB NOT NULL)");
        }
        CHECK_THROWS(HistoryStore(tmp.path), "not a pair");
        // and the refusal created nothing in the journal it declined to start
        if (fs::exists(tmp.path / "history.db")) {
            auto leftover = sqlite::Db::open(tmp.path / "history.db");
            CHECK_EQ(count(leftover, "SELECT count(*) FROM sqlite_master WHERE type = 'table'"), 0);
        }
    }

    return testkit::summary("history_store_tests");
}
