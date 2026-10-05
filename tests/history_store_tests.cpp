// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The history store (issue #72), attacked from what it promises rather than
// from how it is written:
//
//   - the storage properties it rests on are real, read back from the file:
//     one file, a rollback journal, 16 KB pages, incremental vacuum
//   - a take and the row naming it land together or not at all — a failed row
//     leaves no bytes behind — and bytes never exist without their metadata
//   - a kept take comes back byte-exact, is kept once however often it
//     arrives, and survives reopening
//   - the lifecycle cannot lie: an op the app never finished reads as
//     interrupted, never as done; nothing finishes twice
//   - the store refuses what it cannot read correctly: a newer version, a file
//     that could never return space, another program's database
//   - an operation remembers the slot it was about even when it changed
//     nothing there (#144): a row for the slot, never a state of it
//   - a loudness reading is a fact about bytes (#140): filed under their
//     hash and nothing else, every field back as it went in, replaced by a
//     newer reading of the same bytes, untouched by whatever happens to the
//     slot or the kept copy; a key that is not a hash is refused
//   - every take row names the file's modification time (#141 reads it)
//   - a take sighted on connect is recognised by all four free facts — name,
//     size, stamp, WavLen — or not at all (#141); the newest row is the last
//     word, and so is anything recorded about the slot after it (a clear, a
//     failed write) — but not a subject alone, nor another slot's change;
//     only track 1 is asked; another card's rows are never asked; only a
//     finished operation's row speaks (a first sighting's always does);
//     asking writes nothing
//   - a real read's sighting is the newest word while no operation on the
//     slot came after it, by sequence and never by clock: a different hash
//     under the very same facts wins over the row; an operation that began
//     after the read did — clock set back, read filed late — makes it
//     history, a subject alone or another slot's operation does not;
//     another card's or other facts' reads never count; the same facts read
//     again keep the newest read; a first sighting photographing the slot,
//     and forgetting it, take its sightings, and only its own
//   - heldBefore hands on a valid read's hash with the file's stamp, a hash
//     vouched for by a stampless row without a stamp, and the file's own
//     stamp in every other case

#include "support.hpp"

#include "../app/history/HistoryStore.h"
#include "../app/history/Schema.h"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
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
        session = store.openSession(store.card("test-RC-5", "RC-5", "BOSS RC-5", 1000), 1000);
    }
};

} // namespace

int main()
{
    // --- a fresh directory becomes one verified file ---
    {
        TempDir tmp;
        HistoryStore store(tmp.path / "history");
        CHECK(fs::exists(tmp.path / "history" / "history.db"));
        std::size_t files = 0;
        for (const auto& entry : fs::directory_iterator(tmp.path / "history")) {
            (void) entry;
            ++files;
        }
        CHECK_EQ(files, 1u); // one file: no second store, no WAL, no journal left over
        CHECK_EQ(schema::pragmaText(store.db(), "journal_mode"), std::string("delete"));
        CHECK_EQ(schema::pragmaInteger(store.db(), "auto_vacuum"), 2); // INCREMENTAL
        CHECK_EQ(schema::pragmaInteger(store.db(), "page_size"), 16384);
        CHECK_EQ(schema::pragmaInteger(store.db(), "foreign_keys"), 1);
        CHECK_EQ(schema::pragmaInteger(store.db(), "user_version"), schema::kVersion);
    }

    // --- a directory named outside Latin letters (a player's account name on
    //     Windows): the files land exactly there, under their real name ---
    {
        TempDir tmp;
        const fs::path dir = tmp.path / fs::path(u8"\u03b9\u03c3\u03c4\u03bf\u03c1\u03af\u03b1-\u97f3"); // Greek, and a CJK sign
        { HistoryStore store(dir); }
        CHECK(fs::exists(dir / "history.db"));
        HistoryStore reopened(dir); // and it opens again from the same place
        CHECK_EQ(schema::pragmaInteger(reopened.db(), "main.user_version"), schema::kVersion);
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
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs"), 2);
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
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs"), 0);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs_meta"), 0);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM slot_audio"), 0);

        // The same for a row the schema refuses on its own terms.
        const auto op = r.store.beginOp(r.session, "op-bad-slot", "clear", 2000);
        CHECK_THROWS(r.store.keepAudio(op, 100, 1, "x.wav", take(1000, 1), 2000), "");
        CHECK_THROWS(r.store.keepAudio(op, 0, 1, "x.wav", take(1000, 1), 2000), "");
        CHECK_THROWS(r.store.keepAudio(op, 5, 0, "x.wav", take(1000, 1), 2000), "");
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs"), 0);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs_meta"), 0);
    }

    // --- bytes never exist without their metadata: the schema says so ---
    {
        TempDir tmp;
        HistoryStore store(tmp.path);
        const std::string stray = HistoryStore::contentHash("stray");
        sqlite::Statement put(store.db(), "INSERT INTO blobs(hash, bytes) VALUES (?1, x'00')");
        put.bindBlob(1, stray);
        CHECK_THROWS(put.run(), "FOREIGN KEY");
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM blobs"), 0);
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
        r.store.recordLanded(op, 9, 1, "new.wav", landed, 2345);
        sqlite::Statement read(r.store.db(), "SELECT side, size, hash, modified FROM slot_audio WHERE op = ?1");
        read.bind(1, op);
        CHECK(read.step());
        CHECK_EQ(read.text(0), std::string("after"));
        CHECK_EQ(read.integer(1), 30000);
        CHECK(read.blob(2) == HistoryStore::contentHash(landed));
        CHECK_EQ(read.integer(3), 2345); // the file's stamp, as the recorder read it off the card
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs"), 0);
    }

    // --- every take row names the file's modification time (#141) ---
    //
    // Theory: whatever way a take comes to be what a slot holds — landed by a
    // write, found in place after one, photographed by the first sighting —
    // its row carries the stamp the card's directory entry showed, exactly,
    // in milliseconds since the epoch like every other time here. No such row
    // written by this store is without one: NULL belongs to rows older than
    // the column, and to archived takes, which are leaving their slot.
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string bytes = take(3000, 144);
        const auto op = r.store.beginOp(r.session, "op-stamped", "push", 2000);
        r.store.recordLanded(op, 11, 1, "011_1.WAV", bytes, 1'700'000'000'123);
        r.store.recordPresentAudio(op, 12, 1, "012_1.WAV", 3000, std::nullopt, 1'700'000'000'456);
        r.store.finishOp(op, OpStatus::done, "");
        const auto snapshot = r.store.firstSeen(r.session, "snap", 2100);
        r.store.snapshotSlot(snapshot, 13, "body", { { 1, "013_1.WAV", bytes, 1'700'000'000'789 } }, 2100);
        sqlite::Db& db = r.store.db();
        CHECK_EQ(count(db, "SELECT modified FROM slot_audio WHERE slot = 11"), 1'700'000'000'123);
        CHECK_EQ(count(db, "SELECT modified FROM slot_audio WHERE slot = 12"), 1'700'000'000'456);
        CHECK_EQ(count(db, "SELECT modified FROM slot_audio WHERE slot = 13"), 1'700'000'000'789);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE side = 'after' AND modified IS NULL"), 0);
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
        const auto a = store.card("test-RC-5", "RC-5", "BOSS RC-5", 1000);
        CHECK_EQ(store.card("test-RC-5", "RC-5", "BOSS RC-5", 5000), a); // found again, not duplicated
        CHECK(store.card("test-RC-500", "RC-500", "BOSS RC-5", 5000) != a); // a different marker is a different card
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
            sqlite::Statement drop(r.store.db(), "DELETE FROM blobs WHERE hash = ?1");
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
        // a store that could never give space back: our version stamp, but
        // created without incremental vacuum
        TempDir tmp;
        {
            auto raw = sqlite::Db::open(tmp.path / "history.db");
            for (const char* step : schema::kSteps)
                raw.exec(step);
            raw.exec("PRAGMA user_version = " + std::to_string(schema::kVersion));
        }
        CHECK_THROWS(HistoryStore(tmp.path), "auto_vacuum");
    }
    {
        // another program's database under our file name: tables, no version
        TempDir tmp;
        {
            auto raw = sqlite::Db::open(tmp.path / "history.db");
            raw.exec("CREATE TABLE contacts(name TEXT)");
        }
        CHECK_THROWS(HistoryStore(tmp.path), "not a LooperCat history");
        // and the refusal created nothing in it
        auto leftover = sqlite::Db::open(tmp.path / "history.db");
        CHECK_EQ(count(leftover, "SELECT count(*) FROM sqlite_master WHERE type = 'table'"), 1);
    }

    // --- a slot's timeline: in the order a player reads it ---
    //
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string old = take(20000, 41);
        const std::string fresh = take(30000, 42);

        // what the app did today
        const auto live = r.store.beginOp(r.session, "op-live", "trim", 9000);
        r.store.recordBodies(live, { { 5, "body-before", "body-after" } });
        r.store.keepAudio(live, 5, 1, "005_1.WAV", old, 9000);
        r.store.recordPresentAudio(live, 5, 1, "005_1.WAV",
                                   static_cast<std::int64_t>(fresh.size()),
                                   HistoryStore::contentHash(fresh), 9000);
        r.store.finishOp(live, OpStatus::done, "trimmed");

        // A later operation after the system clock moved backwards.
        const auto cleared = r.store.beginOp(r.session, "op-clear", "clear", 1000);
        r.store.keepAudio(cleared, 5, 1, "005_1.WAV", take(15000, 43), 1000);
        r.store.finishOp(cleared, OpStatus::done, "");

        const auto rows = r.store.slotTimeline(5);
        CHECK_EQ(rows.size(), 2u);
        if (rows.size() == 2u) {
            CHECK_EQ(rows[0].kind, std::string("clear")); // older by the clock, later by seq
            CHECK_EQ(rows[0].actor, std::string("app"));
            CHECK(rows[0].op > rows[1].op); // exactly the trap: insertion order would lie
            CHECK_EQ(rows[1].kind, std::string("trim"));
            CHECK_EQ(rows[1].note, std::string("trimmed"));
            CHECK(rows[1].beforeBody.has_value() && *rows[1].beforeBody == "body-before");
            CHECK(rows[1].afterBody.has_value() && *rows[1].afterBody == "body-after");
            // the live row offers the take its state holds...
            CHECK(rows[1].takeHash == HistoryStore::contentHash(fresh));
            CHECK(!rows[1].takeKept); // ...whose bytes are on the card, not in the store
            // ...and the clear row offers the one it kept, which can be played
            CHECK(rows[0].takeHash == HistoryStore::contentHash(take(15000, 43)));
            CHECK(rows[0].takeKept);
        }
        // and nothing from another slot leaks in
        CHECK(r.store.slotTimeline(6).empty());
    }

    {
        // A swap row names the slot it exchanged with, from either side.
        TempDir tmp;
        Ready r(tmp.path);
        const auto op = r.store.beginOp(r.session, "op-swap", "swap", 2000);
        r.store.recordBodies(op, { { 3, "a-before", "a-after" }, { 7, "b-before", "b-after" } });
        r.store.finishOp(op, OpStatus::done, "");
        const auto three = r.store.slotTimeline(3);
        const auto seven = r.store.slotTimeline(7);
        CHECK_EQ(three.size(), 1u);
        CHECK_EQ(seven.size(), 1u);
        if (!three.empty() && !seven.empty()) {
            CHECK(three.front().swappedWith == 7);
            CHECK(seven.front().swappedWith == 3);
        }
    }

    {
        // A take whose bytes were released reads as a take that cannot be
        // played — the row stays, the offer does not.
        TempDir tmp;
        Ready r(tmp.path);
        const std::string bytes = take(12000, 44);
        const auto op = r.store.beginOp(r.session, "op-gone", "clear", 2000);
        r.store.keepAudio(op, 9, 1, "009_1.WAV", bytes, 2000);
        r.store.finishOp(op, OpStatus::done, "");
        CHECK(r.store.slotTimeline(9).front().takeKept);
        sqlite::Statement drop(r.store.db(), "DELETE FROM blobs WHERE hash = ?1");
        drop.bindBlob(1, HistoryStore::contentHash(bytes)).run();
        const auto after = r.store.slotTimeline(9);
        CHECK_EQ(after.size(), 1u);
        CHECK(after.front().takeHash.has_value()); // we still know which take it was
        CHECK(!after.front().takeKept);            // and that its bytes are gone
    }

    // --- what an operation was about, apart from what it changed (#144) ---
    {
        // A normalize that found slot 7 at target: no bodies, no take, and
        // still a row that names slot 7 — in the card's timeline, and in the
        // slot's own, where it is never the state the slot is in.
        TempDir tmp;
        Ready r(tmp.path);
        const std::string bytes = take(8000, 71);
        const auto push = r.store.beginOp(r.session, "op-push", "push", 2000);
        r.store.recordBodies(push, { { 7, "empty", "loaded" } });
        r.store.recordLanded(push, 7, 1, "007_1.WAV", bytes, 2000);
        r.store.finishOp(push, OpStatus::done, "");
        const std::string why = "already at -18.0 LUFS (measured -18.1), nothing to do";
        const auto nothing = r.store.beginOp(r.session, "op-nothing", "normalize", 3000);
        r.store.recordSubject(nothing, 7);
        r.store.finishOp(nothing, OpStatus::done, why);
        CHECK(r.store.subjects(nothing) == std::vector<int> { 7 });
        CHECK(r.store.subjects(push).empty());
        CHECK(r.store.touchedSlots(nothing).empty()); // a subject is not a touch
        CHECK(!r.store.hasAfterAudio(nothing, 7));

        const auto seven = r.store.slotTimeline(7);
        CHECK_EQ(seven.size(), 2u);
        if (seven.size() == 2u) {
            CHECK_EQ(seven[0].op, push);
            CHECK(!seven[0].subjectOnly);
            CHECK_EQ(seven[1].op, nothing);
            CHECK(seven[1].subjectOnly);
            CHECK(!seven[1].beforeBody && !seven[1].afterBody);
            CHECK(seven[1].takeName.empty() && !seven[1].takeHash && !seven[1].takeKept);
            CHECK_EQ(seven[1].takeCount, 0);
            CHECK_EQ(seven[1].note, why);
            CHECK_EQ(seven[1].status, std::string("done"));
        }
        CHECK(r.store.slotTimeline(8).empty());

        const auto card = r.store.cardTimeline();
        CHECK_EQ(card.size(), 2u);
        if (card.size() == 2u) {
            CHECK(card[0].subjects.empty());
            CHECK(card[1].slots.empty());
            CHECK(card[1].subjects == std::vector<int> { 7 });
            CHECK_EQ(card[1].note, why);
        }
        // the push is still the state slot 7 is in: the normalize recorded none
        CHECK(card.size() == 2u && card[0].slots.size() == 1u && card[0].slots[0].newest);
        // and another card sees nothing of it
        r.store.selectCard(r.store.card("other", "RC-5", "Other", 1));
        CHECK(r.store.slotTimeline(7).empty());
        CHECK(r.store.cardTimeline().empty());
    }
    {
        // A subject is about one operation the store has, one slot of the
        // pedal's, once: refused otherwise, and a refusal writes nothing. A
        // swap about the two slots it then changes has each of them once.
        TempDir tmp;
        Ready r(tmp.path);
        const auto op = r.store.beginOp(r.session, "op-swap", "swap", 2000);
        CHECK_THROWS(r.store.recordSubject(9999, 12), "no operation 9999");
        CHECK_THROWS(r.store.recordSubject(op, 0), "1..99");
        CHECK_THROWS(r.store.recordSubject(op, 100), "1..99");
        CHECK_THROWS(r.store.recordSubject(op, -7), "1..99");
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM op_subjects"), 0);
        r.store.recordSubject(op, 43);
        r.store.recordSubject(op, 12);
        CHECK_THROWS(r.store.recordSubject(op, 12), "already a subject");
        CHECK_THROWS(r.store.recordSubject(op, 43), "already a subject");
        CHECK(r.store.subjects(op) == (std::vector<int> { 12, 43 })); // ascending, each once
        r.store.recordBodies(op, { { 12, "a-before", "a-after" }, { 43, "b-before", "b-after" } });
        r.store.finishOp(op, OpStatus::done, "");
        const auto card = r.store.cardTimeline();
        CHECK_EQ(card.size(), 1u);
        if (!card.empty()) {
            CHECK_EQ(card.front().slots.size(), 2u);
            CHECK(card.front().subjects == (std::vector<int> { 12, 43 }));
        }
        const auto twelve = r.store.slotTimeline(12);
        CHECK_EQ(twelve.size(), 1u); // about it and changed it: one row, not two
        CHECK(twelve.size() == 1u && !twelve.front().subjectOnly);
        CHECK(twelve.size() == 1u && twelve.front().swappedWith == 43);
        // the table refuses on its own what the method refuses
        CHECK_THROWS(r.store.db().exec("INSERT INTO op_subjects VALUES (9999, 1)"), "FOREIGN KEY");
    }
    {
        // A subject is named while an operation runs, and only by one that
        // can be about a slot: the first sighting has its rows, maintenance
        // is about the history itself, and a closed operation is closed.
        TempDir tmp;
        Ready r(tmp.path);
        const auto snapshot = r.store.firstSeen(r.session, "snap", 1500);
        CHECK_THROWS(r.store.recordSubject(snapshot, 7), "about no slot");
        const auto forget = r.store.beginOp(r.session, "op-forget", "forget-history", 2000);
        CHECK_THROWS(r.store.recordSubject(forget, 7), "about no slot");
        const auto done = r.store.beginOp(r.session, "op-done", "normalize", 2100);
        r.store.finishOp(done, OpStatus::done, "");
        CHECK_THROWS(r.store.recordSubject(done, 7), "not pending");
        const auto failed = r.store.beginOp(r.session, "op-failed", "normalize", 2200);
        r.store.finishOp(failed, OpStatus::failed, "cannot read 007_1.WAV");
        CHECK_THROWS(r.store.recordSubject(failed, 7), "not pending");
        const auto cut = r.store.beginOp(r.session, "op-cut", "normalize", 2300);
        r.store.finishOp(cut, OpStatus::interrupted, "unplugged");
        CHECK_THROWS(r.store.recordSubject(cut, 7), "not pending");
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM op_subjects"), 0);
        // while it runs, it may: and the subject stays once it has closed
        const auto running = r.store.beginOp(r.session, "op-running", "normalize", 2400);
        r.store.recordSubject(running, 7);
        r.store.finishOp(running, OpStatus::failed, "cannot read 007_1.WAV");
        CHECK(r.store.subjects(running) == std::vector<int> { 7 });
        CHECK_EQ(r.store.slotTimeline(7).size(), 1u);
        CHECK(r.store.slotTimeline(7).size() == 1u && r.store.slotTimeline(7).front().status == "failed");
    }

    // --- a loudness reading is a fact about bytes (#140) ---
    //
    // Theory: a reading is of bytes and of nothing else. It is filed under
    // their hash whether or not any row names them, every field comes back as
    // it went in — an unmeasurable loudness as absent, never as a number; a
    // peak of -inf as -inf — a second reading of the same bytes replaces the
    // first, and a key that is not a hash is refused rather than filed under.
    {
        TempDir tmp;
        HistoryStore store(tmp.path);
        const std::string bytes = take(5000, 140);
        const std::string hash = HistoryStore::contentHash(bytes);
        CHECK(!store.readingFor(hash).has_value()); // never measured: absent, not zeros

        // no slot_audio row, no blobs_meta row knows these bytes: filed regardless
        const wav::LoudnessReading loud { -22.75, 0.73f, -2.5, 0 };
        store.recordReading(hash, loud, 5000);
        const auto back = store.readingFor(hash);
        CHECK(back.has_value());
        if (back) {
            CHECK(back->reading.integratedLufs.has_value());
            CHECK(back->reading.integratedLufs.has_value()
                  && std::bit_cast<std::uint64_t>(*back->reading.integratedLufs)
                         == std::bit_cast<std::uint64_t>(-22.75));
            CHECK_EQ(std::bit_cast<std::uint32_t>(back->reading.samplePeak),
                     std::bit_cast<std::uint32_t>(0.73f));
            CHECK_EQ(std::bit_cast<std::uint64_t>(back->reading.truePeakDb),
                     std::bit_cast<std::uint64_t>(-2.5));
            CHECK_EQ(back->reading.wildSamples, 0);
            CHECK_EQ(back->measuredMs, 5000);
        }
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM blobs_meta"), 0);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM slot_audio"), 0);

        // the same bytes measured again: one row, the newer numbers, the newer date
        store.recordReading(hash, { -22.7, 0.75f, -2.4, 0 }, 6000);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM loudness_readings"), 1);
        const auto newer = store.readingFor(hash);
        CHECK(newer.has_value() && newer->measuredMs == 6000);
        CHECK(newer.has_value() && newer->reading.integratedLufs.has_value()
              && std::abs(*newer->reading.integratedLufs - (-22.7)) <= 1.0e-12);
        CHECK_EQ(std::bit_cast<std::uint32_t>(newer ? newer->reading.samplePeak : 0.0f),
                 std::bit_cast<std::uint32_t>(0.75f));

        // digital silence: no loudness and a peak of -inf come back exactly so
        const std::string quiet = HistoryStore::contentHash(take(5000, 141));
        store.recordReading(quiet, { std::nullopt, 0.0f, -std::numeric_limits<double>::infinity(), 0 }, 7000);
        const auto silence = store.readingFor(quiet);
        CHECK(silence.has_value());
        CHECK(silence.has_value() && !silence->reading.integratedLufs.has_value());
        CHECK(silence.has_value() && std::isinf(silence->reading.truePeakDb) && silence->reading.truePeakDb < 0.0);
        CHECK_EQ(std::bit_cast<std::uint32_t>(silence ? silence->reading.samplePeak : 1.0f),
                 std::bit_cast<std::uint32_t>(0.0f));
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM loudness_readings WHERE integrated_lufs = 0"), 0);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM loudness_readings WHERE integrated_lufs IS NULL"), 1);

        // damaged bytes: the count is the fact, the numbers beside it are kept as read
        const std::string junk = HistoryStore::contentHash(take(5000, 142));
        store.recordReading(junk, { 767.0, 2.4e38f, 400.0, 1234 }, 8000);
        const auto damaged = store.readingFor(junk);
        CHECK(damaged.has_value() && damaged->reading.wildSamples == 1234);
        CHECK_EQ(std::bit_cast<std::uint32_t>(damaged ? damaged->reading.samplePeak : 0.0f),
                 std::bit_cast<std::uint32_t>(2.4e38f));

        // a key that is not a hash is refused, on the way in and on the way out
        CHECK_THROWS(store.recordReading(hash.substr(0, 31), loud, 9000), "32 bytes");
        CHECK_THROWS(store.recordReading(hash + "x", loud, 9000), "32 bytes");
        CHECK_THROWS(store.recordReading("", loud, 9000), "32 bytes");
        CHECK_THROWS(store.readingFor(hash.substr(0, 31)), "32 bytes");
        CHECK_THROWS(store.readingFor(""), "32 bytes");
        // a value that is not a number cannot pass for "unmeasurable", and a
        // count cannot be negative: refused, with nothing filed
        const double nan = std::numeric_limits<double>::quiet_NaN();
        CHECK_THROWS(store.recordReading(HistoryStore::contentHash("nan-lufs"), { nan, 0.5f, -3.0, 0 }, 1),
                     "not a number");
        CHECK_THROWS(store.recordReading(HistoryStore::contentHash("nan-peak"),
                                         { -20.0, std::numeric_limits<float>::quiet_NaN(), -3.0, 0 }, 1),
                     "not a number");
        CHECK_THROWS(store.recordReading(HistoryStore::contentHash("nan-tp"), { -20.0, 0.5f, nan, 0 }, 1),
                     "not a number");
        CHECK_THROWS(store.recordReading(HistoryStore::contentHash("negative"), { -20.0, 0.5f, -3.0, -1 }, 1),
                     "negative");
        // a sample peak is a magnitude: below zero it is not a peak
        CHECK_THROWS(store.recordReading(HistoryStore::contentHash("neg-peak"), { -20.0, -0.5f, -3.0, 0 }, 1),
                     "negative");
        CHECK_THROWS(store.recordReading(HistoryStore::contentHash("neg-peak"), { -20.0, -1.0e-30f, -3.0, 0 }, 1),
                     "negative");
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM loudness_readings"), 3);
        // the table refuses on its own what the method refuses
        CHECK_THROWS(store.db().exec("INSERT INTO loudness_readings VALUES (x'00', NULL, 0.0, 0.0, 0, 1)"), "CHECK");
        CHECK_THROWS(store.db().exec("INSERT INTO loudness_readings VALUES (x'" + std::string(64, '0')
                                     + "', NULL, -0.5, 0.0, 0, 1)"),
                     "CHECK");
        CHECK_THROWS(store.db().exec("INSERT INTO loudness_readings VALUES (x'" + std::string(64, '0')
                                     + "', NULL, 0.0, 0.0, -1, 1)"),
                     "CHECK");
        CHECK_THROWS(store.db().exec("INSERT INTO loudness_readings VALUES (x'" + std::string(64, '0')
                                     + "', NULL, NULL, 0.0, 0, 1)"),
                     "NOT NULL");
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM loudness_readings"), 3);
    }
    {
        // A reading outlives everything that can happen to the slot and to
        // the bytes: the take's bytes released, the slot's history forgotten,
        // the store reopened. A fact about bytes is not about a slot, and it
        // costs nothing to keep.
        TempDir tmp;
        const std::string bytes = take(20000, 143);
        const std::string hash = HistoryStore::contentHash(bytes);
        {
            Ready r(tmp.path);
            const auto cleared = r.store.beginOp(r.session, "op-cleared", "clear", 2000);
            r.store.keepAudio(cleared, 8, 1, "008_1.WAV", bytes, 2000);
            r.store.finishOp(cleared, OpStatus::done, "");
            r.store.recordReading(hash, { -19.5, 0.9f, -1.2, 0 }, 2500);
            // a later operation elsewhere, so the clear is no longer what Undo would put back
            const auto renamed = r.store.beginOp(r.session, "op-renamed", "rename", 3000);
            r.store.recordBodies(renamed, { { 9, "before", "after" } });
            r.store.finishOp(renamed, OpStatus::done, "");
            // the bytes go: the reading stays
            CHECK(r.store.releaseBlobs({ hash }, r.store.offeredTargets(), 3500) > 0);
            CHECK(!r.store.takeBytes(hash).has_value());
            CHECK(r.store.readingFor(hash).has_value());
            // the slot's history goes: the reading stays
            r.store.forgetSlot(*r.store.selectedCard(), 8, 4000, true);
            CHECK(r.store.slotTimeline(8).empty());
            CHECK(r.store.readingFor(hash).has_value());
        }
        HistoryStore reopened(tmp.path);
        const auto kept = reopened.readingFor(hash);
        CHECK(kept.has_value());
        CHECK(kept.has_value() && kept->measuredMs == 2500);
        CHECK(kept.has_value() && kept->reading.integratedLufs.has_value()
              && std::abs(*kept->reading.integratedLufs - (-19.5)) <= 1.0e-12);
    }

    // --- a hash is carried to a new row only for the very file an earlier row saw (#141) ---
    //
    // Theory: the slot's newest row for a file name is the slot's last word
    // about that file. It vouches for the file in front of the recorder only
    // when it carries a hash and its size and stamp are the file's now. A
    // newer row without a hash means the file changed while the app was away,
    // and no older row may speak over it. A row from before the store kept
    // stamps is held to the rule it was written under, name and size — the
    // one allowance, so a migrated history's slots stay restorable.
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string a = take(3000, 151);
        const std::string hashA = HistoryStore::contentHash(a);
        const auto push = r.store.beginOp(r.session, "op-push", "push", 2000);
        r.store.recordLanded(push, 5, 1, "005_1.WAV", a, 1000);
        r.store.recordLanded(push, 7, 1, "007_1.WAV", take(3000, 152), 1000);
        r.store.finishOp(push, OpStatus::done, "");
        const auto next = r.store.beginOp(r.session, "op-next", "rename", 3000);
        // the same file: name, size and stamp as the row saw them
        CHECK(r.store.hashHeldBefore(next, 5, "005_1.WAV", 3000, 1000) == hashA);
        // another file under the same name: a different stamp, or a different size
        CHECK(!r.store.hashHeldBefore(next, 5, "005_1.WAV", 3000, 2000).has_value());
        CHECK(!r.store.hashHeldBefore(next, 5, "005_1.WAV", 3001, 1000).has_value());
        // another slot, another name: nothing to carry
        CHECK(!r.store.hashHeldBefore(next, 6, "005_1.WAV", 3000, 1000).has_value());
        CHECK(!r.store.hashHeldBefore(next, 5, "006_1.WAV", 3000, 1000).has_value());
        // only rows before the operation asking
        CHECK(!r.store.hashHeldBefore(push, 5, "005_1.WAV", 3000, 1000).has_value());
        // the newest row has no hash: the last word, and an older match is not consulted
        r.store.recordPresentAudio(next, 5, 1, "005_1.WAV", 3000, std::nullopt, 2000);
        r.store.finishOp(next, OpStatus::done, "");
        const auto later = r.store.beginOp(r.session, "op-later", "rename", 4000);
        CHECK(!r.store.hashHeldBefore(later, 5, "005_1.WAV", 3000, 2000).has_value());
        CHECK(!r.store.hashHeldBefore(later, 5, "005_1.WAV", 3000, 1000).has_value()); // not even for the old stamp
        // a row from before the store kept stamps is asked only what it knows:
        // name and size carry its hash, whatever stamp the file has now; a
        // different size is still another file
        sqlite::Statement unstamp(r.store.db(), "UPDATE slot_audio SET modified = NULL WHERE slot = 7");
        unstamp.run();
        CHECK(r.store.hashHeldBefore(later, 7, "007_1.WAV", 3000, 1000) == HistoryStore::contentHash(take(3000, 152)));
        CHECK(r.store.hashHeldBefore(later, 7, "007_1.WAV", 3000, 987654321) == HistoryStore::contentHash(take(3000, 152)));
        CHECK(!r.store.hashHeldBefore(later, 7, "007_1.WAV", 2999, 1000).has_value());
        // and a stamped row stays held to its stamp: the allowance is for old rows only
        CHECK(!r.store.hashHeldBefore(later, 5, "005_1.WAV", 3000, 1000).has_value());
        // another card's rows are another card's
        r.store.finishOp(later, OpStatus::done, "");
        const auto other = r.store.openSession(r.store.card("other", "RC-5", "Other", 5000), 5000);
        const auto elsewhere = r.store.beginOp(other, "op-elsewhere", "rename", 6000);
        r.store.recordLanded(elsewhere, 9, 1, "009_1.WAV", a, 1000);
        r.store.finishOp(elsewhere, OpStatus::done, "");
        r.store.selectCard(1);
        const auto back = r.store.beginOp(r.session, "op-back", "rename", 7000);
        CHECK(!r.store.hashHeldBefore(back, 9, "009_1.WAV", 3000, 1000).has_value());
    }

    // --- what a connect can tell from a directory entry (#141) ---
    //
    // Theory: a take is recognised without reading it only when every free
    // fact agrees — the entry's name, size and stamp against the slot's
    // newest row for that name, and the config's WavLen against the body on
    // record with that row. The newest row is the slot's last word: a row
    // that cannot match (no hash, another size or stamp, no stamp at all)
    // answers nothing, and no older row speaks instead. Stamps are compared
    // as the integers they are. Another card's rows are never consulted. The
    // lookup writes nothing.
    {
        TempDir tmp;
        Ready r(tmp.path);
        const auto bodyWith = [](long long frames) {
            std::string body = rc0::setSectionField(testkit::syntheticSlotBody("Loop"),
                                                    rc0::trackSectionName(1), "WavStat", 1);
            return rc0::setSectionField(body, rc0::trackSectionName(1), "WavLen", frames);
        };
        const std::string a = take(3000, 161);
        const std::string hashA = HistoryStore::contentHash(a);
        constexpr std::int64_t kStamp = 1'700'000'000'000; // ms, like every time in the store
        const auto push = r.store.beginOp(r.session, "op-push", "push", 2000);
        r.store.recordBodies(push, { { 5, testkit::syntheticSlotBody("Memory 05"), bodyWith(132300) } });
        r.store.recordLanded(push, 5, 1, "005_1.WAV", a, kStamp);
        r.store.finishOp(push, OpStatus::done, "");
        const std::int64_t card = *r.store.selectedCard();
        using Sighting = HistoryStore::TakeSighting;
        // all four facts agree: the take is recognised
        CHECK(r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 3000, kStamp, 132300 }) == hashA);
        // any one of them off: nothing — a name, a size, a stamp, a WavLen
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_2.WAV", 3000, kStamp, 132300 }).has_value());
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 2999, kStamp, 132300 }).has_value());
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 3001, kStamp, 132300 }).has_value());
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 3000, kStamp + 2000, 132300 }).has_value());
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 3000, kStamp + 1, 132300 }).has_value()); // no tolerance
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 3000, kStamp - 1, 132300 }).has_value());
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 3000, kStamp, 132301 }).has_value());
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 3000, kStamp, 0 }).has_value());
        // another slot: nothing, even for the same facts
        CHECK(!r.store.hashOfSighting(card, 6, Sighting { "005_1.WAV", 3000, kStamp, 132300 }).has_value());
        // a rewrite of the take without a body of its own (a normalize): the
        // newest row wins, the body on record with it is the one before
        const std::string b = take(3000, 162);
        const std::string hashB = HistoryStore::contentHash(b);
        const auto normalize = r.store.beginOp(r.session, "op-normalize", "normalize", 3000);
        r.store.recordLanded(normalize, 5, 1, "005_1.WAV", b, kStamp + 4000);
        r.store.finishOp(normalize, OpStatus::done, "");
        CHECK(r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 3000, kStamp + 4000, 132300 }) == hashB);
        // the file as the older row saw it: the newest row is the last word,
        // and the older one is not consulted
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 3000, kStamp, 132300 }).has_value());
        // a later body with another WavLen, and a newest row that names the
        // file without a hash (bytes the store did not know): the slot's
        // last word cannot vouch, so nothing — not for the old facts either
        const auto trim = r.store.beginOp(r.session, "op-trim", "trim", 4000);
        r.store.recordBodies(trim, { { 5, bodyWith(132300), bodyWith(88200) } });
        r.store.recordPresentAudio(trim, 5, 1, "005_1.WAV", 2000, std::nullopt, kStamp + 6000);
        r.store.finishOp(trim, OpStatus::done, "");
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 2000, kStamp + 6000, 88200 }).has_value());
        CHECK(!r.store.hashOfSighting(card, 5, Sighting { "005_1.WAV", 3000, kStamp + 4000, 132300 }).has_value());
        // a row without a stamp (older than store version 9) gets no
        // allowance here: three facts are not four
        const auto old = r.store.beginOp(r.session, "op-old", "push", 5000);
        r.store.recordBodies(old, { { 7, testkit::syntheticSlotBody("Memory 07"), bodyWith(44100) } });
        r.store.recordLanded(old, 7, 1, "007_1.WAV", a, kStamp);
        r.store.finishOp(old, OpStatus::done, "");
        CHECK(r.store.hashOfSighting(card, 7, Sighting { "007_1.WAV", 3000, kStamp, 44100 }) == hashA);
        sqlite::Statement unstamp(r.store.db(), "UPDATE slot_audio SET modified = NULL WHERE slot = 7");
        unstamp.run();
        CHECK(!r.store.hashOfSighting(card, 7, Sighting { "007_1.WAV", 3000, kStamp, 44100 }).has_value());
        CHECK(!r.store.hashOfSighting(card, 7, Sighting { "007_1.WAV", 3000, 0, 44100 }).has_value());
        // a take row with no body on record for the slot: nothing to hold
        // WavLen against, so nothing
        const auto bodiless = r.store.beginOp(r.session, "op-bodiless", "push", 6000);
        r.store.recordLanded(bodiless, 8, 1, "008_1.WAV", a, kStamp);
        r.store.finishOp(bodiless, OpStatus::done, "");
        CHECK(!r.store.hashOfSighting(card, 8, Sighting { "008_1.WAV", 3000, kStamp, 132300 }).has_value());
        // another card: its rows are its own, in both directions
        const auto otherCard = r.store.card("other", "RC-5", "Other", 7000);
        const auto other = r.store.openSession(otherCard, 7000);
        const auto elsewhere = r.store.beginOp(other, "op-elsewhere", "push", 8000);
        r.store.recordBodies(elsewhere, { { 9, testkit::syntheticSlotBody("Memory 09"), bodyWith(132300) } });
        r.store.recordLanded(elsewhere, 9, 1, "009_1.WAV", a, kStamp);
        r.store.finishOp(elsewhere, OpStatus::done, "");
        CHECK(r.store.hashOfSighting(otherCard, 9, Sighting { "009_1.WAV", 3000, kStamp, 132300 }) == hashA);
        CHECK(!r.store.hashOfSighting(card, 9, Sighting { "009_1.WAV", 3000, kStamp, 132300 }).has_value());
        CHECK(!r.store.hashOfSighting(otherCard, 7, Sighting { "007_1.WAV", 3000, kStamp, 44100 }).has_value());
        // the card a marker names, looked up and not touched: last_seen stays
        CHECK(r.store.cardFor("other") == otherCard);
        CHECK(r.store.cardFor("test-RC-5") == card);
        CHECK(!r.store.cardFor("never-met").has_value());
        CHECK_EQ(count(r.store.db(), "SELECT last_seen FROM cards WHERE marker_id = 'other'"), 7000);

        // Back on this card, in its own session.
        r.store.selectCard(card);
        const auto landedOn = [&](int slot, const std::string& name, const std::string& bytes,
                                  const std::string& opId, std::int64_t at, bool withBody) {
            const auto op = r.store.beginOp(r.session, opId, "push", at);
            if (withBody)
                r.store.recordBodies(op, { { slot, testkit::syntheticSlotBody("Memory"), bodyWith(132300) } });
            r.store.recordLanded(op, slot, 1, name, bytes, kStamp);
            r.store.finishOp(op, OpStatus::done, "");
            return op;
        };
        // a clear after the row: the slot was seen emptied, and the very
        // same facts met there again are not the take the row saw — the
        // pedal records with a clock that does not keep the date
        landedOn(12, "012_1.WAV", a, "op-12", 9000, true);
        CHECK(r.store.hashOfSighting(card, 12, Sighting { "012_1.WAV", 3000, kStamp, 132300 }) == hashA);
        const auto clear = r.store.beginOp(r.session, "op-clear-12", "clear", 9100);
        r.store.recordBodies(clear, { { 12, bodyWith(132300), testkit::syntheticSlotBody("Memory 12") } });
        r.store.keepAudio(clear, 12, 1, "012_1.WAV", a, 9100);
        r.store.finishOp(clear, OpStatus::done, "");
        CHECK(!r.store.hashOfSighting(card, 12, Sighting { "012_1.WAV", 3000, kStamp, 132300 }).has_value());
        // a write that failed after the row: the slot is where the failure
        // left it, and the row is not its word any more either
        landedOn(13, "013_1.WAV", a, "op-13", 9200, true);
        const auto failed = r.store.beginOp(r.session, "op-failed-13", "trim", 9300);
        r.store.recordBodies(failed, { { 13, bodyWith(132300), bodyWith(88200) } });
        r.store.finishOp(failed, OpStatus::failed, "the card went away");
        CHECK(!r.store.hashOfSighting(card, 13, Sighting { "013_1.WAV", 3000, kStamp, 132300 }).has_value());
        // what does not unsay it: an operation only about the slot that
        // changed nothing (#144), and another slot's change
        landedOn(14, "014_1.WAV", a, "op-14", 9400, true);
        const auto aboutOnly = r.store.beginOp(r.session, "op-about-14", "normalize", 9500);
        r.store.recordSubject(aboutOnly, 14);
        r.store.finishOp(aboutOnly, OpStatus::done, "already at -18.0 LUFS");
        landedOn(15, "015_1.WAV", b, "op-15", 9600, true);
        CHECK(r.store.hashOfSighting(card, 14, Sighting { "014_1.WAV", 3000, kStamp, 132300 }) == hashA);
        // only track 1 is asked: the LUFS column measures track 1's take
        const auto second = r.store.beginOp(r.session, "op-16", "push", 9700);
        r.store.recordBodies(second, { { 16, testkit::syntheticSlotBody("Memory 16"), bodyWith(132300) } });
        r.store.recordPresentAudio(second, 16, 2, "016_1.WAV", 3000, hashA, kStamp);
        r.store.finishOp(second, OpStatus::done, "");
        CHECK(!r.store.hashOfSighting(card, 16, Sighting { "016_1.WAV", 3000, kStamp, 132300 }).has_value());
        // two rows agreeing on every fact and not on the bytes (the pedal's
        // clock again): the newest one speaks
        landedOn(17, "017_1.WAV", a, "op-17a", 9800, true);
        landedOn(17, "017_1.WAV", b, "op-17b", 9900, false);
        CHECK(r.store.hashOfSighting(card, 17, Sighting { "017_1.WAV", 3000, kStamp, 132300 }) == hashB);
        // a write that failed after its take landed: the row is the slot's
        // last word and still says nothing — the operation never finished
        {
            const auto failedPush = r.store.beginOp(r.session, "op-18", "push", 10000);
            r.store.recordLanded(failedPush, 18, 1, "018_1.WAV", a, kStamp);
            r.store.recordBodies(failedPush, { { 18, testkit::syntheticSlotBody("Memory 18"), bodyWith(132300) } });
            r.store.finishOp(failedPush, OpStatus::failed, "the pair write failed");
            CHECK(!r.store.hashOfSighting(card, 18, Sighting { "018_1.WAV", 3000, kStamp, 132300 }).has_value());
        }
        // a first sighting commits slot by slot: its rows speak while it is
        // still running, and after it was interrupted
        {
            const auto baseline = r.store.firstSeen(r.session, "op-first", 10100);
            r.store.snapshotSlot(baseline, 19, bodyWith(132300), { { 1, "019_1.WAV", a, kStamp } }, 10100);
            CHECK_EQ(r.store.opStatus(baseline), std::string("pending"));
            CHECK(r.store.hashOfSighting(card, 19, Sighting { "019_1.WAV", 3000, kStamp, 132300 }) == hashA);
            r.store.finishOp(baseline, OpStatus::interrupted, "the card went away");
            CHECK(r.store.hashOfSighting(card, 19, Sighting { "019_1.WAV", 3000, kStamp, 132300 }) == hashA);
        }

        // --- what a real read saw (take_sightings) ---
        //
        // Theory: a read hashed the bytes under these facts, so a read that
        // no operation on the slot came after is the newest word about the
        // file and outranks a row that only named it. "After" is the
        // operations' sequence, never a clock: an operation recording a body
        // or a take in the slot with a higher sequence than the read began
        // after makes the read history; a subject alone, or another slot's
        // operation, does not. A read of other facts, or on another card,
        // says nothing about these. (Review S8, S9, S10, at the store.)
        const std::string c = take(3000, 163);
        const std::string hashC = HistoryStore::contentHash(c);
        const std::int64_t op20 = landedOn(20, "020_1.WAV", a, "op-20", 11000, true);
        const Sighting twenty { "020_1.WAV", 3000, kStamp, 132300 };
        const std::string sinceOf20 = "SELECT since_op FROM take_sightings WHERE card = "
                                    + std::to_string(card) + " AND slot = 20";
        CHECK(r.store.hashOfSighting(card, 20, twenty) == hashA);
        // the pedal recorded the slot again under the same facts; a read saw C,
        // placed after the slot's newest operation by sequence
        CHECK_EQ(r.store.newestOp(), op20);
        r.store.recordSighting(card, 20, "020_1.WAV", 3000, kStamp, hashC, r.store.newestOp(), 11100);
        CHECK_EQ(count(r.store.db(), sinceOf20), op20);
        CHECK(r.store.hashOfSighting(card, 20, twenty) == hashC);
        // the read needs no WavLen of its own: the hash is of the bytes
        CHECK(r.store.hashOfSighting(card, 20, Sighting { "020_1.WAV", 3000, kStamp, 1 }) == hashC);
        // read again under the same facts: the newest read is kept, one row
        r.store.recordSighting(card, 20, "020_1.WAV", 3000, kStamp, hashB, r.store.newestOp(), 11200);
        CHECK(r.store.hashOfSighting(card, 20, twenty) == hashB);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM take_sightings WHERE slot = 20"), 1);
        // S9 by sequence: the read stands on the slot's last operation, and
        // neither a subject alone nor another slot's operation unsays it
        const auto about20 = r.store.beginOp(r.session, "op-about-20", "normalize", 11250);
        r.store.recordSubject(about20, 20);
        r.store.finishOp(about20, OpStatus::done, "already at -18.0 LUFS");
        landedOn(23, "023_1.WAV", a, "op-23", 11260, true);
        CHECK(r.store.hashOfSighting(card, 20, twenty) == hashB);
        // a read of other facts says nothing about these
        r.store.recordSighting(card, 21, "021_1.WAV", 3000, kStamp + 2000, hashC, r.store.newestOp(), 11300);
        CHECK(!r.store.hashOfSighting(card, 21, Sighting { "021_1.WAV", 3000, kStamp, 132300 }).has_value());
        CHECK(!r.store.hashOfSighting(card, 21, Sighting { "021_1.WAV", 2999, kStamp + 2000, 132300 }).has_value());
        CHECK(!r.store.hashOfSighting(card, 21, Sighting { "021_2.WAV", 3000, kStamp + 2000, 132300 }).has_value());
        // ...and a slot nothing but a read ever saw answers by the read alone,
        // which came after no operation of its own
        CHECK(r.store.hashOfSighting(card, 21, Sighting { "021_1.WAV", 3000, kStamp + 2000, 132300 }) == hashC);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM take_sightings WHERE slot = 21 AND since_op IS NULL"), 1);
        // S10: an operation after the read, its clock set back an hour before
        // the read's: the sequence says after, so the read is history
        landedOn(20, "020_1.WAV", a, "op-20-again", 11200 - 3'600'000, true);
        CHECK(r.store.hashOfSighting(card, 20, twenty) == hashA);
        // S8: a read begun before an operation and filed after it is history;
        // the same read begun after that operation stands
        const std::int64_t began = r.store.newestOp();
        const std::int64_t meanwhile = landedOn(20, "020_1.WAV", a, "op-20-meanwhile", 11500, true);
        r.store.recordSighting(card, 20, "020_1.WAV", 3000, kStamp, hashC, began, 11600);
        CHECK(count(r.store.db(), sinceOf20) < meanwhile);
        CHECK(r.store.hashOfSighting(card, 20, twenty) == hashA);
        r.store.recordSighting(card, 20, "020_1.WAV", 3000, kStamp, hashC, r.store.newestOp(), 11700);
        CHECK_EQ(count(r.store.db(), sinceOf20), meanwhile);
        CHECK(r.store.hashOfSighting(card, 20, twenty) == hashC);
        // another card's read is never asked
        r.store.recordSighting(otherCard, 20, "020_1.WAV", 3000, kStamp, hashB, r.store.newestOp(), 12000);
        CHECK(r.store.hashOfSighting(card, 20, twenty) == hashC);
        CHECK(r.store.hashOfSighting(otherCard, 20, twenty) == hashB);
        // refused: a key that is not a hash, a slot the pedal does not have,
        // a read begun before the first operation could have
        CHECK_THROWS(r.store.recordSighting(card, 20, "020_1.WAV", 3000, kStamp, "short", 1, 12100), "32 bytes");
        CHECK_THROWS(r.store.recordSighting(card, 100, "100_1.WAV", 3000, kStamp, hashA, 1, 12100), "CHECK");
        CHECK_THROWS(r.store.recordSighting(card, 20, "020_1.WAV", 3000, kStamp, hashA, -1, 12100), "before the first");
        // S7: a first sighting resumed later photographs a slot read meanwhile:
        // its operation is older than the read, its photograph is not — the
        // reads of that slot go
        r.store.recordSighting(card, 24, "024_1.WAV", 3000, kStamp, hashC, r.store.newestOp(), 12200);
        CHECK(r.store.hashOfSighting(card, 24, Sighting { "024_1.WAV", 3000, kStamp, 132300 }) == hashC);
        const auto resumed = r.store.firstSeen(r.session, "op-first-again", 12300);
        CHECK(resumed < r.store.newestOp()); // the baseline's sequence is the old one
        r.store.snapshotSlot(resumed, 24, bodyWith(132300), { { 1, "024_1.WAV", b, kStamp } }, 12300);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM take_sightings WHERE slot = 24"), 0);
        CHECK(r.store.hashOfSighting(card, 24, Sighting { "024_1.WAV", 3000, kStamp, 132300 }) == hashB);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM take_sightings WHERE slot = 20"), 2); // untouched
        // forgetting the slot takes its sightings with it, and only its own
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM take_sightings WHERE card = "
                                     + std::to_string(card) + " AND slot = 21"),
                 1);
        r.store.forgetSlot(card, 21, 12400);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM take_sightings WHERE card = "
                                     + std::to_string(card) + " AND slot = 21"),
                 0);
        CHECK(!r.store.hashOfSighting(card, 21, Sighting { "021_1.WAV", 3000, kStamp + 2000, 132300 }).has_value());
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM take_sightings WHERE slot = 20"), 2); // both cards'

        // --- what a new row inherits (heldBefore) ---
        {
            // S3d: a read of these very facts that nothing came after hands on
            // the hash of the bytes it read, with the file's stamp — not the
            // row's hash under the same frozen-clock facts
            const auto renamed = r.store.beginOp(r.session, "op-held", "rename", 13000);
            const HistoryStore::Held fromRead = r.store.heldBefore(renamed, 20, "020_1.WAV", 3000, kStamp);
            CHECK(fromRead.hash == hashC && fromRead.modifiedMs == kStamp);
            // another stamp: another file — no hash, the file's own stamp
            const HistoryStore::Held moved = r.store.heldBefore(renamed, 20, "020_1.WAV", 3000, kStamp + 2000);
            CHECK(!moved.hash.has_value() && moved.modifiedMs == kStamp + 2000);
            // nothing on record: no hash, the file's stamp
            const HistoryStore::Held none = r.store.heldBefore(renamed, 22, "022_1.WAV", 3000, kStamp);
            CHECK(!none.hash.has_value() && none.modifiedMs == kStamp);
            // and the row written so, the newest word, names the read's bytes
            r.store.recordPresentAudio(renamed, 20, 1, "020_1.WAV", 3000, fromRead.hash, fromRead.modifiedMs);
            r.store.finishOp(renamed, OpStatus::done, "");
            CHECK(r.store.hashOfSighting(card, 20, twenty) == hashC);
            // an operation on the slot after the read: the row's word again
            landedOn(20, "020_1.WAV", a, "op-20-last", 13100, true);
            const auto next = r.store.beginOp(r.session, "op-held-2", "rename", 13200);
            const HistoryStore::Held fromRow = r.store.heldBefore(next, 20, "020_1.WAV", 3000, kStamp);
            CHECK(fromRow.hash == hashA && fromRow.modifiedMs == kStamp);
            // a stampless row (older than version 9) vouches by name and size,
            // and the hash goes on without a stamp — whatever the file's is
            r.store.db().exec("UPDATE slot_audio SET modified = NULL WHERE slot = 20");
            const HistoryStore::Held vouched = r.store.heldBefore(next, 20, "020_1.WAV", 3000, kStamp + 4000);
            CHECK(vouched.hash == hashA && !vouched.modifiedMs.has_value());
            const HistoryStore::Held otherSize = r.store.heldBefore(next, 20, "020_1.WAV", 2999, kStamp + 4000);
            CHECK(!otherSize.hash.has_value() && otherSize.modifiedMs == kStamp + 4000);
            CHECK(r.store.hashHeldBefore(next, 20, "020_1.WAV", 3000, kStamp + 4000) == hashA);
            // and a row written that way never tells a connect which file it is
            r.store.recordPresentAudio(next, 20, 1, "020_1.WAV", 3000, vouched.hash, vouched.modifiedMs);
            r.store.finishOp(next, OpStatus::done, "");
            CHECK(!r.store.hashOfSighting(card, 20, Sighting { "020_1.WAV", 3000, kStamp + 4000, 132300 }).has_value());
            CHECK(!r.store.hashOfSighting(card, 20, twenty).has_value());
        }

        // and asking wrote nothing anywhere
        const auto everything = [&r] {
            return std::array<std::int64_t, 7> {
                count(r.store.db(), "SELECT count(*) FROM ops"),
                count(r.store.db(), "SELECT count(*) FROM slot_audio"),
                count(r.store.db(), "SELECT count(*) FROM slot_changes"),
                count(r.store.db(), "SELECT count(*) FROM loudness_readings"),
                count(r.store.db(), "SELECT count(*) FROM sessions"),
                count(r.store.db(), "SELECT sum(last_seen) FROM cards"),
                count(r.store.db(), "SELECT count(*) FROM take_sightings"),
            };
        };
        const auto before = everything();
        for (int slot = 1; slot <= 24; ++slot)
            (void) r.store.hashOfSighting(card, slot, Sighting { "005_1.WAV", 3000, kStamp, 132300 });
        (void) r.store.hashOfSighting(card, 20, twenty);
        (void) r.store.heldBefore(1, 20, "020_1.WAV", 3000, kStamp);
        (void) r.store.newestOp();
        (void) r.store.hashOfSighting(card, 17, Sighting { "017_1.WAV", 3000, kStamp, 132300 });
        (void) r.store.cardFor("test-RC-5");
        (void) r.store.cardFor("never-met");
        CHECK(everything() == before);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM loudness_readings"), 0);
    }

    // --- the operations' sequence never goes back (#141, review S11b) ---
    //
    // Theory: a sequence orders a read among operations, so none is ever
    // issued twice. Forgetting a slot's history drops operations whole when
    // the slot held their only state — the newest ones included — and the
    // next operation must still take a sequence above every one ever
    // issued, across a reopen too. newestOp says the highest ever issued.
    {
        TempDir tmp;
        std::int64_t newest = 0;
        std::int64_t after = 0;
        {
            Ready r(tmp.path);
            const std::int64_t card = *r.store.selectedCard();
            CHECK_EQ(r.store.newestOp(), 0);
            for (const char* id : { "op-a", "op-b", "op-c" }) {
                const auto op = r.store.beginOp(r.session, id, "rename", 2000);
                r.store.recordBodies(op, { { 40, testkit::syntheticSlotBody("Before"),
                                             testkit::syntheticSlotBody("After") } });
                r.store.finishOp(op, OpStatus::done, "");
                CHECK(op > newest);
                newest = op;
            }
            CHECK_EQ(r.store.newestOp(), newest);
            r.store.forgetSlot(card, 40, 3000, true);
            CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM ops"), 0); // forgotten whole, the newest too
            CHECK_EQ(count(r.store.db(), "SELECT coalesce(max(seq), 0) FROM ops"), 0);
            CHECK_EQ(r.store.newestOp(), newest); // the high-water mark stays
            after = r.store.beginOp(r.session, "op-after", "rename", 4000);
            CHECK(after > newest);
            CHECK_EQ(r.store.newestOp(), after);
        }
        HistoryStore reopened(tmp.path); // the first store is closed by now
        CHECK_EQ(reopened.newestOp(), after);
        const auto session = reopened.openSession(reopened.card("test-RC-5", "RC-5", "BOSS RC-5", 5000), 5000);
        CHECK(reopened.beginOp(session, "op-reopened", "rename", 5000) > after);
    }

    return testkit::summary("history_store_tests");
}
