// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Play Count-In as one feature, not three fields — the shape every editor
// entry takes: the pedal's own name for the thing (PLAY COUNT, as opposed to
// its REC COUNT), one on/off, and an explicit list of the bytes it owns.
//
// The RHYTHM triple is NOT one meaning (hardware, Alisa's RC-5, 2026-08-10):
//
//   PlayCount = 1MEAS  IS the count-in, and it sounds whatever the pattern
//                      is — verified with a real groove playing.
//   State     = 1      the rhythm section must be on for anything of it to
//                      be heard, the count included.
//   Pattern   = Blank  a separate meaning: "and silence after the count".
//
// So this feature owns PlayCount always, and State/Pattern only while the
// rhythm is otherwise silent. A groove set on the pedal is never overwritten
// and never cancels the count — the two coexist on the hardware, and they
// coexist here.

#pragma once

#include "../Rc0.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace loopercat::usecases::countin {

// All three fields are RHYTHM's, and they are read and written inside that
// section only (rc0::sectionField) — a tag of the same name elsewhere in the
// memory is none of this feature's business.
namespace detail {

    inline long long rhythm(std::string_view slotBody, std::string_view tag)
    {
        return rc0::sectionField(slotBody, rc0::kSectionRhythm, tag);
    }

    inline std::string setRhythm(std::string_view slotBody, std::string_view tag, long long value)
    {
        return rc0::setSectionField(slotBody, rc0::kSectionRhythm, tag, value);
    }

} // namespace detail

// Will the musician hear a count before this memory plays? PlayCount alone
// is not enough: with the rhythm section off, nothing of it reaches the
// output. The manual's domain for PlayCount is off / 1MEAS — no third value
// exists to guess at.
inline bool isOn(std::string_view slotBody)
{
    return detail::rhythm(slotBody, "State") == rc0::kRhythmStateOn
        && detail::rhythm(slotBody, "PlayCount") == rc0::kRhythmPlayCount1Meas;
}

// The groove that switching the count ON would replace, if any. Only a
// silent rhythm section puts a pattern at risk: with the rhythm already
// playing we leave Pattern alone, and Blank is not a groove anyone chose.
// Pattern 0 is the factory value every untouched slot carries
// (fixtures/golden.json) — reporting it would cry wolf on a fresh pedal.
inline std::optional<long long> patternAtRisk(std::string_view slotBody)
{
    const long long pattern = detail::rhythm(slotBody, "Pattern");
    if (detail::rhythm(slotBody, "State") == rc0::kRhythmStateOn)
        return std::nullopt;
    if (pattern == rc0::kRhythmPatternBlank || pattern == 0)
        return std::nullopt;
    return pattern;
}

// Switch the count-in for one slot body. Turning it on over a silent rhythm
// writes the whole triple (count, then silence); over a playing rhythm it
// writes the count only. Turning it off gives the borrowed fields back —
// the factory zeros, not a saved copy of anything: the feature keeps no
// hidden state, so a slot that only ever had a count-in round-trips to the
// exact bytes it started with.
inline std::string apply(std::string_view slotBody, bool on)
{
    std::string body(slotBody);
    const bool rhythmPlaying = detail::rhythm(body, "State") == rc0::kRhythmStateOn;
    const bool rhythmSilent = detail::rhythm(body, "Pattern") == rc0::kRhythmPatternBlank;

    if (on) {
        body = detail::setRhythm(body, "PlayCount", rc0::kRhythmPlayCount1Meas);
        if (!rhythmPlaying) {
            body = detail::setRhythm(body, "State", rc0::kRhythmStateOn);
            body = detail::setRhythm(body, "Pattern", rc0::kRhythmPatternBlank);
        }
        return body;
    }

    body = detail::setRhythm(body, "PlayCount", 0);
    // Hand State/Pattern back only if they were borrowed FOR the count: a
    // rhythm that was on without a count-in is the user's, off or not.
    if (isOn(slotBody) && rhythmSilent) {
        body = detail::setRhythm(body, "State", 0);
        body = detail::setRhythm(body, "Pattern", 0);
    }
    return body;
}

} // namespace loopercat::usecases::countin
