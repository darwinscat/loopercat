// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <vector>

//==============================================================================
// loopercat::history::badges — which badges a History row actually wears.
//
// A row has room for a few badges before its sentence, and an operation can
// touch more slots than that: a first-sighting snapshot touches all 99. One
// answer, read by both the painting and the hit test, so they cannot drift
// apart again (#143: the painting drew every slot and the hit test answered
// everywhere, and the snapshot row was 96 badges over its own words):
//
//   - up to `room` slots are badges, in the row's order
//   - past that, the last position is a "+N" chip counting the rest — a
//     number to read, not a badge to click
//   - the slot the timeline is filtered to is never behind the chip: when
//     it would be, it takes the last badge position in place of the row's
//     slot there, so the row shows why it is listed; the chip's count does
//     not move — one slot came out of hiding as one went in
//   - a row counted in words (the snapshot's sentence already says "99
//     slots") wears none at all
//==============================================================================
namespace loopercat::history::badges
{

struct Shown {
    std::vector<int> slots; // the badges, first to last; the chip, if any, sits right after them
    int more = 0;           // the slots the chip counts; 0 means no chip
    bool inWords = false;   // no badges: the row's sentence counts its slots
};

inline Shown shown(const std::vector<int>& slots, bool countedInWords, int room,
                   std::optional<int> filter)
{
    if (room < 1)
        throw Error("a row has room for at least one badge");
    if (countedInWords)
        return { {}, 0, true };
    if (slots.size() <= static_cast<std::size_t>(room))
        return { slots, 0, false };
    const auto badges = static_cast<std::ptrdiff_t>(room - 1); // the last position goes to the chip
    std::vector<int> worn(slots.begin(), std::next(slots.begin(), badges));
    const auto has = [](const std::vector<int>& in, int slot) {
        return std::find(in.begin(), in.end(), slot) != in.end();
    };
    if (filter && !worn.empty() && has(slots, *filter) && !has(worn, *filter))
        worn.back() = *filter;
    return { std::move(worn), static_cast<int>(slots.size()) - static_cast<int>(badges), false };
}

} // namespace loopercat::history::badges
