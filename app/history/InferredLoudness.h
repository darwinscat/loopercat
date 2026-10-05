// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"

#include <loopercat/Normalize.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

//==============================================================================
// loopercat::history::inferLoudness — what the history already knows about
// the takes a connect scan found, without reading one byte of audio (#141).
//
// For each slot sighted, the store is asked which take it holds by the facts
// the directory entry and the config gave for free (HistoryStore::
// hashOfSighting), and then whether those bytes were ever measured
// (readingFor, #140). A slot the history cannot vouch for, or whose bytes it
// never measured, is simply not in the answer: a dash stays a dash, never a
// guess.
//
// A read and nothing else — no row is written, no reading filed, no
// operation opened. What comes back informs the eye (SlotTable::LoudnessCell::
// inferred) and decides nothing: Normalize measures from the card's bytes
// whatever this said.
//
// Worker thread, like every read of the store.
//==============================================================================
namespace loopercat::history
{

struct SlotSighting {
    int slot = 0;
    HistoryStore::TakeSighting take;
};

// The answer names the sighting it answers, whole: the caller holds it
// against the slot as it is by the time the answer lands, and a slot whose
// facts moved on meanwhile is not the one this reading was found for.
struct InferredReading {
    SlotSighting sighted;
    wav::LoudnessReading reading;
    std::int64_t measuredMs = 0; // when the bytes were measured
};

inline std::vector<InferredReading> inferLoudness(HistoryStore& store, std::int64_t card,
                                                  const std::vector<SlotSighting>& sightings)
{
    std::vector<InferredReading> out;
    for (const SlotSighting& sighted : sightings) {
        const std::optional<std::string> hash = store.hashOfSighting(card, sighted.slot, sighted.take);
        if (!hash)
            continue;
        const std::optional<HistoryStore::StoredReading> stored = store.readingFor(*hash);
        if (!stored)
            continue;
        out.push_back({ sighted, stored->reading, stored->measuredMs });
    }
    return out;
}

} // namespace loopercat::history
