// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What the History window may offer for each row of the whole card (#73),
// over the same rule and the same words as the slot's rows. The theory: a
// row is one operation; a swap is one row with two slots; a button is a
// promise the row can keep. The cases are the shapes a real store produces:
// a take whose bytes are gone, an audio-only row, a
// failed operation, an operation that touched nothing — and one that was
// about a slot and changed nothing there (#144): a badge, no offer.

#include "support.hpp"

#include "../app/history/SlotRows.h"

#include <string>
#include <vector>

using namespace loopercat;
namespace rows = loopercat::history::rows;
using Entry = loopercat::history::HistoryStore::TimelineEntry;
using Card = loopercat::history::HistoryStore::CardEntry;

namespace {

std::string bodyWith(long long wavLen, const std::string& name)
{
    std::string body = rc0::setSectionField(rc0::factorySlotBody(42), rc0::kSectionTrack1,
                                            "WavLen", wavLen);
    return rc0::setName(body, name);
}

Card op(std::int64_t seq, std::int64_t at, const std::string& kind, const std::string& actor = "app",
        const std::string& status = "done")
{
    Card entry;
    entry.op = seq;
    entry.at = at;
    entry.kind = kind;
    entry.actor = actor;
    entry.status = status;
    return entry;
}

Card::Slot touched(const Card& card, int slot, bool newest)
{
    Card::Slot out;
    out.slot = slot;
    out.newest = newest;
    out.facts.op = card.op;
    out.facts.at = card.at;
    out.facts.kind = card.kind;
    out.facts.actor = card.actor;
    out.facts.status = card.status;
    out.facts.note = card.note;
    return out;
}

} // namespace

int main()
{
    const std::string empty = bodyWith(0, "Memory42");
    const std::string loaded = bodyWith(1719900, "TEST_42_HIST");
    const std::string shorter = bodyWith(441000, "TEST_42_HIST");

    // --- a swap is one row with two slots, and the same words the tab uses ---
    {
        Card swap = op(3, 3000, "swap");
        Card::Slot a = touched(swap, 12, true);
        a.facts.beforeBody = loaded;
        a.facts.afterBody = shorter;
        a.facts.swappedWith = 43;
        a.facts.takeName = "a.wav";
        a.facts.takeHash = std::string(32, '\x11');
        a.facts.takeIsAfter = true;
        Card::Slot b = touched(swap, 43, true);
        b.facts.beforeBody = shorter;
        b.facts.afterBody = loaded;
        b.facts.swappedWith = 12;
        b.facts.takeName = "b.wav";
        b.facts.takeHash = std::string(32, '\x22');
        b.facts.takeIsAfter = true;
        swap.slots = { a, b };

        const auto made = rows::forCard({ swap });
        CHECK_EQ(made.size(), 1u);
        CHECK_EQ(made.front().action, std::string("Swapped slots 12 and 43"));
        CHECK_EQ(made.front().detail, std::string()); // one slot's numbers would speak for both
        CHECK(made.front().slots() == (std::vector<int> { 12, 43 }));
        CHECK_EQ(made.front().takes.size(), 2u);
        CHECK_EQ(made.front().takes[0].audio, std::string("in the slot now"));
        CHECK_EQ(made.front().takes[1].audio, std::string("in the slot now"));
        CHECK(!made.front().playable());   // both takes are on the card, not in the store
        CHECK(!made.front().restorable()); // and both slots are in that state already
        CHECK_EQ(made.front().state, std::string());
    }

    // --- an older row whose take the store kept: Play, Export and Restore stand ---
    {
        Card pushed = op(1, 1000, "push");
        Card::Slot slot = touched(pushed, 12, false);
        slot.facts.beforeBody = empty;
        slot.facts.afterBody = loaded;
        slot.facts.takeName = "take.wav";
        slot.facts.takeHash = std::string(32, '\x11');
        slot.facts.takeKept = true;
        pushed.slots = { slot };

        const auto made = rows::forCard({ pushed });
        CHECK_EQ(made.front().action, std::string("Pushed"));
        CHECK(made.front().detail.find("take.wav") != std::string::npos);
        CHECK(made.front().playable());
        CHECK(made.front().restorable());
        CHECK_EQ(made.front().takeHash(), std::string(32, '\x11'));
        CHECK_EQ(made.front().takes.front().audio, std::string("take kept"));
    }

    // --- a take whose bytes are gone is not offered to Play, nor its state to Restore ---
    {
        Card pushed = op(1, 1000, "push");
        Card::Slot slot = touched(pushed, 12, false);
        slot.facts.beforeBody = empty;
        slot.facts.afterBody = loaded;
        slot.facts.takeName = "take.wav";
        slot.facts.takeHash = std::string(32, '\x33');
        slot.facts.takeKept = false; // released, or never kept
        pushed.slots = { slot };

        const auto made = rows::forCard({ pushed });
        CHECK_EQ(made.front().takes.front().audio, std::string("take no longer kept"));
        CHECK(!made.front().playable());
        CHECK(!made.front().restorable());
        CHECK_EQ(made.front().takeHash(), std::string());
    }

    // --- the operation's own status and origin are said, plainly ---
    {
        Card failed = op(7, 7000, "trim", "app", "failed");
        failed.note = "cannot write MEMORY1.RC0";
        Card interrupted = op(8, 8000, "clear", "app", "interrupted");
        Card pedal = op(9, 9000, "push", "pedal");
        Card running = op(10, 10000, "normalize", "app", "pending");
        const auto made = rows::forCard({ failed, interrupted, pedal, running });
        CHECK_EQ(made[0].state, std::string("failed"));
        CHECK_EQ(made[0].detail, std::string("cannot write MEMORY1.RC0"));
        CHECK_EQ(made[1].state, std::string("interrupted"));
        CHECK_EQ(made[2].state, std::string("recorded on the pedal"));
        CHECK_EQ(made[3].state, std::string("still running"));
        for (const auto& row : made) {
            CHECK(!row.playable());
            CHECK(!row.restorable());
        }
        Card odd = op(12, 12000, "teleport");
        odd.note = "far";
        CHECK_EQ(rows::forCard({ odd }).front().action, std::string("teleport"));
        CHECK_EQ(rows::forCard({ odd }).front().detail, std::string("far"));
    }

    // --- Restore needs every recorded state to go back; Play needs one kept take ---
    {
        Card two = op(4, 4000, "restore");
        Card::Slot a = touched(two, 1, false);
        a.facts.beforeBody = loaded;
        a.facts.afterBody = shorter;
        a.facts.takeName = "a.wav";
        a.facts.takeHash = std::string(32, '\x11');
        a.facts.takeKept = true;
        Card::Slot b = touched(two, 2, false);
        b.facts.beforeBody = loaded;
        b.facts.afterBody = shorter;
        b.facts.takeName = "b.wav";
        b.facts.takeHash = std::string(32, '\x22');
        b.facts.takeKept = false; // this one's bytes are gone
        two.slots = { a, b };
        const auto made = rows::forCard({ two });
        CHECK(made.front().playable());        // a.wav can be heard
        CHECK(!made.front().restorable());     // but the pair cannot go back whole
        CHECK_EQ(made.front().takeHash(), std::string(32, '\x11'));
        CHECK_EQ(made.front().takes[1].audio, std::string("take no longer kept"));
    }

    // --- the rule is the tab's rule: a card row and a slot row agree ---
    {
        Card pushed = op(1, 1000, "push");
        Card::Slot slot = touched(pushed, 12, false);
        slot.facts.beforeBody = empty;
        slot.facts.afterBody = loaded;
        slot.facts.takeName = "take.wav";
        slot.facts.takeHash = std::string(32, '\x11');
        slot.facts.takeKept = true;
        Card trimmed = op(2, 2000, "trim");
        Card::Slot later = touched(trimmed, 12, true);
        later.facts.beforeBody = loaded;
        later.facts.afterBody = shorter;
        later.facts.takeName = "take.wav";
        later.facts.takeHash = std::string(32, '\x22');
        pushed.slots = { slot };
        trimmed.slots = { later };

        const auto card = rows::forCard({ pushed, trimmed });
        const auto tab = rows::forSlot({ slot.facts, later.facts });
        CHECK_EQ(card.size(), tab.size());
        for (std::size_t i = 0; i < card.size(); ++i) {
            CHECK_EQ(card[i].action, tab[i].line.action);
            CHECK_EQ(card[i].detail, tab[i].line.detail);
            CHECK_EQ(card[i].takes.front().audio, tab[i].line.audio);
            CHECK_EQ(card[i].playable(), tab[i].playable);
            CHECK_EQ(card[i].restorable(), tab[i].restorable);
            CHECK_EQ(card[i].takeHash(), tab[i].takeHash);
        }
    }

    // --- a settings change: no slots, the section's words, the fields that moved ---
    {
        Card controls = op(30, 30000, "controls");
        controls.system = { { "CTL", "<CTL>\n\t<Ctl1>17</Ctl1>\n\t<Ctl2>18</Ctl2>\n\t<Cc80>0</Cc80>\n</CTL>",
                                     "<CTL>\n\t<Ctl1>17</Ctl1>\n\t<Ctl2>22</Ctl2>\n\t<Cc80>1</Cc80>\n</CTL>" } };
        const auto made = rows::forCard({ controls });
        CHECK_EQ(made.size(), 1u);
        CHECK_EQ(made.front().action, std::string("Pedal controls changed"));
        CHECK_EQ(made.front().detail, std::string("Ctl2 18 -> 22, Cc80 0 -> 1"));
        CHECK(made.front().slots().empty());
        CHECK(!made.front().playable() && !made.front().restorable());
        Card midi = op(31, 31000, "midi");
        midi.system = { { "MIDI", "<MIDI>\n\t<RxCh>1</RxCh>\n</MIDI>", "<MIDI>\n\t<RxCh>2</RxCh>\n</MIDI>" } };
        CHECK_EQ(rows::forCard({ midi }).front().action, std::string("Pedal settings changed"));
        CHECK_EQ(rows::forCard({ midi }).front().detail, std::string("RxCh 1 -> 2"));
    }

    // --- an operation about a slot it did not change: its badge, its words, nothing to offer (#144) ---
    {
        Card nothing = op(5, 5000, "normalize");
        nothing.note = "already at -18.0 LUFS (measured -18.1), nothing to do";
        nothing.subjects = { 7 };
        const auto made = rows::forCard({ nothing });
        CHECK_EQ(made.size(), 1u);
        CHECK(made.front().slots() == (std::vector<int> { 7 }));
        CHECK(made.front().takes.empty()); // a subject is no take
        CHECK_EQ(made.front().action, std::string("Normalized"));
        CHECK_EQ(made.front().detail, nothing.note);
        CHECK_EQ(made.front().state, std::string());
        CHECK(!made.front().playable());
        CHECK(!made.front().restorable());
        CHECK(made.front().restorableSlots().empty());
        CHECK_EQ(made.front().takeHash(), std::string());
        // a failed one, about its slot, still wears the badge and says why
        Card failed = op(6, 6000, "normalize", "app", "failed");
        failed.note = "cannot read 007_1.WAV";
        failed.subjects = { 7 };
        CHECK(rows::forCard({ failed }).front().slots() == (std::vector<int> { 7 }));
        CHECK_EQ(rows::forCard({ failed }).front().state, std::string("failed"));
        CHECK_EQ(rows::forCard({ failed }).front().detail, failed.note);
    }

    // --- a swap about the two slots it changed wears two badges, not four ---
    {
        Card swap = op(3, 3000, "swap");
        Card::Slot a = touched(swap, 12, true);
        a.facts.beforeBody = loaded;
        a.facts.afterBody = shorter;
        a.facts.swappedWith = 43;
        Card::Slot b = touched(swap, 43, true);
        b.facts.beforeBody = shorter;
        b.facts.afterBody = loaded;
        b.facts.swappedWith = 12;
        swap.slots = { a, b };
        swap.subjects = { 12, 43 };
        const auto made = rows::forCard({ swap });
        CHECK(made.front().slots() == (std::vector<int> { 12, 43 }));
        CHECK_EQ(made.front().takes.size(), 2u);
        CHECK_EQ(made.front().action, std::string("Swapped slots 12 and 43"));
        // a subject beside a touched slot sorts into its place, whichever is lower
        Card mixed = op(4, 4000, "restore");
        Card::Slot c = touched(mixed, 12, false);
        c.facts.beforeBody = loaded;
        c.facts.afterBody = shorter;
        mixed.slots = { c };
        mixed.subjects = { 43 };
        CHECK(rows::forCard({ mixed }).front().slots() == (std::vector<int> { 12, 43 }));
        mixed.subjects = { 7 };
        CHECK(rows::forCard({ mixed }).front().slots() == (std::vector<int> { 7, 12 }));
        CHECK(rows::forCard({ mixed }).front().restorableSlots() == (std::vector<int> { 12 }));
        CHECK(rows::forCard({ mixed }).front().restorable()); // the one state it recorded can go back
    }

    // --- badges are what the row is about; a restore acts on what it recorded ---
    {
        // A row touching {3, 40} and about {7, 12, 40}: four badges, two
        // slots to put back — never "Restore 4 slots".
        Card two = op(8, 8000, "restore");
        Card::Slot a = touched(two, 40, false);
        a.facts.beforeBody = loaded;
        a.facts.afterBody = shorter;
        Card::Slot b = touched(two, 3, false);
        b.facts.beforeBody = loaded;
        b.facts.afterBody = shorter;
        two.slots = { b, a };
        two.subjects = { 7, 12, 40 };
        const auto made = rows::forCard({ two });
        CHECK(made.front().slots() == (std::vector<int> { 3, 7, 12, 40 }));
        CHECK(made.front().touchedSlots() == (std::vector<int> { 3, 40 }));
        CHECK(made.front().restorableSlots() == (std::vector<int> { 3, 40 }));
        CHECK(made.front().restorable());
        // a subject-only row has badges and nothing to act on
        Card nothing = op(9, 9000, "normalize");
        nothing.subjects = { 7 };
        CHECK(rows::forCard({ nothing }).front().slots() == (std::vector<int> { 7 }));
        CHECK(rows::forCard({ nothing }).front().touchedSlots().empty());
    }

    // --- the window's words for a row that did nothing, or did not finish ---
    {
        Card renamed = op(10, 10000, "rename");
        renamed.subjects = { 7 };
        CHECK_EQ(rows::forCard({ renamed }).front().action, std::string("Renamed"));
        CHECK_EQ(rows::forCard({ renamed }).front().detail, std::string("nothing changed"));
        Card oneshot = op(11, 11000, "oneshot");
        oneshot.subjects = { 7 };
        CHECK_EQ(rows::forCard({ oneshot }).front().action, std::string("One Shot"));
        CHECK_EQ(rows::forCard({ oneshot }).front().detail, std::string("nothing changed"));
        // a failed attempt: the state, the reason, and no switch position
        Card failedOneShot = op(12, 12000, "oneshot", "app", "failed");
        failedOneShot.note = "cannot write MEMORY1.RC0";
        failedOneShot.subjects = { 7 };
        CHECK_EQ(rows::forCard({ failedOneShot }).front().action, std::string("One Shot"));
        CHECK_EQ(rows::forCard({ failedOneShot }).front().detail, failedOneShot.note);
        CHECK_EQ(rows::forCard({ failedOneShot }).front().state, std::string("failed"));
        // an old no-op (no subject, no note) says nothing rather than inventing it
        Card old = op(13, 13000, "normalize");
        CHECK_EQ(rows::forCard({ old }).front().detail, std::string());
        // a failed trim that recorded bodies on the way: its reason, not its numbers
        Card failedTrim = op(14, 14000, "trim", "app", "failed");
        failedTrim.note = "cannot write MEMORY2.RC0";
        Card::Slot s = touched(failedTrim, 12, false);
        s.facts.beforeBody = loaded;
        s.facts.afterBody = shorter;
        failedTrim.slots = { s };
        CHECK_EQ(rows::forCard({ failedTrim }).front().detail, failedTrim.note);
        CHECK_EQ(rows::forCard({ failedTrim }).front().state, std::string("failed"));
    }

    // --- pins ride along; an empty card has no rows ---
    {
        Card pinned = op(1, 1000, "rename");
        pinned.pinned = true;
        CHECK(rows::forCard({ pinned }).front().pinned);
        CHECK(!rows::forCard({ op(2, 2000, "rename") }).front().pinned);
        CHECK(rows::forCard({}).empty());
    }

    return testkit::summary("card_rows_tests");
}
