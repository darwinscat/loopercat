// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What LooperCat does with a card's root when it meets the card: knows the
// card by its marker (CardMarker.hpp), minting one when the card has none,
// and keeps Spotlight off it (Spotlight.hpp, issue #169). One place, so
// every path that reaches a card's identity — the corner's name at Connect,
// the history's session for an operation — leaves the root the same way.

#pragma once

#include "CardMarker.hpp"
#include "Commands.hpp"
#include "Rc0.hpp"
#include "Spotlight.hpp"
#include "Volume.hpp"

#include <filesystem>
#include <iterator>
#include <optional>
#include <utility>

namespace loopercat::cardclaim {

namespace fs = std::filesystem;

struct Claimed {
    marker::Card card;
    bool minted = false;            // the card had no marker: it was given one just now
    bool spotlightKeptOff = false;  // .metadata_never_index was written just now
    volume::SweepResult sweep;      // the sidecars those writes left, removed — or, in `failed`, not
};

// The marker first, then Spotlight: a marker this build cannot read is
// refused before anything is written, exactly as marker::read refuses it.
// A new marker is named after the card's model until the player renames it.
inline Claimed claim(const fs::path& volume)
{
    Claimed claimed;
    if (std::optional<marker::Card> found = marker::read(volume)) {
        claimed.card = std::move(*found);
    } else {
        marker::Written written =
            marker::mint(volume, rc0::familyOf(commands::readMemory(volume)).familyName);
        claimed.card = std::move(written.card);
        claimed.minted = true;
        claimed.sweep = std::move(written.sweep);
    }
    spotlight::KeptOff kept = spotlight::keepOff(volume);
    claimed.spotlightKeptOff = kept.created;
    claimed.sweep.removed.insert(claimed.sweep.removed.end(),
                                 std::make_move_iterator(kept.sweep.removed.begin()),
                                 std::make_move_iterator(kept.sweep.removed.end()));
    claimed.sweep.failed.insert(claimed.sweep.failed.end(),
                                std::make_move_iterator(kept.sweep.failed.begin()),
                                std::make_move_iterator(kept.sweep.failed.end()));
    return claimed;
}

} // namespace loopercat::cardclaim
