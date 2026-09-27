// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// One entry of a list the reference manual prints: the number the card
// stores, the name the screen shows. Every use case that owns a list-valued
// field (Rhythm.hpp, PlayStop.hpp) keeps its lists as arrays of these, in
// the manual's order, counted from zero — and a number outside the list is
// a typed error carrying the number, never a guess.

#pragma once

#include "../Error.hpp"

#include <span>
#include <string>
#include <string_view>

namespace loopercat::usecases {

struct Choice {
    long long number;
    std::string_view name;
};

// The name at `number`, or the error that says which list and how long.
inline std::string_view nameIn(std::span<const Choice> list, std::string_view what,
                               long long number)
{
    for (const Choice& choice : list)
        if (choice.number == number)
            return choice.name;
    throw Error(std::string(what) + " " + std::to_string(number)
                + " is not in the manual's list of " + std::to_string(list.size()) + " (0.."
                + std::to_string(list.size() - 1) + ")");
}

} // namespace loopercat::usecases
