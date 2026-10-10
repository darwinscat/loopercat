// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What the History tab may offer for each row (#50). The theory: a button is
// a promise. Play promises bytes to sound; Restore promises a state to put
// back. A row offers only what it can actually produce — which take it is
// about, and whether those bytes still exist, is the difference between a
// button that works and a button that lies.
//
// The cases below are the shapes a real store produces, including a take
// whose bytes were released, a row that archived audio without changing
// the slot body, and a row that only names the slot — an operation that
// was about it and changed nothing there (#144).

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
        pushed.takeIsAfter = true; // the take the push left in the slot

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
        trimmed.takeIsAfter = true;

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

    // --- a newest row whose take is the one it archived is not where the slot is ---
    {
        // Alisa's run: push b.wav into slot 4, then Undo. The undo archived
        // b.wav and left the slot empty; the store keeps no 'after' take for
        // it, so the row's take is the archived one — not on the card.
        Entry pushed = op(1, "push");
        pushed.beforeBody = empty;
        pushed.afterBody = loaded;
        pushed.takeName = "b.wav";
        pushed.takeHash = std::string(32, '\x0b');
        pushed.takeKept = true;
        pushed.takeIsAfter = true;
        Entry undone = op(2, "undo");
        undone.note = "push";
        undone.beforeBody = loaded;
        undone.afterBody = empty;
        undone.takeName = "b.wav";
        undone.takeHash = std::string(32, '\x0b');
        undone.takeKept = true;
        undone.takeIsAfter = false; // the archived 'before' side
        const auto made = rows::forSlot({ pushed, undone });
        CHECK_EQ(made.back().line.audio, std::string("take kept")); // not "in the slot now"
        CHECK(made.back().line.audio.find("in the slot now") == std::string::npos);
        CHECK(made.back().playable); // the kept take can still be heard
        CHECK_EQ(made.back().takeHash, std::string(32, '\x0b'));
        CHECK_EQ(made.back().line.action, std::string("Undid push"));

        // the newest clear, the same way; lost once its bytes are gone
        Entry cleared = op(3, "clear");
        cleared.beforeBody = loaded;
        cleared.afterBody = empty;
        cleared.takeName = "b.wav";
        cleared.takeHash = std::string(32, '\x0b');
        cleared.takeKept = true;
        CHECK_EQ(rows::forSlot({ pushed, cleared }).back().line.audio, std::string("take kept"));
        cleared.takeKept = false;
        CHECK_EQ(rows::forSlot({ pushed, cleared }).back().line.audio, std::string("take no longer kept"));

        // a newest normalize — no bodies, a take it left — is still where the slot is
        Entry normalized = op(4, "normalize");
        normalized.note = "normalized -3.2 dB";
        normalized.takeName = "b.wav";
        normalized.takeHash = std::string(32, '\x0c');
        normalized.takeIsAfter = true;
        CHECK_EQ(rows::forSlot({ pushed, normalized }).back().line.audio, std::string("in the slot now"));
        CHECK(!rows::forSlot({ pushed, normalized }).back().playable); // on the card: the Audio tab plays it
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

    // --- a row that only names the slot leaves the state where it was (#144) ---
    {
        Entry pushed = op(1, "push");
        pushed.beforeBody = empty;
        pushed.afterBody = loaded;
        pushed.takeName = "take.wav";
        pushed.takeHash = std::string(32, '\x11');
        pushed.takeIsAfter = true;
        Entry nothing = op(2, "normalize");
        nothing.note = "already at -18.0 LUFS (measured -18.1), nothing to do";
        nothing.subjectOnly = true;

        const auto made = rows::forSlot({ pushed, nothing });
        CHECK_EQ(made.size(), 2u);
        CHECK_EQ(made.front().line.audio, std::string("in the slot now")); // the push is still where the slot is
        CHECK(!made.front().restorable);
        CHECK(!made.front().playable);
        CHECK_EQ(made.back().line.action, std::string("Normalized"));
        CHECK_EQ(made.back().line.detail, nothing.note);
        CHECK_EQ(made.back().line.audio, std::string()); // never "in the slot now"
        CHECK(!made.back().playable);
        CHECK(!made.back().restorable);
        CHECK(made.back().takeHash.empty());

        // alone in the slot's timeline it is still no state of the slot
        const auto alone = rows::forSlot({ nothing });
        CHECK_EQ(alone.size(), 1u);
        CHECK(!alone.front().restorable && !alone.front().playable);
        CHECK_EQ(alone.front().line.audio, std::string());

        // a state recorded after it is the newest again; the row between stays what it is
        Entry trimmed = op(3, "trim");
        trimmed.beforeBody = loaded;
        trimmed.afterBody = bodyWith(441000, "TEST_42_HIST");
        trimmed.takeName = "take.wav";
        trimmed.takeHash = std::string(32, '\x22');
        trimmed.takeIsAfter = true;
        const auto later = rows::forSlot({ pushed, nothing, trimmed });
        CHECK_EQ(later.size(), 3u);
        CHECK_EQ(later[2].line.audio, std::string("in the slot now"));
        CHECK(later[0].line.audio != "in the slot now");
        CHECK_EQ(later[1].line.audio, std::string());
        CHECK(!later[1].restorable);

        // a row that names the slot and claims a state in it is not the store's
        Entry lying = op(4, "normalize");
        lying.subjectOnly = true;
        lying.afterBody = loaded;
        CHECK_THROWS(rows::forSlot({ lying }), "subject");
        Entry lyingTake = op(5, "normalize");
        lyingTake.subjectOnly = true;
        lyingTake.takeName = "take.wav";
        CHECK_THROWS(rows::forSlot({ lyingTake }), "subject");
    }

    // --- a row that did not finish says so, and claims no change (P1 of #144's review) ---
    {
        // A push that could not read its source, a tempo refused as out of
        // range, a One Shot ON attempt that failed: each is a row of the slot
        // (its subject), and none of them happened. The window says "failed"
        // beside such a row; the tab must say the same, and the words must
        // not invent what the write would have left.
        Entry push = op(1, "push");
        push.status = "failed";
        push.note = "cannot read a.wav";
        push.subjectOnly = true;
        Entry tempo = op(2, "tempo");
        tempo.status = "failed";
        tempo.note = "400.0 BPM is out of the pedal's range";
        tempo.subjectOnly = true;
        Entry oneshot = op(3, "oneshot");
        oneshot.status = "failed";
        oneshot.note = "cannot write MEMORY1.RC0";
        oneshot.subjectOnly = true;
        const auto made = rows::forSlot({ push, tempo, oneshot });
        CHECK_EQ(made.size(), 3u);
        for (const auto& row : made) {
            CHECK_EQ(row.state, std::string("failed"));
            CHECK(!row.playable && !row.restorable);
            CHECK_EQ(row.line.audio, std::string());
            CHECK(row.line.action.find(" on") == std::string::npos);
            CHECK(row.line.action.find(" off") == std::string::npos);
        }
        CHECK_EQ(made[0].line.detail, push.note);
        CHECK_EQ(made[1].line.detail, tempo.note);
        CHECK(made[1].line.detail.find("->") == std::string::npos); // no numbers it never wrote
        CHECK_EQ(made[2].line.action, std::string("One Shot")); // not "on", not "off"
        CHECK_EQ(made[2].line.detail, oneshot.note);

        // A row that recorded bodies on the way and then failed or was cut
        // off: the reason outranks its numbers, and the state says it.
        Entry cutTrim = op(4, "trim");
        cutTrim.status = "interrupted";
        cutTrim.note = "the card went away";
        cutTrim.beforeBody = loaded;
        cutTrim.afterBody = bodyWith(441000, "TEST_42_HIST");
        Entry failedTempo = op(5, "tempo");
        failedTempo.status = "failed";
        failedTempo.note = "cannot write MEMORY2.RC0";
        failedTempo.beforeBody = loaded;
        failedTempo.afterBody = bodyWith(1719900, "TEST_42_HIST");
        const auto cut = rows::forSlot({ cutTrim, failedTempo });
        CHECK_EQ(cut[0].state, std::string("interrupted"));
        CHECK_EQ(cut[0].line.detail, cutTrim.note);
        CHECK_EQ(cut[1].state, std::string("failed"));
        CHECK_EQ(cut[1].line.detail, failedTempo.note);
        // the state a cut-off write recorded can be offered back like any
        // other (it is not where the slot is); the last state row is the
        // slot's place, whatever its outcome, so it is not somewhere to go back to
        CHECK(cut[0].restorable);
        CHECK(!cut[1].restorable);

        // an interrupted row without a reason keeps the numbers it recorded, and the state
        Entry silent = op(6, "trim");
        silent.status = "interrupted";
        silent.beforeBody = loaded;
        silent.afterBody = bodyWith(441000, "TEST_42_HIST");
        CHECK_EQ(rows::forSlot({ silent }).front().state, std::string("interrupted"));
        CHECK_EQ(rows::forSlot({ silent }).front().line.detail, std::string("0:39 -> 0:10"));

        // a pending row and a pedal's row wear the window's words too; a plain finished one wears none
        Entry running = op(7, "normalize");
        running.status = "pending";
        running.subjectOnly = true;
        CHECK_EQ(rows::forSlot({ running }).front().state, std::string("still running"));
        Entry pedal = op(8, "push");
        pedal.actor = "pedal";
        pedal.beforeBody = empty;
        pedal.afterBody = loaded;
        CHECK_EQ(rows::forSlot({ pedal }).front().state, std::string("recorded on the pedal"));
        Entry plain = op(9, "rename");
        plain.beforeBody = loaded;
        plain.afterBody = bodyWith(1719900, "Kitty");
        CHECK_EQ(rows::forSlot({ plain }).front().state, std::string());
    }

    // --- a finished operation about the slot with nothing to say did nothing: say so ---
    {
        Entry rename = op(1, "rename");
        rename.subjectOnly = true;
        Entry tempo = op(2, "tempo");
        tempo.subjectOnly = true;
        Entry oneshot = op(3, "oneshot");
        oneshot.subjectOnly = true;
        Entry normalize = op(4, "normalize");
        normalize.subjectOnly = true;
        normalize.note = "already at -18.0 LUFS (measured -18.1), nothing to do";
        const auto made = rows::forSlot({ rename, tempo, oneshot, normalize });
        CHECK_EQ(made[0].line.action, std::string("Renamed"));
        CHECK_EQ(made[0].line.detail, std::string("nothing changed"));
        CHECK_EQ(made[1].line.action, std::string("Tempo"));
        CHECK_EQ(made[1].line.detail, std::string("nothing changed"));
        CHECK_EQ(made[2].line.action, std::string("One Shot")); // no switch position it never set
        CHECK_EQ(made[2].line.detail, std::string("nothing changed"));
        CHECK_EQ(made[3].line.detail, normalize.note); // the note, when there is one
        for (const auto& row : made) {
            CHECK_EQ(row.state, std::string());
            CHECK(!row.restorable && !row.playable);
        }
        // a touched row with no detail is not "nothing changed": it recorded a state
        Entry touched = op(5, "rename");
        touched.beforeBody = loaded;
        touched.afterBody = loaded;
        CHECK_EQ(rows::forSlot({ touched }).front().line.detail, std::string("TEST_42_HIST -> TEST_42_HIST"));
    }

    // --- an empty slot has an empty timeline, and no offers to make ---
    {
        CHECK(rows::forSlot({}).empty());
    }

    return testkit::summary("slot_rows_tests");
}
