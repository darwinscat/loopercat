// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "support.hpp"
#include "../app/history/HistoryStore.h"
#include "../app/history/Schema.h"
#include "../app/history/Undo.h"
#include <chrono>
#include <filesystem>
using namespace loopercat;
using history::HistoryStore;
using history::OpStatus;
namespace fs = std::filesystem;
namespace {
struct Fixture {
    fs::path dir = fs::temp_directory_path() / ("loopercat-forget-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    HistoryStore store { dir };
    std::int64_t card = store.card("card-a", "RC-5", "A", 1);
    std::int64_t session = store.openSession(card, 1);
    int serial = 0;
    ~Fixture() { fs::remove_all(dir); }
    std::int64_t op(std::vector<int> slots, const std::string& kind = "rename") {
        const auto id = ++serial;
        const auto row = store.beginOp(session, "op-" + std::to_string(id), kind, id + 10);
        for (int slot : slots)
            store.recordBodies(row, {{ slot, testkit::syntheticSlotBody("Before"),
                                             testkit::syntheticSlotBody("After") }});
        store.finishOp(row, OpStatus::done, "");
        return row;
    }
    std::string bytes() { return commands::readFileBytes(dir / "history.db"); }
    std::int64_t scalar(const std::string& sql) {
        sqlite::Statement q(store.db(), sql); q.step(); return q.integer(0);
    }
};
}
int main()
{
    // Three own operations, a swap and a different card with the same slot.
    {
        Fixture f;
        const auto a = f.op({4}); const auto b = f.op({4}); const auto c = f.op({4});
        const auto swap = f.op({4, 7}, "swap");
        const auto other = f.store.card("card-b", "RC-5", "B", 2);
        f.session = f.store.openSession(other, 2);
        const auto otherOp = f.op({4});
        const auto before = f.bytes();
        const auto plan = f.store.planForgetSlot(f.card, 4);
        CHECK_EQ(plan.rowsRemoved, 4);
        CHECK_EQ(plan.takesFreed, 0);
        CHECK(plan.undoTargets == std::vector<std::int64_t>{swap});
        CHECK_EQ(f.bytes(), before); // planning never writes, even with another card selected
        CHECK_THROWS(f.store.forgetSlot(f.card, 4, 30), "separate confirmation");
        CHECK_EQ(f.bytes(), before);
        const auto done = f.store.forgetSlot(f.card, 4, 30, true);
        CHECK(done == plan);
        CHECK(f.store.touchedSlots(swap) == std::vector<int>{7});
        for (auto op : {a, b, c}) CHECK_THROWS(f.store.opStatus(op), "no operation");
        CHECK_EQ(f.store.opStatus(swap), std::string("done"));
        CHECK(f.store.offeredTargets().undo == otherOp);
        CHECK_EQ(f.store.slotTimeline(4).size(), 1u);
        f.store.selectCard(f.card);
        CHECK(f.store.slotTimeline(4).empty());
        CHECK_EQ(f.store.slotTimeline(7).size(), 1u);
        CHECK(!f.store.offeredTargets().undo);
        CHECK_EQ(f.scalar("SELECT count(*) FROM pragma_foreign_key_check"), 0);
    }
    // Last-reference reclamation: dedup across slots, tracks, sides and cards.
    {
        Fixture f;
        const auto op = f.op({4});
        const std::string only(3 * 1024 * 1024, 'x');
        const std::string shared(1024 * 1024, 'y');
        f.store.keepAudio(op, 4, 1, "one.wav", only, 10);
        f.store.recordPresentAudio(op, 4, 1, "one.wav", static_cast<std::int64_t>(only.size()), HistoryStore::contentHash(only));
        f.store.keepAudio(op, 4, 2, "two.wav", shared, 10);
        const auto nine = f.op({9});
        f.store.keepAudio(nine, 9, 1, "same.wav", shared, 10);
        const auto plan = f.store.planForgetSlot(f.card, 4);
        CHECK_EQ(plan.rowsRemoved, 1);
        CHECK_EQ(plan.takesFreed, 1);
        CHECK_EQ(plan.bytesFreed, static_cast<std::int64_t>(only.size()));
        CHECK(!plan.hasHolds());
        CHECK(plan.cutsUndo);
        const auto size = fs::file_size(f.dir / "history.db");
        CHECK(f.store.forgetSlot(f.card, 4, 40) == plan);
        CHECK(!f.store.takeBytes(HistoryStore::contentHash(only)));
        CHECK(f.store.takeBytes(HistoryStore::contentHash(shared)) == shared);
        CHECK_EQ(f.scalar("SELECT released FROM blobs_meta WHERE released IS NOT NULL"), 40);
        int slices = 0;
        while (f.store.vacuum(16) > 0) ++slices;
        CHECK(slices > 1);
        CHECK(fs::file_size(f.dir / "history.db") < size - 2 * 1024 * 1024);
        CHECK(f.store.offeredTargets().undo == nine);
    }
    {
        Fixture f;
        auto a = f.op({4}); f.store.keepAudio(a, 4, 1, "shared", "bytes", 10);
        const auto other = f.store.card("b", "RC-5", "B", 1);
        f.session = f.store.openSession(other, 1);
        auto b = f.op({4}); f.store.keepAudio(b, 4, 1, "shared", "bytes", 11);
        CHECK_EQ(f.store.planForgetSlot(f.card, 4).takesFreed, 0);
        f.store.forgetSlot(f.card, 4, 40, true);
        CHECK(f.store.takeBytes(HistoryStore::contentHash("bytes")) == "bytes");
        CHECK(f.store.offeredTargets().undo == b);
    }
    // A 99-slot first-seen snapshot remains a 98-slot snapshot, including pins.
    {
        Fixture f;
        const auto snapshot = f.store.firstSeen(f.session, "snapshot", 2);
        for (int i = 1; i <= 99; ++i) f.store.snapshotSlot(snapshot, i, "body", {}, 2);
        f.store.finishOp(snapshot, OpStatus::done, "");
        f.store.pinOp(snapshot, true);
        auto plan = f.store.planForgetSlot(f.card, 4);
        CHECK(plan.pinned == std::vector<std::int64_t>{snapshot});
        CHECK(plan.undoTargets.empty());
        CHECK_THROWS(f.store.forgetSlot(f.card, 4, 50), "separate confirmation");
        f.store.forgetSlot(f.card, 4, 50, true);
        CHECK_EQ(f.store.touchedSlots(snapshot).size(), 98u);
        CHECK_EQ(f.store.snapshotSlots(snapshot).size(), 99u);
        CHECK_EQ(f.store.firstSeen(f.session, "again", 60), snapshot);
        CHECK_EQ(f.store.slotTimeline(7).size(), 1u);
        CHECK(f.store.slotTimeline(4).empty());
        CHECK_EQ(f.scalar("SELECT pinned FROM ops WHERE kind = 'snapshot'"), 1);
    }
    // Undo and Redo chains cannot resurrect a removed state; future edits work.
    for (bool forgetUndo : {false, true}) {
        Fixture f;
        const auto first = f.op({4});
        const auto undo = f.op({4}, "undo"); f.store.setReverts(undo, first);
        CHECK(f.store.offeredTargets().redo == undo);
        if (forgetUndo) f.store.pinOp(undo, true);
        const auto before = f.bytes();
        CHECK_THROWS(f.store.forgetSlot(f.card, 4, 40), "separate confirmation");
        CHECK_EQ(f.bytes(), before);
        f.store.forgetSlot(f.card, 4, 40, true);
        CHECK(!f.store.offeredTargets().undo); CHECK(!f.store.offeredTargets().redo);
        auto next = f.op({9});
        CHECK(next > undo); // SQLite must not reuse a sequence below the Undo boundary
        CHECK(f.store.offeredTargets().undo == next);
        HistoryStore reopened(f.dir); reopened.selectCard(f.card);
        CHECK(reopened.offeredTargets().undo == next);
    }
    // Pins alone hold a historical entry; rechecking catches a stale dialog.
    {
        Fixture f;
        const auto old = f.op({4}); f.op({9}); f.store.pinOp(old, true);
        CHECK_THROWS(f.store.forgetSlot(f.card, 4, 40), "separate confirmation");
        auto plan = f.store.planForgetSlot(f.card, 4);
        f.store.pinOp(old, false);
        CHECK_THROWS(f.store.forgetSlot(f.card, 4, 40, true, &plan), "changed");
        plan = f.store.planForgetSlot(f.card, 4);
        f.op({4});
        CHECK_THROWS(f.store.forgetSlot(f.card, 4, 40, true, &plan), "changed");
        CHECK_EQ(f.store.slotTimeline(4).size(), 2u);
    }
    // Foreign keys, audio-only rows, unrelated settings and empty-slot no-op.
    {
        Fixture f;
        auto op = f.op({}, "clear"); f.store.keepAudio(op, 4, 1, "audio", "bytes", 10);
        auto later = f.op({9}, "undo"); f.store.setReverts(later, op);
        CHECK_EQ(f.store.planForgetSlot(f.card, 4).rowsRemoved, 1);
        f.store.forgetSlot(f.card, 4, 40, true);
        CHECK_EQ(f.scalar("SELECT count(*) FROM pragma_foreign_key_check"), 0);
        CHECK_EQ(f.store.slotTimeline(9).size(), 1u);
        const auto before = f.bytes();
        CHECK_EQ(f.store.forgetSlot(f.card, 4, 50).rowsRemoved, 0);
        CHECK_EQ(f.bytes(), before);
        CHECK_THROWS(f.store.planForgetSlot(f.card, 0), "1..99");
        CHECK_THROWS(f.store.forgetSlot(f.card, 100, 50), "1..99");
        auto mixed = f.op({4});
        f.store.recordSystemChange(mixed, {"CTL", "<CTL>before</CTL>", "<CTL>after</CTL>"});
        f.store.forgetSlot(f.card, 4, 50, true);
        CHECK_EQ(f.store.systemChanges(mixed).size(), 1u);
        CHECK_EQ(f.store.opStatus(mixed), std::string("done"));
    }
    // Pending writes refuse even extra confirmation; any SQL failure rolls back everything.
    {
        Fixture f;
        auto op = f.store.beginOp(f.session, "pending", "clear", 5);
        f.store.keepAudio(op, 4, 1, "take", "bytes", 5);
        CHECK_THROWS(f.store.forgetSlot(f.card, 4, 40, true), "still being recorded");
        f.store.finishOp(op, OpStatus::done, "");
        f.store.db().exec("CREATE TRIGGER fail_forget BEFORE DELETE ON blobs BEGIN SELECT RAISE(ABORT, 'injected'); END");
        const auto before = f.bytes();
        CHECK_THROWS(f.store.forgetSlot(f.card, 4, 40, true), "injected");
        CHECK_EQ(f.bytes(), before);
        CHECK(f.store.offeredTargets().undo == op);
        CHECK(f.store.takeBytes(HistoryStore::contentHash("bytes")) == "bytes");
    }
    return testkit::summary("forget_slot_tests");
}
