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
//
// Blank and the factory 0 are 4/4 numbers (Beat.hpp, #147): the PATTERN a
// memory stores indexes the list of its current BEAT, and only the 4/4 list
// is charted. So the two paths of this feature that write Pattern — the
// borrow on the way on, the hand-back on the way off — are refused at any
// other beat before a byte moves (#149; found with the pedal's own Rock2 at
// 6/4, which "count-in on" turned into 57). The count alone, PlayCount over
// a rhythm that is already playing, is still ours at every beat.

#pragma once

#include "../Error.hpp"
#include "../Rc0.hpp"
#include "Beat.hpp"

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

// The two paths of the switch that write a PATTERN number, both 4/4 numbers.
enum class Refusal {
    onBorrowsSection,  // on over a silent section would write Blank (57)
    offReturnsSection, // off would hand the borrowed section back as the factory 0
};

// Why switching the count to `on` is refused at this memory's beat, or
// nothing. Pure over the body, and typed, so the table and the card make the
// switch a lamp before the click and say so in their own words, while
// apply() refuses after it with the fact (refusalText). The beat is not
// named here: a memory can hold a beat the manual's list lacks, and a
// PlayCount-only switch there must still pass. Already there is already
// there — a count switched on over a playing rhythm, or off where nothing
// was borrowed, moves PlayCount alone and passes at every beat.
inline std::optional<Refusal> refusal(std::string_view slotBody, bool on)
{
    if (beat::patternListCharted(detail::rhythm(slotBody, "Beat")))
        return std::nullopt;
    const bool rhythmPlaying = detail::rhythm(slotBody, "State") == rc0::kRhythmStateOn;
    if (on) {
        if (rhythmPlaying)
            return std::nullopt;
        return Refusal::onBorrowsSection;
    }
    const bool borrowed = isOn(slotBody)
        && detail::rhythm(slotBody, "Pattern") == rc0::kRhythmPatternBlank;
    if (borrowed)
        return Refusal::offReturnsSection;
    return std::nullopt;
}

// The refusal as the core states it, for the banner and the log: what the
// click needed, the beat as the screen prints it, and the fact.
inline std::string refusalText(Refusal why, long long atBeat)
{
    const std::string fact(beat::kOnlyFourFourCharted);
    switch (why) {
    case Refusal::onBorrowsSection:
        return "Switching the count-in on at " + beat::label(atBeat)
            + " would silence the rhythm with Blank, a 4/4 number, and " + fact
            + ": switch the rhythm on first, and the count joins it.";
    case Refusal::offReturnsSection:
        return "Switching the count-in off at " + beat::label(atBeat)
            + " would hand the rhythm back the factory pattern, a 4/4 number, and " + fact
            + ": set it on the pedal.";
    }
    throw Error("unknown count-in refusal " + std::to_string(static_cast<int>(why)));
}

// The groove that switching the count ON would replace, if any. Only a
// silent rhythm section puts a pattern at risk: with the rhythm already
// playing we leave Pattern alone, and Blank is not a groove anyone chose.
// Pattern 0 is the factory value every untouched slot carries
// (fixtures/golden.json) — reporting it would cry wolf on a fresh pedal.
// At an uncharted beat nothing is at risk, because nothing will be written:
// the switch that would replace the pattern is refused there (refusal), and
// the refusal is what the UI says instead. Deciding anything from 0 or 57
// at such a beat would be reading 4/4 names into another list's numbers.
inline std::optional<long long> patternAtRisk(std::string_view slotBody)
{
    if (!beat::patternListCharted(detail::rhythm(slotBody, "Beat")))
        return std::nullopt;
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
// exact bytes it started with. At an uncharted beat the two paths that
// write Pattern are refused before any byte moves (refusal).
inline std::string apply(std::string_view slotBody, bool on)
{
    if (const auto why = refusal(slotBody, on))
        throw Error(refusalText(*why, detail::rhythm(slotBody, "Beat")));

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
