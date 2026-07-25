// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Memory Settings, Tier 1 (issue #30): the per-memory fields exposed for
// external editing — playback shaping and the onboard drums. Only fields
// that pass the verification gate are here: booleans (stored 0/1 like every
// flag in the file) and numeric ranges documented in the RC-5 manual
// (docs/pedal-settings.md — levels 0..200 with 100 = unity, Pan 0..100 with
// 50 = center). Enum-valued fields (StrtMod/StpMod, Pattern, Kit, Beat,
// Variation…) join ONLY with a hardware-verified value map; until then they
// stay out entirely rather than shipping guessed labels.
//
// All field access is section-scoped: <Level> means one thing in MASTER and
// another in RHYTHM, and this module edits the RHYTHM one.

#pragma once

#include "Error.hpp"
#include "Rc0.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace loopercat::memsettings {

inline constexpr long long kLevelMax = 200; // PlyLvl / RHYTHM Level; 100 = unity
inline constexpr long long kPanMax = 100;   // 50 = center

// The current state of a slot's Tier-1 settings, for display.
struct Values {
    bool reverse;          // TRACK1 <Rev>
    long long playLevel;   // TRACK1 <PlyLvl>
    long long pan;         // TRACK1 <Pan>
    bool rhythmOn;         // RHYTHM <State>
    long long rhythmLevel; // RHYTHM <Level> — not the MASTER <Level>

    bool operator==(const Values&) const = default;
};

// One edit request: any subset of fields. An absent field's stored bytes are
// reproduced exactly (the byte-surgery invariant).
struct Edits {
    std::optional<bool> reverse;
    std::optional<long long> playLevel;
    std::optional<long long> pan;
    std::optional<bool> rhythmOn;
    std::optional<long long> rhythmLevel;

    bool empty() const
    {
        return !reverse && !playLevel && !pan && !rhythmOn && !rhythmLevel;
    }
};

inline Values read(std::string_view slotBody)
{
    return { rc0::sectionField(slotBody, "TRACK1", "Rev") == 1,
             rc0::sectionField(slotBody, "TRACK1", "PlyLvl"),
             rc0::sectionField(slotBody, "TRACK1", "Pan"),
             rc0::sectionField(slotBody, "RHYTHM", "State") == 1,
             rc0::sectionField(slotBody, "RHYTHM", "Level") };
}

namespace detail {

    inline void assertRange(std::string_view what, long long value, long long max)
    {
        if (value < 0 || value > max)
            throw Error(std::string(what) + " out of range 0.." + std::to_string(max) + ": "
                        + std::to_string(value));
    }

} // namespace detail

// Apply an edit set to a slot body. Every present field validates before the
// first byte moves; an empty edit set is a caller bug, not a no-op.
inline std::string apply(std::string_view slotBody, const Edits& edits)
{
    if (edits.empty())
        throw Error("no memory settings to change");
    if (edits.playLevel)
        detail::assertRange("play level", *edits.playLevel, kLevelMax);
    if (edits.pan)
        detail::assertRange("pan", *edits.pan, kPanMax);
    if (edits.rhythmLevel)
        detail::assertRange("rhythm level", *edits.rhythmLevel, kLevelMax);

    std::string body(slotBody);
    if (edits.reverse)
        body = rc0::setSectionField(body, "TRACK1", "Rev", *edits.reverse ? 1 : 0);
    if (edits.playLevel)
        body = rc0::setSectionField(body, "TRACK1", "PlyLvl", *edits.playLevel);
    if (edits.pan)
        body = rc0::setSectionField(body, "TRACK1", "Pan", *edits.pan);
    if (edits.rhythmOn)
        body = rc0::setSectionField(body, "RHYTHM", "State", *edits.rhythmOn ? 1 : 0);
    if (edits.rhythmLevel)
        body = rc0::setSectionField(body, "RHYTHM", "Level", *edits.rhythmLevel);
    return body;
}

} // namespace loopercat::memsettings
