// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What the History tab may offer for each row (#50). The theory: a button is
// a promise. Play promises bytes to sound; Restore promises a state to put
// back. A row offers only what it can actually produce — which take it is
// about, and whether those bytes still exist, is the difference between a
// button that works and a button that lies.
//
// The cases below are the shapes a real store produces, including the two
// that only appear after a while: a take whose bytes were never kept (the
// slot was cleared with "Delete permanently" later), and a row imported from
// the folders that predate the store, which has audio but no state.

#include "support.hpp"

#include "../app/history/SlotRows.h"

#include <string>
#include <vector>

using namespace loopercat;
namespace rows = loopercat::history::rows;
using Entry = loopercat::history::HistoryStore::TimelineEntry;

namespace {

std::string bodyWith(long long wavLen, const std::string& name)
{
    std::string body = rc0::setSectionField(rc0::factorySlotBody(42), rc0::kSectionTrack1,
                                            "WavLen", wavLen);
    return rc0::setName(body, name);
}

Entry op(std::int64_t seq, const std::string& kind)
{
    Entry entry;
    entry.op = seq;
    entry.at = 1000 * seq;
    entry.kind = kind;
    entry.actor = "app";
    entry.status = "done";
    return entry;
}

} // namespace

int main()
{
    const std::string empty = bodyWith(0, "Memory42");
    const std::string loaded = bodyWith(1719900, "TEST_42_HIST");

    // --- the newest row is the state the slot is in now ---
    {
        Entry pushed = op(1, "push");
        pushed.beforeBody = empty;
        pushed.afterBody = loaded;
        pushed.takeName = "take.wav";
        pushed.takeHash = std::string(32, '\x11');
        pushed.takeKept = false; // the bytes are on the card; nothing replaced them yet

        const auto made = rows::forSlot({ pushed });
        CHECK_EQ(made.size(), 1u);
        CHECK_EQ(made.front().line.audio, std::string("in the slot now"));
        CHECK(!made.front().playable);   // the Audio tab already plays that take
        CHECK(!made.front().restorable); // and the slot is in that state already
        CHECK(made.front().takeHash.empty());
    }

    // --- an older row whose take the store kept: both offers stand ---
    {
        Entry pushed = op(1, "push");
        pushed.beforeBody = empty;
        pushed.afterBody = loaded;
        pushed.takeName = "take.wav";
        pushed.takeHash = std::string(32, '\x11');
        pushed.takeKept = true;
        Entry trimmed = op(2, "trim");
        trimmed.beforeBody = loaded;
        trimmed.afterBody = bodyWith(441000, "TEST_42_HIST");
        trimmed.takeName = "take.wav";
        trimmed.takeHash = std::string(32, '\x22');

        const auto made = rows::forSlot({ pushed, trimmed });
        CHECK_EQ(made.size(), 2u);
        CHECK_EQ(made.front().line.audio, std::string("take kept"));
        CHECK(made.front().playable);
        CHECK(made.front().restorable);
        CHECK_EQ(made.front().takeHash, std::string(32, '\x11')); // what Play would sound
        CHECK_EQ(made.back().line.audio, std::string("in the slot now"));
    }

    // --- a take whose bytes are gone offers neither ---
    {
        // what Alisa's card produced: a push, then a clear that kept nothing,
        // so the pushed take exists in no store and on no card
        Entry pushed = op(1, "push");
        pushed.beforeBody = empty;
        pushed.afterBody = loaded;
        pushed.takeName = "take.wav";
        pushed.takeHash = std::string(32, '\x33');
        pushed.takeKept = false;
        Entry cleared = op(2, "clear");
        cleared.beforeBody = loaded;
        cleared.afterBody = empty;

        const auto made = rows::forSlot({ pushed, cleared });
        CHECK_EQ(made.front().line.audio, std::string("take no longer kept"));
        CHECK(!made.front().playable);
        CHECK(!made.front().restorable); // a state whose take cannot be produced
        CHECK(made.front().takeHash.empty());
        // the clear itself put the slot back to empty, and that state stands
        CHECK_EQ(made.back().line.audio, std::string("nothing left in the slot"));
        CHECK(!made.back().restorable); // the newest row IS where the slot is
        CHECK(!made.back().playable);
    }

    // --- a row from the folders that predate the store ---
    {
        // the import records the take it found and no state at all: there is
        // something to listen to, and nothing to go back to
        Entry legacy = op(1, "legacy");
        legacy.actor = "legacy";
        legacy.note = "trash/2026-09-01T21-35-46";
        legacy.takeName = "005_1.WAV";
        legacy.takeHash = std::string(32, '\x44');
        legacy.takeKept = true;
        Entry later = op(2, "rename");
        later.beforeBody = empty;
        later.afterBody = bodyWith(0, "Named");

        const auto made = rows::forSlot({ legacy, later });
        CHECK_EQ(made.front().line.action, std::string("legacy")); // its own name, not a guess
        CHECK_EQ(made.front().line.detail, std::string("trash/2026-09-01T21-35-46"));
        CHECK(made.front().playable);
        CHECK(!made.front().restorable);
        CHECK_EQ(made.front().takeHash, std::string(32, '\x44'));
    }

    // --- rows that never held audio say nothing about it ---
    {
        Entry renamed = op(1, "rename");
        renamed.beforeBody = empty;
        renamed.afterBody = bodyWith(0, "Named");
        Entry later = op(2, "tempo");
        later.beforeBody = bodyWith(0, "Named");
        later.afterBody = bodyWith(0, "Named");
        const auto made = rows::forSlot({ renamed, later });
        CHECK_EQ(made.front().line.audio, std::string());
        CHECK(!made.front().playable);
        CHECK(made.front().restorable); // an empty slot is a state like any other
    }

    // --- an empty slot has an empty timeline, and no offers to make ---
    {
        CHECK(rows::forSlot({}).empty());
    }

    return testkit::summary("slot_rows_tests");
}
