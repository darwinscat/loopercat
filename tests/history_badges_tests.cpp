// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The badges a History row wears, from what the row promises (#143):
//
//   - the slots that fit are badges, in the row's own order, and no more
//   - past the room, the last position is a "+N" chip counting the rest
//   - a row that fits exactly wears every badge and no chip
//   - the slot the timeline is filtered to is never hidden behind the
//     chip: it takes the last badge position, and the chip's count stays
//   - a row counted in words — the first-sighting snapshot, "99 slots" —
//     wears none, whatever it touched and whatever the filter
//   - no room at all is a mistake, not an empty row

#include "support.hpp"

#include "../app/HistoryBadges.h"

#include <optional>
#include <vector>

using loopercat::history::badges::Shown;
using loopercat::history::badges::shown;

namespace {

const std::optional<int> noFilter = std::nullopt;

std::vector<int> all99()
{
    std::vector<int> all;
    for (int slot = 1; slot <= 99; ++slot)
        all.push_back(slot);
    return all;
}

} // namespace

int main()
{
    const int room = 3; // the window's: three before the sentence

    // The issue's row: a snapshot of all 99 slots wears no badge at all.
    {
        const Shown s = shown(all99(), true, room, noFilter);
        CHECK(s.slots.empty());
        CHECK_EQ(s.more, 0);
        CHECK(s.inWords);
    }

    // A snapshot of one slot is still said in words, not worn.
    {
        const Shown s = shown({ 57 }, true, room, noFilter);
        CHECK(s.slots.empty());
        CHECK_EQ(s.more, 0);
        CHECK(s.inWords);
    }

    // Five slots: the first two, then a chip counting the other three.
    {
        const Shown s = shown({ 3, 7, 12, 40, 99 }, false, room, noFilter);
        CHECK(s.slots == (std::vector<int> { 3, 7 }));
        CHECK_EQ(s.more, 3);
        CHECK(!s.inWords);
    }

    // Exactly the room: all three, no chip.
    {
        const Shown s = shown({ 5, 6, 8 }, false, room, noFilter);
        CHECK(s.slots == (std::vector<int> { 5, 6, 8 }));
        CHECK_EQ(s.more, 0);
        CHECK(!s.inWords);
    }

    // One over the room: the chip takes the third position and counts two.
    // A "+1" there would hide a slot only to show a number.
    {
        const Shown s = shown({ 1, 2, 3, 4 }, false, room, noFilter);
        CHECK(s.slots == (std::vector<int> { 1, 2 }));
        CHECK_EQ(s.more, 2);
    }

    // One slot, and a swap of two: worn as they are.
    {
        CHECK(shown({ 12 }, false, room, noFilter).slots == (std::vector<int> { 12 }));
        CHECK_EQ(shown({ 12 }, false, room, noFilter).more, 0);
        const Shown swap = shown({ 12, 43 }, false, room, noFilter);
        CHECK(swap.slots == (std::vector<int> { 12, 43 }));
        CHECK_EQ(swap.more, 0);
        CHECK(!swap.inWords);
    }

    // The pedal's own settings touch no slot: nothing worn, nothing counted,
    // and not a row said in words either.
    {
        const Shown s = shown({}, false, room, noFilter);
        CHECK(s.slots.empty());
        CHECK_EQ(s.more, 0);
        CHECK(!s.inWords);
    }

    // All 99 slots in one operation that is not a snapshot: two badges and
    // a chip for the other 97 — never 99 badges.
    {
        const Shown s = shown(all99(), false, room, noFilter);
        CHECK(s.slots == (std::vector<int> { 1, 2 }));
        CHECK_EQ(s.more, 97);
        CHECK(!s.inWords);
    }

    // The order is the row's own; badges are not sorted here.
    {
        CHECK(shown({ 43, 12, 7, 1 }, false, room, noFilter).slots == (std::vector<int> { 43, 12 }));
    }

    // Room for one: a single slot is worn; two slots are only a count.
    {
        CHECK(shown({ 9 }, false, 1, noFilter).slots == (std::vector<int> { 9 }));
        CHECK_EQ(shown({ 9 }, false, 1, noFilter).more, 0);
        const Shown two = shown({ 9, 10 }, false, 1, noFilter);
        CHECK(two.slots.empty());
        CHECK_EQ(two.more, 2);
    }

    // --- the filter: a listed row shows the slot it is listed for ---

    // Filtered to 40, hidden behind the chip: 40 takes the second badge
    // position from 7, and the chip still counts three (7, 12 and 99 now).
    {
        const Shown s = shown({ 3, 7, 12, 40, 99 }, false, room, 40);
        CHECK(s.slots == (std::vector<int> { 3, 40 }));
        CHECK_EQ(s.more, 3);
        CHECK(!s.inWords);
    }

    // The last slot, hidden the same way.
    {
        const Shown s = shown({ 3, 7, 12, 40, 99 }, false, room, 99);
        CHECK(s.slots == (std::vector<int> { 3, 99 }));
        CHECK_EQ(s.more, 3);
    }

    // Filtered to a slot already worn: nothing moves, first or second.
    {
        CHECK(shown({ 3, 7, 12, 40, 99 }, false, room, 3).slots == (std::vector<int> { 3, 7 }));
        CHECK(shown({ 3, 7, 12, 40, 99 }, false, room, 7).slots == (std::vector<int> { 3, 7 }));
        CHECK_EQ(shown({ 3, 7, 12, 40, 99 }, false, room, 7).more, 3);
    }

    // Filtered to a slot the row never touched: the answer stays the row's
    // own — nothing is invented to match the filter.
    {
        const Shown s = shown({ 3, 7, 12, 40, 99 }, false, room, 50);
        CHECK(s.slots == (std::vector<int> { 3, 7 }));
        CHECK_EQ(s.more, 3);
    }

    // A row that fits has nothing hidden: the filter changes nothing.
    {
        CHECK(shown({ 5, 6, 8 }, false, room, 8).slots == (std::vector<int> { 5, 6, 8 }));
        CHECK_EQ(shown({ 5, 6, 8 }, false, room, 8).more, 0);
        CHECK(shown({ 12, 43 }, false, room, 43).slots == (std::vector<int> { 12, 43 }));
    }

    // A snapshot stays in words under any filter.
    {
        const Shown s = shown(all99(), true, room, 57);
        CHECK(s.slots.empty());
        CHECK_EQ(s.more, 0);
        CHECK(s.inWords);
    }

    // Room for two: the one badge position is the filtered slot's.
    {
        const Shown s = shown({ 3, 7, 12, 40, 99 }, false, 2, 40);
        CHECK(s.slots == (std::vector<int> { 40 }));
        CHECK_EQ(s.more, 4);
    }

    // Room for one: there is no badge position to give, so the count stays
    // a count — the filter cannot conjure a badge.
    {
        const Shown s = shown({ 9, 10 }, false, 1, 10);
        CHECK(s.slots.empty());
        CHECK_EQ(s.more, 2);
    }

    // No room is a mistake, before the row is even looked at.
    {
        CHECK_THROWS(shown({ 1 }, false, 0, noFilter), "room");
        CHECK_THROWS(shown({}, false, 0, noFilter), "room");
        CHECK_THROWS(shown({ 1 }, true, -1, 1), "room");
    }

    return testkit::summary("history_badges_tests");
}
