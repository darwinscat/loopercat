// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Undo and Redo over a timeline that is only appended to (#73), attacked
// from what the cursor and the plan promise:
//
//   - Cmd-Z targets the newest live operation; after an undo it moves one
//     back, and Cmd-Shift-Z targets the undo row that stands
//   - a step forward after an undo closes the way back; a redo reopens
//     nothing but what its undo took
//   - a legacy row, a failed row, a row still running are never targets; a
//     row cut off closes the way back like a step forward
//   - the plan puts back exactly what the rows say was there: the body
//     recorded, the take archived; a body-only change leaves the file
//     alone; a push into an empty slot goes back to empty; a swap swaps back
//   - it is refused by name: a take no longer kept, a state not recorded,
//     an operation that did not finish, nothing to put back
//   - crossing a change made on the pedal, or another session, is flagged,
//     and what the press writes over is listed

#include "support.hpp"

#include "../app/history/Undo.h"

#include <string>
#include <vector>

using namespace loopercat;
namespace undo = loopercat::history::undo;
using Card = loopercat::history::HistoryStore::CardEntry;
using Op = loopercat::history::HistoryStore::OpSummary;
using TakeRef = loopercat::history::HistoryStore::TakeRef;

namespace {

Op op(std::int64_t seq, const char* kind, const char* status = "done", const char* actor = "app",
      std::optional<std::int64_t> reverts = std::nullopt)
{
    Op o;
    o.op = seq;
    o.kind = kind;
    o.status = status;
    o.actor = actor;
    o.reverts = reverts;
    return o;
}

Card card(std::int64_t seq, const char* kind, std::int64_t session = 1, const char* status = "done",
          const char* actor = "app")
{
    Card c;
    c.op = seq;
    c.at = seq * 1000;
    c.kind = kind;
    c.status = status;
    c.actor = actor;
    c.session = session;
    return c;
}

Card::Slot slot(int number, std::optional<std::string> before, std::optional<std::string> after,
                std::optional<TakeRef> archived = std::nullopt, const std::string& takeName = {},
                bool newest = false)
{
    Card::Slot s;
    s.slot = number;
    s.newest = newest;
    s.facts.beforeBody = std::move(before);
    s.facts.afterBody = std::move(after);
    s.archived = std::move(archived);
    s.facts.takeName = takeName;
    if (!takeName.empty())
        s.facts.takeHash = std::string(32, '\x11');
    return s;
}

} // namespace

int main()
{
    // ---------------- the cursor ----------------

    // --- nothing happened: nothing to undo or redo ---
    {
        const auto t = undo::cursor({});
        CHECK(!t.undo && !t.redo && !t.redoRestores);
    }

    // --- Cmd-Z targets the newest live operation; after an undo, the one before ---
    {
        const auto t = undo::cursor({ op(1, "push"), op(2, "trim") });
        CHECK(t.undo == 2);
        CHECK(!t.redo);
        const auto after = undo::cursor({ op(1, "push"), op(2, "trim"), op(3, "undo", "done", "app", 2) });
        CHECK(after.undo == 1);     // one step back
        CHECK(after.redo == 3);     // reverting the undo row is the redo
        CHECK(after.redoRestores == 2);
        const auto twice = undo::cursor({ op(1, "push"), op(2, "trim"), op(3, "undo", "done", "app", 2),
                                          op(4, "undo", "done", "app", 1) });
        CHECK(!twice.undo);         // nothing live is left
        CHECK(twice.redo == 4);     // the last undo stands first in line
        CHECK(twice.redoRestores == 1);
    }

    // --- a redo brings the operation back; a step forward closes the way back ---
    {
        const auto redone = undo::cursor({ op(1, "push"), op(2, "trim"), op(3, "undo", "done", "app", 2),
                                           op(4, "redo", "done", "app", 3) });
        CHECK(redone.undo == 2);
        CHECK(!redone.redo);
        const auto branched = undo::cursor({ op(1, "push"), op(2, "trim"), op(3, "undo", "done", "app", 2),
                                             op(4, "rename") });
        CHECK(branched.undo == 4);
        CHECK(!branched.redo); // the trim cannot be redone over the rename
        // undo the rename: the trim is still gone, and only the rename comes back
        const auto again = undo::cursor({ op(1, "push"), op(2, "trim"), op(3, "undo", "done", "app", 2),
                                          op(4, "rename"), op(5, "undo", "done", "app", 4) });
        CHECK(again.undo == 1);
        CHECK(again.redo == 5);
        CHECK(again.redoRestores == 4);
    }

    // --- rows that are not targets ---
    {
        const auto t = undo::cursor({ op(1, "push"), op(2, "trim", "failed"), op(3, "clear", "pending"),
                                      op(500, "legacy", "done", "legacy") });
        CHECK(t.undo == 1);
        CHECK(!t.redo);
        // an operation cut off may have changed the card: no way back past it
        const auto cut = undo::cursor({ op(1, "push"), op(2, "trim"), op(3, "undo", "done", "app", 2),
                                        op(4, "clear", "interrupted") });
        CHECK(cut.undo == 1);
        CHECK(!cut.redo);
        // a failed undo took nothing
        const auto failed = undo::cursor({ op(1, "push"), op(2, "trim"), op(3, "undo", "failed", "app", 2) });
        CHECK(failed.undo == 2);
        CHECK(!failed.redo);
        // an undo naming nothing live is a row that stands and a cursor that ignores it
        const auto stray = undo::cursor({ op(1, "push"), op(3, "undo", "done", "app", 42) });
        CHECK(stray.undo == 1);
        CHECK(!stray.redo);
        // the pedal's own change is a step like any other
        const auto pedal = undo::cursor({ op(1, "push"), op(2, "push", "done", "pedal") });
        CHECK(pedal.undo == 2);
    }

    // ---------------- the plan ----------------
    const std::string empty = rc0::setSectionField(rc0::factorySlotBody(42), rc0::kSectionTrack1, "WavLen", 0);
    const std::string loaded = rc0::setSectionField(rc0::factorySlotBody(42), rc0::kSectionTrack1, "WavLen", 1719900);
    const std::string shorter = rc0::setSectionField(rc0::factorySlotBody(42), rc0::kSectionTrack1, "WavLen", 441000);

    // --- a trim goes back to the body and the take it archived ---
    {
        Card trim = card(2, "trim");
        trim.slots = { slot(12, loaded, shorter, TakeRef { "take.wav", std::string(32, '\x22'), true }, "take.wav", true) };
        const auto plan = undo::plan({ card(1, "push"), trim }, 2);
        CHECK(plan.possible());
        CHECK_EQ(plan.steps.size(), 1u);
        CHECK(plan.steps.front().slot == 12);
        CHECK(plan.steps.front().body == loaded);
        CHECK(!plan.steps.front().keepTake);
        CHECK(plan.steps.front().takeName == std::string("take.wav"));
        CHECK(plan.steps.front().takeHash == std::string(32, '\x22'));
        CHECK(!plan.swapBack);
        CHECK(plan.writesOver.empty());
        CHECK(!plan.crossesConnection && !plan.crossesPedal);
    }

    // --- the archived take is gone: refused by name, with the slot ---
    {
        Card trim = card(2, "trim");
        trim.slots = { slot(12, loaded, shorter, TakeRef { "take.wav", std::string(32, '\x22'), false }, "take.wav") };
        const auto plan = undo::plan({ trim }, 2);
        CHECK(!plan.possible());
        CHECK(plan.refusal == undo::Refusal::takeNotKept);
        CHECK_EQ(plan.refusedSlot, 12);
        CHECK(plan.reason.find("slot 12") != std::string::npos);
        CHECK(plan.reason.find("no longer kept") != std::string::npos);
        CHECK(plan.steps.empty());
    }

    // --- a body-only change leaves the file alone; a push into an empty slot goes back to empty ---
    {
        Card rename = card(3, "rename");
        rename.slots = { slot(12, loaded, loaded, std::nullopt, "take.wav") };
        const auto r = undo::plan({ rename }, 3);
        CHECK(r.possible());
        CHECK(r.steps.front().keepTake);
        CHECK(r.steps.front().body == loaded);
        CHECK(!r.steps.front().takeHash);

        Card push = card(4, "push");
        push.slots = { slot(7, empty, loaded, std::nullopt, "new.wav") };
        const auto p = undo::plan({ push }, 4);
        CHECK(p.possible());
        CHECK(!p.steps.front().keepTake);
        CHECK(!p.steps.front().takeName && !p.steps.front().takeHash); // the slot goes empty
        CHECK(p.steps.front().body == empty);

        // an audio-only operation that recorded no body and archived nothing: not knowable
        Card odd = card(5, "normalize");
        odd.slots = { slot(7, std::nullopt, std::nullopt, std::nullopt, "") };
        const auto o = undo::plan({ odd }, 5);
        CHECK(o.refusal == undo::Refusal::stateNotRecorded);
        CHECK_EQ(o.refusedSlot, 7);
        // a normalize that archived its take: the body stays, the take goes back
        Card norm = card(6, "normalize");
        norm.slots = { slot(7, std::nullopt, std::nullopt, TakeRef { "n.wav", std::string(32, '\x33'), true }, "n.wav") };
        const auto n = undo::plan({ norm }, 6);
        CHECK(n.possible());
        CHECK(!n.steps.front().body);
        CHECK(n.steps.front().takeHash == std::string(32, '\x33'));
    }

    // --- a swap goes back by swapping again ---
    {
        Card swap = card(8, "swap");
        swap.slots = { slot(12, loaded, shorter, std::nullopt, "a.wav"), slot(43, shorter, loaded, std::nullopt, "b.wav") };
        const auto plan = undo::plan({ swap }, 8);
        CHECK(plan.possible());
        CHECK(plan.swapBack == std::make_pair(12, 43));
        CHECK(plan.steps.empty());
    }

    // --- refusals by name ---
    {
        CHECK(undo::plan({ card(1, "push") }, 99).refusal == undo::Refusal::noSuchOperation);
        Card failed = card(2, "trim", 1, "failed");
        failed.slots = { slot(1, loaded, shorter) };
        CHECK(undo::plan({ failed }, 2).refusal == undo::Refusal::notFinished);
        Card legacy = card(500, "legacy", 1, "done", "legacy");
        legacy.slots = { slot(5, std::nullopt, std::nullopt, TakeRef { "005_1.WAV", std::string(32, '\x44'), true }, "005_1.WAV") };
        const auto l = undo::plan({ legacy }, 500);
        CHECK(l.refusal == undo::Refusal::nothingToPutBack);
        CHECK(l.reason.find("before the history") != std::string::npos);
        CHECK(undo::plan({ card(3, "clear") }, 3).refusal == undo::Refusal::nothingToPutBack);
        for (const auto& p : { undo::plan({ card(1, "push") }, 99), undo::plan({ failed }, 2), l })
            CHECK(!p.reason.empty());
    }

    // --- what the press crosses, and what it writes over ---
    {
        Card first = card(1, "push", 1);
        first.slots = { slot(12, empty, loaded, std::nullopt, "a.wav") };
        Card pedal = card(2, "push", 2, "done", "pedal"); // after a reconnect, the pedal itself
        pedal.slots = { slot(43, empty, loaded, std::nullopt, "p.wav") };
        Card later = card(3, "rename", 2);
        later.slots = { slot(12, loaded, loaded, std::nullopt, "a.wav") };
        Card elsewhere = card(4, "rename", 2);
        elsewhere.slots = { slot(7, empty, empty) };
        Card unfinished = card(5, "trim", 2, "failed");
        unfinished.slots = { slot(12, loaded, shorter) };

        const auto plan = undo::plan({ first, pedal, later, elsewhere, unfinished }, 1);
        CHECK(plan.possible());
        CHECK(plan.crossesPedal);
        CHECK(plan.crossesConnection);                  // session 1, the newest is in session 2
        CHECK(plan.writesOver == (std::vector<std::int64_t> { 3 })); // the rename on slot 12; not slot 7, not the failed trim
        // the newest operation crosses nothing
        const auto newest = undo::plan({ first, pedal, later, elsewhere }, 4);
        CHECK(!newest.crossesPedal && !newest.crossesConnection && newest.writesOver.empty());
        // same session, nothing from the pedal after it: no bump
        const auto quiet = undo::plan({ later, elsewhere }, 3);
        CHECK(!quiet.crossesPedal && !quiet.crossesConnection);
    }

    return testkit::summary("undo_tests");
}
