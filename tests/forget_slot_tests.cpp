// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "support.hpp"
#include "../app/history/HistoryStore.h"
#include "../app/history/Schema.h"
#include "../app/history/Undo.h"
#include <chrono>
#include <filesystem>
#include <optional>
using namespace loopercat;
using history::HistoryStore;
using history::OpStatus;
namespace fs = std::filesystem;
namespace {
struct Fixture {
    fs::path dir = fs::temp_directory_path() / ("loopercat-forget-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::optional<HistoryStore> open { std::in_place, dir };
    HistoryStore& store = *open;
    std::int64_t card = store.card("card-a", "RC-5", "A", 1);
    std::int64_t session = store.openSession(card, 1);
    int serial = 0;
    // Windows refuses to delete a file something still holds open: close the
    // store before the directory. A throw here would leave a destructor and
    // kill the suite with nothing but an exit code, so it is reported instead.
    ~Fixture() {
        open.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
        if (ec) testkit::fail("scratch history left behind: " + ec.message(), __FILE__, __LINE__);
    }
    std::int64_t op(std::vector<int> slots, const std::string& kind = "rename") {
        const auto id = ++serial;
        const auto row = store.beginOp(session, "op-" + std::to_string(id), kind, id + 10);
        for (int slot : slots)
            store.recordBodies(row, {{ slot, testkit::syntheticSlotBody("Before"),
                                             testkit::syntheticSlotBody("After") }});
        store.finishOp(row, OpStatus::done, "");
        return row;
    }
    // An operation about `subjects` (#144) that then changed `slots`.
    std::int64_t about(std::vector<int> subjects, std::vector<int> slots, const std::string& kind,
                       OpStatus outcome = OpStatus::done) {
        const auto id = ++serial;
        const auto row = store.beginOp(session, "op-" + std::to_string(id), kind, id + 10);
        for (int slot : subjects)
            store.recordSubject(row, slot);
        for (int slot : slots)
            store.recordBodies(row, {{ slot, testkit::syntheticSlotBody("Before"),
                                             testkit::syntheticSlotBody("After") }});
        store.finishOp(row, outcome, outcome == OpStatus::done ? "" : "the card went away");
        return row;
    }
    std::string bytes() { return commands::readFileBytes(dir / "history.db"); }
    std::int64_t scalar(const std::string& sql) {
        sqlite::Statement q(store.db(), sql); q.step(); return q.integer(0);
    }
};
}
static int runTests()
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
        f.store.recordPresentAudio(op, 4, 1, "one.wav", static_cast<std::int64_t>(only.size()), HistoryStore::contentHash(only), 4000);
        f.store.keepAudio(op, 4, 2, "two.wav", shared, 10);
        const auto nine = f.op({9});
        f.store.keepAudio(nine, 9, 1, "same.wav", shared, 10);
        const auto plan = f.store.planForgetSlot(f.card, 4);
        CHECK_EQ(plan.rowsRemoved, 1);
        CHECK_EQ(plan.takesFreed, 1);
        CHECK_EQ(plan.bytesFreed, static_cast<std::int64_t>(only.size()));
        CHECK(!plan.hasHolds());
        CHECK(!plan.cutsUndo); // forgotten whole: nothing of it stands to become a boundary
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
        // A new operation must land above the card's Undo boundary, or the store
        // would hide work that has just been done. Operations forgotten whole
        // move no boundary, so here the boundary is still nothing.
        CHECK(next > f.scalar("SELECT undo_floor FROM cards WHERE id = " + std::to_string(f.card)));
        CHECK_EQ(f.scalar("SELECT undo_floor FROM cards WHERE id = " + std::to_string(f.card)), 0);
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
    // Work on other slots stays undoable: an operation forgotten whole leaves
    // nothing behind, so it is not an Undo boundary for the rest of the card.
    {
        Fixture f;
        const auto nine = f.op({9});
        const auto four = f.op({4});
        CHECK(f.store.offeredTargets().undo == four);
        f.store.forgetSlot(f.card, 4, 40, true);
        CHECK(f.store.offeredTargets().undo == nine);
        CHECK_EQ(f.scalar("SELECT undo_floor FROM cards WHERE id = " + std::to_string(f.card)), 0);
        CHECK_EQ(f.store.slotTimeline(9).size(), 1u);
        CHECK_EQ(f.scalar("SELECT count(*) FROM forgotten_slots"), 0);
    }
    // A swap that survives half-forgotten IS a boundary: undoing it would put
    // the other slot back and leave the forgotten one where it is.
    {
        Fixture f;
        f.op({9});
        const auto swap = f.op({4, 9}, "swap");
        f.store.forgetSlot(f.card, 4, 40, true);
        CHECK_EQ(f.store.opStatus(swap), std::string("done"));
        CHECK(!f.store.offeredTargets().undo);
        CHECK_EQ(f.scalar("SELECT undo_floor FROM cards WHERE id = " + std::to_string(f.card)), swap);
    }
    // A slot the first sighting never reached is not marked forgotten: only
    // clearing the snapshot's own row may make a resumed run skip a slot.
    {
        Fixture f;
        const auto snapshot = f.store.firstSeen(f.session, "marker-a", 5);
        for (int slot : {1, 2})
            f.store.snapshotSlot(snapshot, slot, testkit::syntheticSlotBody("First"), {}, 10);
        f.store.finishOp(snapshot, OpStatus::interrupted, "unplugged"); // it never reached slot 50
        f.op({50});
        f.store.forgetSlot(f.card, 50, 40, true);
        CHECK_EQ(f.scalar("SELECT count(*) FROM forgotten_slots"), 0);
        CHECK((f.store.snapshotSlots(snapshot) == std::vector<int>{1, 2})); // slot 50 still to come
        f.store.forgetSlot(f.card, 1, 50, true);
        CHECK_EQ(f.scalar("SELECT count(*) FROM forgotten_slots"), 1);
        CHECK((f.store.snapshotSlots(snapshot) == std::vector<int>{1, 2})); // 1 counts as covered
        CHECK(f.store.touchedSlots(snapshot) == std::vector<int>{2});
    }
    // An operation about slot 7 that changed nothing there (#144) is one of
    // the slot's entries: the plan counts it as the tab showed it, and
    // forgetting the slot takes its row away whole. A swap about both slots
    // keeps the other slot's rows and its subject, and stays the boundary
    // it was; one with no state left keeps standing for its other subject.
    {
        Fixture f;
        const auto nine = f.op({9});
        const auto nothing = f.about({7}, {}, "normalize");
        const auto swap = f.about({7, 12}, {7, 12}, "swap");
        const auto failed = f.about({7, 12}, {}, "swap", OpStatus::failed);
        const auto shown = f.store.slotTimeline(7).size();
        CHECK_EQ(shown, 3u);
        const auto plan = f.store.planForgetSlot(f.card, 7);
        CHECK_EQ(plan.rowsRemoved, static_cast<std::int64_t>(shown));
        CHECK((plan.operations == std::vector<std::int64_t>{nothing, swap, failed}));
        CHECK(plan.undoTargets == std::vector<std::int64_t>{swap});
        CHECK_EQ(plan.takesFreed, 0);
        CHECK(f.store.forgetSlot(f.card, 7, 40, true, &plan) == plan);
        CHECK_THROWS(f.store.opStatus(nothing), "no operation"); // nothing of it was left
        CHECK(f.store.subjects(swap) == std::vector<int>{12});
        CHECK(f.store.touchedSlots(swap) == std::vector<int>{12});
        CHECK_EQ(f.store.opStatus(swap), std::string("done"));
        CHECK(f.store.subjects(failed) == std::vector<int>{12});
        CHECK_EQ(f.store.opStatus(failed), std::string("failed"));
        CHECK(f.store.slotTimeline(7).empty());
        CHECK_EQ(f.store.slotTimeline(12).size(), 2u);
        CHECK_EQ(f.store.slotTimeline(9).size(), 1u);
        CHECK_EQ(f.scalar("SELECT count(*) FROM op_subjects WHERE slot = 7"), 0);
        CHECK_EQ(f.scalar("SELECT undo_floor FROM cards WHERE id = " + std::to_string(f.card)), swap);
        CHECK(!f.store.offeredTargets().undo); // the half-forgotten swap is the boundary, as before
        CHECK_EQ(f.scalar("SELECT count(*) FROM pragma_foreign_key_check"), 0);
        (void) nine;
    }
    // A subject with no state behind it moves no Undo boundary: the work
    // below stays undoable, and the operation is gone once its last subject is.
    {
        Fixture f;
        const auto nine = f.op({9});
        const auto failed = f.about({7, 12}, {}, "swap", OpStatus::failed);
        CHECK_EQ(f.store.slotTimeline(12).size(), 1u);
        CHECK_EQ(f.store.planForgetSlot(f.card, 7).rowsRemoved, 1);
        f.store.forgetSlot(f.card, 7, 40, true);
        CHECK(f.store.subjects(failed) == std::vector<int>{12});
        CHECK_EQ(f.store.opStatus(failed), std::string("failed"));
        CHECK_EQ(f.store.slotTimeline(12).size(), 1u); // still listed where it is still about
        CHECK_EQ(f.scalar("SELECT undo_floor FROM cards WHERE id = " + std::to_string(f.card)), 0);
        CHECK(f.store.offeredTargets().undo == nine);
        CHECK_EQ(f.store.planForgetSlot(f.card, 12).rowsRemoved, 1);
        f.store.forgetSlot(f.card, 12, 50, true);
        CHECK_THROWS(f.store.opStatus(failed), "no operation");
        CHECK_EQ(f.scalar("SELECT count(*) FROM op_subjects"), 0);
        CHECK(f.store.offeredTargets().undo == nine);
        CHECK_EQ(f.scalar("SELECT count(*) FROM pragma_foreign_key_check"), 0);
        // a rolled-back clearing leaves the subject in place with everything else
        const auto again = f.about({7}, {}, "normalize");
        f.store.db().exec("CREATE TRIGGER fail_forget BEFORE DELETE ON op_subjects BEGIN SELECT RAISE(ABORT, 'injected'); END");
        const auto before = f.bytes();
        CHECK_THROWS(f.store.forgetSlot(f.card, 7, 60, true), "injected");
        CHECK_EQ(f.bytes(), before);
        CHECK(f.store.subjects(again) == std::vector<int>{7});
        CHECK_EQ(f.store.slotTimeline(7).size(), 1u);
    }
    // A slot whose only entry is a no-op normalize that is the newest
    // operation: the dialog has no hold to confirm and no boundary to warn
    // about, because afterwards Undo is exactly what it was.
    {
        Fixture f;
        const auto nine = f.op({9});
        const auto nothing = f.about({7}, {}, "normalize");
        CHECK(f.store.offeredTargets().undo == nothing); // the cursor's answer today
        const auto plan = f.store.planForgetSlot(f.card, 7);
        CHECK_EQ(plan.rowsRemoved, 1);
        CHECK(plan.undoTargets.empty()); // no state to lose
        CHECK(!plan.cutsUndo);
        CHECK(!plan.hasHolds());
        CHECK(f.store.forgetSlot(f.card, 7, 40, false, &plan) == plan); // no separate confirmation
        CHECK_THROWS(f.store.opStatus(nothing), "no operation");
        CHECK_EQ(f.scalar("SELECT undo_floor FROM cards WHERE id = " + std::to_string(f.card)), 0);
        CHECK(f.store.offeredTargets().undo == nine);
        CHECK(f.store.slotTimeline(7).empty());
    }
    // A slot that is only a SUBJECT of an operation whose state lives
    // elsewhere: its name comes off the slot, and the operation stands as it
    // was — no boundary, still the Undo target, no hold to confirm.
    {
        Fixture f;
        const auto nine = f.op({9});
        const auto op = f.about({7, 12}, {12}, "rename");
        const auto settings = f.about({7}, {}, "controls");
        f.store.recordSystemChange(settings, {"CTL", "<CTL>before</CTL>", "<CTL>after</CTL>"});
        CHECK(f.store.offeredTargets().undo == settings);
        CHECK_EQ(f.store.slotTimeline(7).size(), 2u);
        const auto plan = f.store.planForgetSlot(f.card, 7);
        CHECK_EQ(plan.rowsRemoved, 2);
        CHECK(plan.undoTargets.empty());
        CHECK(!plan.cutsUndo);
        CHECK(!plan.hasHolds());
        CHECK(f.store.forgetSlot(f.card, 7, 40, false, &plan) == plan);
        CHECK_EQ(f.store.opStatus(op), std::string("done"));
        CHECK(f.store.subjects(op) == std::vector<int>{12});
        CHECK(f.store.touchedSlots(op) == std::vector<int>{12});
        CHECK_EQ(f.store.opStatus(settings), std::string("done"));
        CHECK(f.store.subjects(settings).empty());
        CHECK_EQ(f.store.systemChanges(settings).size(), 1u);
        CHECK_EQ(f.scalar("SELECT undo_floor FROM cards WHERE id = " + std::to_string(f.card)), 0);
        CHECK(f.store.offeredTargets().undo == settings);
        CHECK(f.store.slotTimeline(7).empty());
        CHECK_EQ(f.store.slotTimeline(12).size(), 1u);
        CHECK_EQ(f.scalar("SELECT count(*) FROM pragma_foreign_key_check"), 0);
        (void) nine;
    }
    // An operation that loses its last state here goes whole, the slots it
    // was still about included: a subject is not a surviving state, and the
    // row the other slot loses said "nothing changed". No boundary moves,
    // and the work below stays undoable — exactly as before subjects existed.
    {
        Fixture f;
        const auto nine = f.op({9});
        const auto op = f.about({7, 12}, {7}, "normalize");
        CHECK(f.store.offeredTargets().undo == op);
        CHECK_EQ(f.store.slotTimeline(12).size(), 1u);
        const auto plan = f.store.planForgetSlot(f.card, 7);
        CHECK(plan.undoTargets == std::vector<std::int64_t>{op}); // the undo on offer goes with it
        CHECK(!plan.cutsUndo); // no state of it survives anywhere
        CHECK_THROWS(f.store.forgetSlot(f.card, 7, 40), "separate confirmation");
        CHECK(f.store.forgetSlot(f.card, 7, 40, true) == plan);
        CHECK_THROWS(f.store.opStatus(op), "no operation");
        CHECK(f.store.subjects(op).empty());
        CHECK_EQ(f.scalar("SELECT count(*) FROM op_subjects"), 0);
        CHECK_EQ(f.scalar("SELECT undo_floor FROM cards WHERE id = " + std::to_string(f.card)), 0);
        CHECK(f.store.offeredTargets().undo == nine);
        CHECK(f.store.slotTimeline(12).empty());
        CHECK_EQ(f.scalar("SELECT count(*) FROM pragma_foreign_key_check"), 0);
    }
    // A pinned operation that loses only its badge here is not a protected
    // entry: its row, its state and its pin all stay. Three shapes — a
    // subject beside a body elsewhere, beside a settings change, beside
    // another subject. One whose only link was this slot's subject goes
    // whole, and that IS protected, as is one losing a state here.
    {
        Fixture f;
        const auto body = f.about({7, 12}, {12}, "rename");
        const auto settings = f.about({7}, {}, "controls");
        f.store.recordSystemChange(settings, {"CTL", "<CTL>before</CTL>", "<CTL>after</CTL>"});
        const auto twice = f.about({7, 12}, {}, "swap", OpStatus::failed);
        for (const auto op : {body, settings, twice}) f.store.pinOp(op, true);
        const auto plan = f.store.planForgetSlot(f.card, 7);
        CHECK_EQ(plan.rowsRemoved, 3);
        CHECK(plan.pinned.empty());
        CHECK(plan.undoTargets.empty());
        CHECK(!plan.cutsUndo);
        CHECK(!plan.hasHolds());
        CHECK(f.store.forgetSlot(f.card, 7, 40, false, &plan) == plan); // no separate confirmation
        for (const auto op : {body, settings, twice}) {
            CHECK_EQ(f.scalar("SELECT pinned FROM ops WHERE seq = " + std::to_string(op)), 1);
            CHECK_EQ(f.scalar("SELECT count(*) FROM ops WHERE seq = " + std::to_string(op)), 1);
        }
        CHECK(f.store.subjects(body) == std::vector<int>{12});
        CHECK(f.store.touchedSlots(body) == std::vector<int>{12});
        CHECK(f.store.subjects(settings).empty());
        CHECK_EQ(f.store.systemChanges(settings).size(), 1u);
        CHECK(f.store.subjects(twice) == std::vector<int>{12});
        CHECK(f.store.slotTimeline(7).empty());
        CHECK_EQ(f.store.slotTimeline(12).size(), 2u);
        CHECK_EQ(f.scalar("SELECT undo_floor FROM cards WHERE id = " + std::to_string(f.card)), 0);
        // the only link was this slot's subject: dropped whole, and protected
        const auto alone = f.about({7}, {}, "normalize");
        f.store.pinOp(alone, true);
        const auto second = f.store.planForgetSlot(f.card, 7);
        CHECK(second.pinned == std::vector<std::int64_t>{alone});
        CHECK(second.hasHolds());
        CHECK_THROWS(f.store.forgetSlot(f.card, 7, 50), "separate confirmation");
        f.store.forgetSlot(f.card, 7, 50, true);
        CHECK_THROWS(f.store.opStatus(alone), "no operation");
        // and one that loses a state here is protected, as it always was
        const auto stateful = f.about({7, 12}, {7, 12}, "swap");
        f.store.pinOp(stateful, true);
        CHECK(f.store.planForgetSlot(f.card, 7).pinned == std::vector<std::int64_t>{stateful});
        CHECK(f.store.planForgetSlot(f.card, 7).cutsUndo);
        CHECK_EQ(f.scalar("SELECT count(*) FROM pragma_foreign_key_check"), 0);
    }
    return testkit::summary("forget_slot_tests");
}

int main()
{
    // A suite that dies of an uncaught throw tells the reader nothing but an
    // exit code, and on Windows not even a message.
    try { return runTests(); }
    catch (const std::exception& error) {
        testkit::fail(error.what(), __FILE__, __LINE__);
        return testkit::summary("forget_slot_tests");
    }
}
