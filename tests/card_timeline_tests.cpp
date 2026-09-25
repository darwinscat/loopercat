// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The whole card's timeline out of a real store (#73), and a take exported
// to a file, attacked from what they promise:
//
//   - one entry per operation, in the order a player reads: by time, so a
//     row imported from the folders that predate the store — written last,
//     older than everything — comes first
//   - a swap is one entry with two slots, each carrying what slotTimeline
//     gives that slot: the two views cannot drift into two truths
//   - the last operation on a slot is marked as the state the slot is in
//   - an operation that touched no slot is still an entry, with none
//   - pins ride along
//   - an exported take is the bytes, whole; a take no longer kept writes
//     nothing at all

#include "support.hpp"

#include "../app/history/HistoryStore.h"
#include "../app/history/TakeExport.h"

#include <loopercat/Commands.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace loopercat;
using history::HistoryStore;
using history::OpStatus;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-card-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

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

struct Ready {
    HistoryStore store;
    std::int64_t session;
    std::int64_t clock = 10'000;
    int ops = 0;
    explicit Ready(const fs::path& dir) : store(dir)
    {
        session = store.openSession(store.card("RC-5", "BOSS RC-5", 1000), 1000);
    }
    std::int64_t begin(const std::string& kind)
    {
        return store.beginOp(session, "op-" + std::to_string(++ops), kind, ++clock);
    }
};

const HistoryStore::CardEntry* entryFor(const std::vector<HistoryStore::CardEntry>& entries,
                                        std::int64_t op)
{
    for (const auto& e : entries)
        if (e.op == op)
            return &e;
    return nullptr;
}

} // namespace

int main()
{
    // --- an empty store has an empty timeline ---
    {
        TempDir tmp;
        HistoryStore store(tmp.path);
        CHECK(store.cardTimeline().empty());
    }

    // --- one entry per operation, by time; a swap is one entry with two slots ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string bytesA = take(3000, 1);
        const std::string bytesB = take(3000, 2);

        const auto push = r.begin("push");
        r.store.recordBodies(push, { { 12, "b12-0", "b12-1" } });
        r.store.recordLanded(push, 12, 1, "a.wav", bytesA);
        r.store.finishOp(push, OpStatus::done, "");

        const auto trim = r.begin("trim");
        r.store.keepAudio(trim, 12, 1, "a.wav", bytesA, ++r.clock);
        r.store.recordBodies(trim, { { 12, "b12-1", "b12-2" } });
        r.store.recordLanded(trim, 12, 1, "a.wav", bytesB);
        r.store.finishOp(trim, OpStatus::done, "");

        const auto swap = r.begin("swap");
        r.store.recordBodies(swap, { { 12, "b12-2", "b43-0" }, { 43, "b43-0", "b12-2" } });
        r.store.recordPresentAudio(swap, 43, 1, "a.wav", 3000, HistoryStore::contentHash(bytesB));
        r.store.finishOp(swap, OpStatus::done, "");

        const auto failed = r.begin("clear"); // refused before it touched the card
        r.store.finishOp(failed, OpStatus::failed, "the pedal is away");

        const auto entries = r.store.cardTimeline();
        CHECK_EQ(entries.size(), 4u);
        CHECK_EQ(entries[0].op, push);
        CHECK_EQ(entries[1].op, trim);
        CHECK_EQ(entries[2].op, swap);
        CHECK_EQ(entries[3].op, failed);

        // the swap: two slots, the other slot named on each, both newest
        const auto* s = entryFor(entries, swap);
        CHECK(s != nullptr);
        CHECK(s && s->slots.size() == 2);
        CHECK(s && s->slots.size() == 2 && s->slots[0].slot == 12 && s->slots[1].slot == 43);
        CHECK(s && s->slots.size() == 2 && s->slots[0].facts.swappedWith == 43);
        CHECK(s && s->slots.size() == 2 && s->slots[1].facts.swappedWith == 12);
        CHECK(s && s->slots.size() == 2 && s->slots[0].newest && s->slots[1].newest);
        // a row offers the take its own state holds: the push's take is the
        // one the trim archived (kept), the trim's is the one it left on the
        // card (not in the store); neither is the newest on slot 12
        const auto* p = entryFor(entries, push);
        CHECK(p && p->slots.size() == 1 && !p->slots[0].newest);
        CHECK(p && p->slots.size() == 1 && p->slots[0].facts.takeKept);
        CHECK(p && p->slots.size() == 1 && p->slots[0].facts.takeHash == HistoryStore::contentHash(bytesA));
        const auto* t = entryFor(entries, trim);
        CHECK(t && t->slots.size() == 1 && !t->slots[0].newest);
        CHECK(t && t->slots.size() == 1 && !t->slots[0].facts.takeKept);
        CHECK(t && t->slots.size() == 1 && t->slots[0].facts.takeHash == HistoryStore::contentHash(bytesB));
        CHECK(t && t->slots.size() == 1 && t->slots[0].facts.afterBody == std::string("b12-2"));
        // the failed op touched nothing and is still an entry, with its reason
        const auto* f = entryFor(entries, failed);
        CHECK(f && f->slots.empty());
        CHECK(f && f->status == "failed");
        CHECK(f && f->note == "the pedal is away");
        CHECK(f && !f->pinned);

        // the two views agree: every slot's facts are slotTimeline's for that op
        for (const auto& entry : entries)
            for (const auto& touched : entry.slots) {
                bool found = false;
                for (const auto& row : r.store.slotTimeline(touched.slot)) {
                    if (row.op != entry.op)
                        continue;
                    found = true;
                    CHECK(row.beforeBody == touched.facts.beforeBody);
                    CHECK(row.afterBody == touched.facts.afterBody);
                    CHECK(row.swappedWith == touched.facts.swappedWith);
                    CHECK_EQ(row.takeName, touched.facts.takeName);
                    CHECK(row.takeHash == touched.facts.takeHash);
                    CHECK_EQ(row.takeKept, touched.facts.takeKept);
                    CHECK_EQ(row.kind, touched.facts.kind);
                    CHECK_EQ(row.note, touched.facts.note);
                }
                CHECK(found);
            }
        // and "newest" is exactly the last row of each slot's own timeline
        for (const int slot : { 12, 43 }) {
            const auto own = r.store.slotTimeline(slot);
            CHECK(!own.empty());
            for (const auto& entry : entries)
                for (const auto& touched : entry.slots)
                    if (touched.slot == slot)
                        CHECK_EQ(touched.newest, !own.empty() && own.back().op == entry.op);
        }
    }

    // --- a legacy row is written last and belongs first ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const auto live = r.begin("rename");
        r.store.recordBodies(live, { { 5, "before", "after" } });
        r.store.finishOp(live, OpStatus::done, "");
        const auto legacySession = r.store.openSession(r.store.card("unknown", "legacy folders", 1), 1);
        const auto old = r.store.recordLegacyOp(legacySession, "2026-09-01T21-35-46", 500,
                                                "trash/2026-09-01T21-35-46");
        r.store.keepLegacyTake(old, "trash/2026-09-01T21-35-46/005_1/005_1.WAV", 5, 1, "005_1.WAV",
                               take(2000, 9), 600);
        CHECK(old > live); // written after
        const auto entries = r.store.cardTimeline();
        CHECK_EQ(entries.size(), 2u);
        CHECK_EQ(entries.front().op, old); // read first
        CHECK_EQ(entries.front().actor, std::string("legacy"));
        CHECK(entries.front().slots.size() == 1 && entries.front().slots[0].slot == 5);
        CHECK(entries.front().slots.size() == 1 && entries.front().slots[0].facts.takeKept);
        CHECK(entries.front().slots.size() == 1 && !entries.front().slots[0].newest);
        CHECK(entries.back().slots.size() == 1 && entries.back().slots[0].newest);
    }

    // --- pins ride along ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const auto op = r.begin("rename");
        r.store.recordBodies(op, { { 5, "before", "after" } });
        r.store.finishOp(op, OpStatus::done, "");
        CHECK(!r.store.cardTimeline().front().pinned);
        r.store.pinOp(op, true);
        CHECK(r.store.cardTimeline().front().pinned);
    }

    // --- an exported take is the bytes, whole; nothing is written for a take not kept ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const std::string bytes = take(200000, 3);
        const std::string hash = HistoryStore::contentHash(bytes);
        const auto op = r.begin("trim");
        r.store.keepAudio(op, 7, 1, "loop.wav", bytes, ++r.clock);
        r.store.finishOp(op, OpStatus::done, "");

        const fs::path out = tmp.path / "exported" / "loop.wav";
        fs::create_directories(out.parent_path());
        CHECK_EQ(history::exportTake(r.store, hash, out), 200000);
        CHECK(commands::readFileBytes(out) == bytes);
        CHECK(!fs::exists(out.string() + history::kExportPartial));
        // exported again over itself: replaced, still whole
        CHECK_EQ(history::exportTake(r.store, hash, out), 200000);
        CHECK(commands::readFileBytes(out) == bytes);

        const fs::path never = tmp.path / "exported" / "never.wav";
        CHECK_THROWS(history::exportTake(r.store, HistoryStore::contentHash("never"), never),
                     "no longer kept");
        CHECK(!fs::exists(never));
        CHECK(!fs::exists(never.string() + history::kExportPartial));
        CHECK_THROWS(history::exportTake(r.store, hash, fs::path()), "file name");
        CHECK_THROWS(history::exportTake(r.store, hash, tmp.path / "nowhere" / "x.wav"), "");
        CHECK(!fs::exists(tmp.path / "nowhere" / "x.wav.part"));

        r.store.releaseBlobs({ hash }, std::nullopt, 9);
        const fs::path gone = tmp.path / "exported" / "gone.wav";
        CHECK_THROWS(history::exportTake(r.store, hash, gone), "no longer kept");
        CHECK(!fs::exists(gone));
    }

    return testkit::summary("card_timeline_tests");
}
