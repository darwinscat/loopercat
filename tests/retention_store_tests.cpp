// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The store's side of freeing space (issue #74), attacked from what it
// promises:
//
//   - what the history costs is read from the file, each take counted once
//   - the undo on offer is the newest finished operation that is not legacy
//   - a take is held while any row naming it is pinned, in flight, or the
//     undo's 'before'; a shared take is released only when the last lets go
//   - releasing frees the bytes and nothing else: every row keeps naming the
//     take, and says honestly that its bytes are gone
//   - a held take is refused, and a refused list frees nothing at all
//   - passing the limit deletes nothing by itself
//   - incremental vacuum really gives the file's space back, in slices

#include "support.hpp"

#include "../app/history/HistoryStore.h"
#include "../app/history/Retention.h"
#include "../app/history/Schema.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace loopercat;
using history::HistoryStore;
using history::OpStatus;
namespace retention = history::retention;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t MB = std::int64_t { 1 } << 20;

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-retention-" + std::to_string(stamp));
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

// A store with one open session and a clock that only moves forward.
struct Ready {
    HistoryStore store;
    std::int64_t session;
    std::int64_t clock = 10'000;
    int ops = 0;

    explicit Ready(const fs::path& dir) : store(dir)
    {
        session = store.openSession(store.card("RC-5", "BOSS RC-5", 1000), 1000);
    }
    std::int64_t now() { return ++clock; }

    // One finished operation that replaced `bytes` in `slot`.
    std::int64_t replaced(int slot, const std::string& bytes, OpStatus status = OpStatus::done)
    {
        const auto op = store.beginOp(session, "op-" + std::to_string(++ops), "trim", now());
        store.keepAudio(op, slot, 1, "take.wav", bytes, now());
        store.finishOp(op, status, "");
        return op;
    }
    // An operation that changed only a body: nothing archived.
    std::int64_t renamed(int slot)
    {
        const auto op = store.beginOp(session, "op-" + std::to_string(++ops), "rename", now());
        store.recordBodies(op, { { slot, "before", "after" } });
        store.finishOp(op, OpStatus::done, "");
        return op;
    }
    std::int64_t pending(int slot, const std::string& bytes)
    {
        const auto op = store.beginOp(session, "op-" + std::to_string(++ops), "clear", now());
        store.keepAudio(op, slot, 1, "take.wav", bytes, now());
        return op;
    }
};

const retention::Blob* find(const std::vector<retention::Blob>& blobs, const std::string& hash)
{
    for (const auto& b : blobs)
        if (b.hash == hash)
            return &b;
    return nullptr;
}

} // namespace

int main()
{
    // --- what the history costs, read from the file, each take once ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const auto empty = r.store.usage();
        CHECK_EQ(empty.audioBytes, 0);
        CHECK_EQ(empty.freeBytes, 0);
        CHECK(empty.fileBytes > 0);
        CHECK_EQ(empty.fileBytes, static_cast<std::int64_t>(fs::file_size(tmp.path / "history.db")));
        CHECK_EQ(empty.otherBytes, empty.fileBytes);
        CHECK(empty.diskAvailable > 0);

        const std::string one = take(static_cast<std::size_t>(MB), 1);
        const std::string two = take(static_cast<std::size_t>(2 * MB), 2);
        const std::string three = take(static_cast<std::size_t>(3 * MB), 3);
        r.replaced(1, one);
        r.replaced(2, two);
        r.replaced(3, three);
        r.replaced(4, two); // the same bytes again: kept once, counted once
        const auto full = r.store.usage();
        CHECK_EQ(full.audioBytes, 6 * MB);
        CHECK(full.fileBytes >= full.audioBytes);
        CHECK_EQ(full.fileBytes, static_cast<std::int64_t>(fs::file_size(tmp.path / "history.db")));
        CHECK(full.otherBytes >= 0);
        CHECK(full.otherBytes < 2 * MB); // rows and indexes, not another take
        CHECK_EQ(full.fileBytes, full.audioBytes + full.otherBytes + full.freeBytes);
        // the limit is not the store's business: it kept 6 MB under any limit
        CHECK(full.audioBytes > 5 * MB);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs"), 3);
    }

    // --- the undo on offer: the newest finished operation that is not legacy ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        CHECK(!r.store.offeredUndo().has_value());
        const auto first = r.replaced(1, take(1000, 1));
        CHECK(r.store.offeredUndo() == first);
        r.replaced(2, take(1000, 2), OpStatus::failed);
        CHECK(r.store.offeredUndo() == first);
        const auto third = r.renamed(3);
        CHECK(r.store.offeredUndo() == third);
        const auto inFlight = r.pending(4, take(1000, 4));
        CHECK(r.store.offeredUndo() == third);
        r.store.finishOp(inFlight, OpStatus::done, "");
        CHECK(r.store.offeredUndo() == inFlight);
        // a legacy op is newer in the timeline's numbering, and offers no undo
        const auto legacySession = r.store.openSession(r.store.card("unknown", "legacy folders", 1), 1);
        r.store.recordLegacyOp(legacySession, "2026-09-01T21-35-46", 500, "trash/2026-09-01T21-35-46");
        CHECK(r.store.offeredUndo() == inFlight);
    }

    // --- what holds a take, and what does not ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string shared = take(20000, 1);
        const std::string hash = HistoryStore::contentHash(shared);
        const auto opA = r.replaced(1, shared);
        const auto opB = r.replaced(2, shared);
        r.store.recordLanded(opB, 2, 1, "landed.wav", shared); // an 'after' naming the same bytes
        const std::string other = take(20000, 2);
        const auto opC = r.replaced(3, other);

        auto blobs = r.store.keptBlobs(std::nullopt);
        CHECK_EQ(blobs.size(), 2u);
        const auto* s = find(blobs, hash);
        CHECK(s != nullptr);
        CHECK(s && s->references == 3);
        CHECK(s && s->size == 20000);
        CHECK(s && !s->held());
        CHECK_EQ(blobs.front().hash, hash); // the older first

        // the undo on offer is opC: only `other` is needed by it
        blobs = r.store.keptBlobs(r.store.offeredUndo());
        CHECK(find(blobs, hash) && !find(blobs, hash)->undo);
        CHECK(find(blobs, HistoryStore::contentHash(other))->undo);
        // asked about opA or opB instead, the shared take is the one held
        CHECK(find(r.store.keptBlobs(opA), hash)->undo);
        CHECK(find(r.store.keptBlobs(opB), hash)->undo);

        // a pin on either operation holds the shared take; both must let go
        r.store.pinOp(opA, true);
        r.store.pinOp(opB, true);
        CHECK(find(r.store.keptBlobs(std::nullopt), hash)->pinned);
        r.store.pinOp(opA, false);
        CHECK(find(r.store.keptBlobs(std::nullopt), hash)->pinned);
        r.store.pinOp(opB, false);
        CHECK(!find(r.store.keptBlobs(std::nullopt), hash)->pinned);
        CHECK(!find(r.store.keptBlobs(std::nullopt), HistoryStore::contentHash(other))->pinned);
        // a pin on the blob itself counts too
        {
            sqlite::Statement pin(r.store.db(), "UPDATE blobs_meta SET pinned = 1 WHERE hash = ?1");
            pin.bindBlob(1, hash).run();
        }
        CHECK(find(r.store.keptBlobs(std::nullopt), hash)->pinned);
        {
            sqlite::Statement pin(r.store.db(), "UPDATE blobs_meta SET pinned = 0 WHERE hash = ?1");
            pin.bindBlob(1, hash).run();
        }
        CHECK_THROWS(r.store.pinOp(424242, true), "no operation");

        // an operation still running holds what it archived
        const std::string live = take(20000, 3);
        const auto opD = r.pending(4, live);
        CHECK(find(r.store.keptBlobs(std::nullopt), HistoryStore::contentHash(live))->inFlight);
        CHECK(!find(r.store.keptBlobs(std::nullopt), hash)->inFlight);
        r.store.finishOp(opD, OpStatus::done, "");
        CHECK(!find(r.store.keptBlobs(std::nullopt), HistoryStore::contentHash(live))->inFlight);
        (void) opC;
    }

    // --- a legacy document is a kept blob too, held by a pin on its op ---
    {
        TempDir tmp;
        HistoryStore store(tmp.path);
        const auto session = store.openSession(store.card("unknown", "legacy folders", 1), 1);
        const auto op = store.recordLegacyOp(session, "2026-09-01T21-35-46", 500, "backups/x");
        store.keepLegacyDocument(op, "backups/2026-09-01T21-35-46/MEMORY1.RC0", "document", 600);
        auto blobs = store.keptBlobs(std::nullopt);
        CHECK_EQ(blobs.size(), 1u);
        CHECK_EQ(blobs.front().references, 1);
        CHECK(!blobs.front().held());
        store.pinOp(op, true);
        CHECK(store.keptBlobs(std::nullopt).front().pinned);
        CHECK_THROWS(store.releaseBlobs({ blobs.front().hash }, std::nullopt, 700), "pinned");
        store.pinOp(op, false);
        CHECK_EQ(store.releaseBlobs({ blobs.front().hash }, std::nullopt, 700), 8);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM legacy_files"), 1); // the ledger row stays
    }

    // --- releasing frees the bytes and nothing else ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string gone = take(30000, 1);
        const std::string stays = take(30000, 2);
        const std::string hGone = HistoryStore::contentHash(gone);
        const std::string hStays = HistoryStore::contentHash(stays);
        const auto opA = r.replaced(1, gone);
        const auto opB = r.replaced(2, gone);
        r.store.recordBodies(opB, { { 2, "b", "a" } });
        const auto opC = r.replaced(3, stays);
        const auto rowsBefore = count(r.store.db(), "SELECT count(*) FROM slot_audio");
        const auto opsBefore = count(r.store.db(), "SELECT count(*) FROM ops");

        CHECK_EQ(r.store.releaseBlobs({ hGone }, r.store.offeredUndo(), 777), 30000);
        CHECK(!r.store.takeBytes(hGone).has_value());
        CHECK(r.store.takeBytes(hStays) == stays);
        // every row still names the take — by name, size and hash
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM slot_audio"), rowsBefore);
        sqlite::Statement rows(r.store.db(), "SELECT count(*) FROM slot_audio WHERE hash = ?1 "
                                             "AND name = 'take.wav' AND size = 30000");
        rows.bindBlob(1, hGone);
        CHECK(rows.step() && rows.integer(0) == 2);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM ops"), opsBefore);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM slot_changes"), 1);
        CHECK_EQ(r.store.opStatus(opA), std::string("done"));
        // and the metadata says when the bytes went
        sqlite::Statement meta(r.store.db(), "SELECT released, size FROM blobs_meta WHERE hash = ?1");
        meta.bindBlob(1, hGone);
        CHECK(meta.step());
        CHECK_EQ(meta.integer(0), 777);
        CHECK_EQ(meta.integer(1), 30000);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs_meta"), 2);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs"), 1);
        // a released take is no longer a kept one
        CHECK_EQ(r.store.keptBlobs(std::nullopt).size(), 1u);
        CHECK_EQ(r.store.usage().audioBytes, 30000);
        // released twice is refused, as is a take never kept
        CHECK_THROWS(r.store.releaseBlobs({ hGone }, std::nullopt, 778), "no bytes are kept");
        CHECK_THROWS(r.store.releaseBlobs({ HistoryStore::contentHash("never") }, std::nullopt, 778),
                     "no bytes are kept");
        // and kept again, the take is back: the row was waiting for it
        r.replaced(4, gone);
        CHECK(r.store.takeBytes(hGone) == gone);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs_meta WHERE released IS NULL"), 2);
        CHECK_EQ(r.store.usage().audioBytes, 60000);
        (void) opC;
    }

    // --- a held take is refused, and a refused list frees nothing at all ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string free = take(10000, 1);
        const std::string held = take(10000, 2);
        const std::string hFree = HistoryStore::contentHash(free);
        const std::string hHeld = HistoryStore::contentHash(held);
        r.replaced(1, free);
        const auto last = r.replaced(2, held); // the undo on offer needs `held`

        CHECK_THROWS(r.store.releaseBlobs({ hFree, hHeld }, r.store.offeredUndo(), 900),
                     "needed by the undo");
        CHECK(r.store.takeBytes(hFree) == free); // nothing went, not even the free one
        CHECK(r.store.takeBytes(hHeld) == held);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs_meta WHERE released IS NOT NULL"), 0);

        r.store.pinOp(last, true);
        CHECK_THROWS(r.store.releaseBlobs({ hHeld }, std::nullopt, 900), "is pinned");
        r.store.pinOp(last, false);
        const auto running = r.pending(3, free);
        CHECK_THROWS(r.store.releaseBlobs({ hFree }, std::nullopt, 900), "still running");
        r.store.finishOp(running, OpStatus::failed, "unplugged");
        // asked without an undo in mind, the store still refuses nothing it should not
        CHECK_EQ(r.store.releaseBlobs({ hFree }, std::nullopt, 901), 10000);
        CHECK(r.store.takeBytes(hHeld) == held);
    }

    // --- a shared take is released only when the last row lets go ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string shared = take(10000, 1);
        const std::string hash = HistoryStore::contentHash(shared);
        r.replaced(1, shared);
        const auto opB = r.replaced(2, shared); // the undo on offer holds it through opB
        CHECK(find(r.store.keptBlobs(r.store.offeredUndo()), hash)->held());
        CHECK_THROWS(r.store.releaseBlobs({ hash }, r.store.offeredUndo(), 1), "undo");
        r.renamed(5); // the undo moves on; opB's row still names the take, but holds nothing
        CHECK(r.store.offeredUndo() != opB);
        CHECK(!find(r.store.keptBlobs(r.store.offeredUndo()), hash)->held());
        CHECK_EQ(find(r.store.keptBlobs(r.store.offeredUndo()), hash)->references, 2);
        CHECK_EQ(r.store.releaseBlobs({ hash }, r.store.offeredUndo(), 2), 10000);
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM slot_audio"), 2);
    }

    // --- passing the limit deletes nothing by itself ---
    {
        TempDir tmp;
        const std::int64_t limit = 5 * MB;
        {
            Ready r(tmp.path);
            for (unsigned i = 1; i <= 3; ++i)
                r.replaced(static_cast<int>(i), take(static_cast<std::size_t>(3 * MB), i));
            CHECK(r.store.usage().audioBytes > limit);
            const auto plan = retention::plan(r.store.keptBlobs(r.store.offeredUndo()), limit);
            CHECK(!plan.withinTarget());
            CHECK_EQ(plan.release.size(), 2u); // the plan knows what WOULD go
            CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM blobs"), 3); // and nothing did
        }
        HistoryStore reopened(tmp.path);
        CHECK_EQ(count(reopened.db(), "SELECT count(*) FROM blobs"), 3);
        CHECK_EQ(reopened.usage().audioBytes, 9 * MB);
        CHECK_EQ(count(reopened.db(), "SELECT count(*) FROM blobs_meta WHERE released IS NOT NULL"), 0);
    }

    // --- the plan, carried out: what it names goes, what it holds stays ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        std::vector<std::string> hashes;
        for (unsigned i = 1; i <= 4; ++i) {
            const std::string bytes = take(static_cast<std::size_t>(2 * MB), i);
            hashes.push_back(HistoryStore::contentHash(bytes));
            r.replaced(static_cast<int>(i), bytes);
        }
        const auto undo = r.store.offeredUndo();
        const auto plan = retention::plan(r.store.keptBlobs(undo), 3 * MB);
        CHECK_EQ(plan.kept, 8 * MB);
        CHECK_EQ(plan.held, 2 * MB); // the last one, needed by undo
        CHECK_EQ(plan.release.size(), 3u);
        CHECK_EQ(plan.release.front().hash, hashes[0]); // oldest first
        std::vector<std::string> toGo;
        for (const auto& b : plan.release)
            toGo.push_back(b.hash);
        CHECK_EQ(r.store.releaseBlobs(toGo, undo, 5), 6 * MB);
        CHECK(r.store.takeBytes(hashes[3]).has_value());
        CHECK(!r.store.takeBytes(hashes[0]).has_value());
        CHECK_EQ(r.store.usage().audioBytes, 2 * MB);
        CHECK(retention::plan(r.store.keptBlobs(undo), 3 * MB).withinTarget());
    }

    // --- incremental vacuum gives the file's space back, in slices ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        std::vector<std::string> hashes;
        for (unsigned i = 1; i <= 4; ++i) {
            const std::string bytes = take(static_cast<std::size_t>(2 * MB), i);
            hashes.push_back(HistoryStore::contentHash(bytes));
            r.replaced(static_cast<int>(i), bytes);
        }
        r.renamed(9); // so no take is the undo's
        const auto before = r.store.usage();
        CHECK(before.fileBytes >= 8 * MB);
        CHECK_EQ(before.freeBytes, 0);

        CHECK_EQ(r.store.releaseBlobs({ hashes[0], hashes[1], hashes[2] }, r.store.offeredUndo(), 5),
                 6 * MB);
        const auto released = r.store.usage();
        CHECK_EQ(released.fileBytes, before.fileBytes); // the file has not shrunk yet
        CHECK(released.freeBytes >= 6 * MB);             // but it knows what it no longer uses
        CHECK_EQ(released.audioBytes, 2 * MB);
        const std::int64_t pageSize = history::schema::pragmaInteger(r.store.db(), "page_size");
        const std::int64_t freePages = released.freeBytes / pageSize;

        // one page at a time: exactly one page fewer, and the file one page shorter
        CHECK_EQ(r.store.vacuum(1), freePages - 1);
        CHECK_EQ(r.store.usage().fileBytes, before.fileBytes - pageSize);
        // then in slices until nothing is left
        int slices = 0;
        while (r.store.vacuum(64) > 0)
            ++slices;
        CHECK(slices >= 2);
        const auto after = r.store.usage();
        CHECK_EQ(after.freeBytes, 0);
        CHECK(after.fileBytes <= before.fileBytes - 6 * MB);
        CHECK_EQ(after.fileBytes, static_cast<std::int64_t>(fs::file_size(tmp.path / "history.db")));
        CHECK(r.store.takeBytes(hashes[3]) == take(static_cast<std::size_t>(2 * MB), 4));
        // nothing left to give: a slice on a full file is a no-op
        CHECK_EQ(r.store.vacuum(64), 0);
        CHECK_THROWS(r.store.vacuum(0), "at least one page");
    }

    return testkit::summary("retention_store_tests");
}
