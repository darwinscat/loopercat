// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "support.hpp"
#include "../app/ClearSlotHistoryAction.h"
#include "../app/history/ForgetSlotJob.h"
#include "../app/history/SlotRows.h"
#include <chrono>
#include <filesystem>
using namespace loopercat;
namespace fs = std::filesystem;
int main()
{
    const auto dir = fs::temp_directory_path() / ("loopercat-clear-action-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        auto recorder = std::make_shared<history::HistoryRecorder>(dir, [] { return 100; });
        auto& store = recorder->store();
        const auto card = store.card("a", "RC-5", "A", 1);
        const auto session = store.openSession(card, 2);
        const auto op = store.beginOp(session, "test", "rename", 3);
        store.recordBodies(op, {{4, "before", "after"}});
        store.keepAudio(op, 4, 1, "take.wav", "bytes", 3);
        store.finishOp(op, history::OpStatus::done, "");
        store.pinOp(op, true);
        auto plan = store.planForgetSlot(card, 4);
        const auto before = commands::readFileBytes(dir / "history.db");
        for (int cancelAt : {1, 2}) {
            int asked = 0;
            int completed = 0;
            clearhistory::ask(4, plan, [&](clearhistory::Question q, clearhistory::Answer answer) {
                ++asked;
                CHECK_EQ(q.holds, asked == 2);
                CHECK(q.message.find("This cannot be undone.") != std::string::npos);
                CHECK(q.message.find("It includes 1 pinned entries") != std::string::npos);
                CHECK(q.message.find("Undo will no longer be able to go back past this point.") != std::string::npos);
                answer(asked != cancelAt);
            }, [&](bool accepted) { CHECK(!accepted); ++completed; });
            CHECK_EQ(asked, cancelAt);
            CHECK_EQ(completed, 1);
            CHECK_EQ(commands::readFileBytes(dir / "history.db"), before);
        }
        int questions = 0;
        bool accepted = false;
        clearhistory::ask(4, plan, [&](clearhistory::Question, clearhistory::Answer answer) {
            ++questions; answer(true);
        }, [&](bool yes) { accepted = yes; });
        CHECK(accepted); CHECK_EQ(questions, 2);
        int refreshed = 0;
        auto job = history::forgetSlotJob(recorder, card, 4, plan, true, [&] { ++refreshed; });
        CHECK(!job.needsVolume); CHECK(!job.background); CHECK(!job.quiet);
        job.before({}); job.work({}); job.after("");
        CHECK_EQ(refreshed, 1);
        CHECK(store.slotTimeline(4).empty());
        CHECK(!store.offeredTargets().undo); CHECK(!store.offeredTargets().redo);
        CHECK_EQ(store.cardTimeline().size(), 1u);
        CHECK_EQ(store.cardTimeline().front().kind, std::string("forget-history"));
        CHECK_EQ(store.cardTimeline().front().status, std::string("done"));
        CHECK_EQ(store.cardTimeline().front().note, std::string("Slot 4: 1 entries, 1 takes, 5 bytes freed"));
        const auto rows = history::rows::forCard(store.cardTimeline());
        CHECK_EQ(rows.front().action, std::string("Cleared slot history"));
        CHECK_EQ(rows.front().detail, std::string("Slot 4: 1 entries, 1 takes, 5 bytes freed"));
        CHECK(!rows.front().restorable());
        CHECK(rows.front().slots().empty());
        CHECK(!store.takeBytes(history::HistoryStore::contentHash("bytes")));
        questions = 0;
        clearhistory::ask(4, store.planForgetSlot(card, 4),
            [&](clearhistory::Question, clearhistory::Answer) { ++questions; },
            [&](bool yes) { CHECK(!yes); });
        CHECK_EQ(questions, 0);
        plan = {}; plan.rowsRemoved = 3; plan.takesFreed = 2; plan.bytesFreed = 1572864;
        const auto q = clearhistory::question(4, plan);
        CHECK_EQ(q.title, std::string("Clear the history of slot 4?"));
        CHECK_EQ(q.message, std::string("This cannot be undone. 3 entries and 2 takes (1.5 MB) will be deleted."));
        clearhistory::ask(4, plan, [&](clearhistory::Question question, clearhistory::Answer answer) {
            ++questions; CHECK(!question.holds); answer(true);
        }, [&](bool yes) { CHECK(yes); });
        CHECK_EQ(questions, 1);
        plan.cutsUndo = true;
        CHECK(clearhistory::question(4, plan).message.find(
            "Undo will no longer be able to go back past this point.") != std::string::npos);
        CHECK(!plan.hasHolds());
    }
    fs::remove_all(dir);
    return testkit::summary("clear_history_action_tests");
}
