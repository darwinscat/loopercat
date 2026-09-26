// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// How the loop starts and stops, as one feature: START, STOP and FADE TIME
// of a memory's LOOP screen (RC-5 reference manual p. 9). START is
// IMMEDIATE or FADE IN; STOP is IMMEDIATE, FADE OUT or LOOP END (play to the
// end of the loop, then stop); FADE TIME is how long a fade takes, and the
// manual prints its values as four note lengths followed by 1MEAS–64MEAS.
//
// Ownership: TRACK1's StrtMod and StpMod, and MASTER's FadeTime. Nothing of
// the loop's LENGTH is this feature's (Measure, MeasLen, LpLen — #92), and
// the loop's modes (LpMod, TrkMod, Sync) wait for their own use case.
//
// The lists are the manual's, in its order, counted from zero. Anchors: the
// factory body carries StrtMod 0, StpMod 0, FadeTime 5, and the manual's
// bold defaults are IMMEDIATE, IMMEDIATE and 2MEAS — three points that fit
// the rule. Inferred, not measured: that the four note lengths occupy
// 0..3 and the measures follow at 4 (1MEAS) .. 67 (64MEAS), which is where
// the factory 5 = 2MEAS anchor puts them. A number outside a list is a
// typed error carrying the number.
//
// One thing the page says that the player should hear before the fact: no
// overdub is possible while a stop is still under way (a fade-out or a run
// to the loop's end).

#pragma once

#include "../Error.hpp"
#include "../Rc0.hpp"
#include "Choice.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace loopercat::usecases::playstop {

// One entry of a list: the number the card stores, the name the screen shows
// (Choice.hpp, shared with every list-valued use case).
using usecases::Choice;

// --- the lists (reference manual p. 9) ---

// START: bold default IMMEDIATE, the factory value 0.
inline constexpr std::array<Choice, 2> kStartModes { { { 0, "IMMEDIATE" }, { 1, "FADE IN" } } };
inline constexpr long long kStartFadeIn = 1;

// STOP: bold default IMMEDIATE, the factory value 0.
inline constexpr std::array<Choice, 3> kStopModes { {
    { 0, "IMMEDIATE" }, { 1, "FADE OUT" }, { 2, "LOOP END" },
} };
inline constexpr long long kStopFadeOut = 1;

// FADE TIME: four note lengths, then whole measures. The screen prints the
// notes as glyphs; these are their names. Measures are stored as
// measures + kFadeTimeMeasureOffset, so 2MEAS is the factory 5.
inline constexpr std::array<Choice, 4> kFadeTimeNotes { {
    { 0, "1/16 note" }, { 1, "1/8 note" }, { 2, "1/4 note" }, { 3, "1/2 note" },
} };
inline constexpr long long kFadeTimeMeasureOffset = 3;
inline constexpr long long kFadeTimeMeasureMax = 64;
inline constexpr long long kFadeTimeCount = kFadeTimeNotes.size() + kFadeTimeMeasureMax; // 68

// --- names ---

inline std::string_view startModeName(long long n) { return nameIn(kStartModes, "START", n); }
inline std::string_view stopModeName(long long n) { return nameIn(kStopModes, "STOP", n); }

// "1/4 note", "1MEAS", "64MEAS" — as the screen prints them.
inline std::string fadeTimeName(long long n)
{
    if (n < 0 || n >= kFadeTimeCount)
        throw Error("FADE TIME " + std::to_string(n) + " is not in the manual's list of "
                    + std::to_string(kFadeTimeCount) + " (0.." + std::to_string(kFadeTimeCount - 1)
                    + ")");
    if (n < static_cast<long long>(kFadeTimeNotes.size()))
        return std::string(kFadeTimeNotes[static_cast<std::size_t>(n)].name);
    return std::to_string(n - kFadeTimeMeasureOffset) + "MEAS";
}

// --- reading ---

namespace detail {

    inline long long track(std::string_view slotBody, std::string_view tag)
    {
        return rc0::sectionField(slotBody, rc0::kSectionTrack1, tag);
    }

    inline long long master(std::string_view slotBody, std::string_view tag)
    {
        return rc0::sectionField(slotBody, rc0::kSectionMaster, tag);
    }

} // namespace detail

struct Values {
    long long startMode;
    long long stopMode;
    long long fadeTime;
    bool fadeInUse; // FADE TIME is heard: a fade-in or a fade-out is set

    bool operator==(const Values&) const = default;
};

inline bool fadeInUse(long long startMode, long long stopMode)
{
    return startMode == kStartFadeIn || stopMode == kStopFadeOut;
}

inline Values read(std::string_view slotBody)
{
    const long long start = detail::track(slotBody, "StrtMod");
    const long long stop = detail::track(slotBody, "StpMod");
    return { start, stop, detail::master(slotBody, "FadeTime"), fadeInUse(start, stop) };
}

// --- edits ---

// Any subset of the three. An absent field's bytes are reproduced exactly.
struct Edits {
    std::optional<long long> startMode;
    std::optional<long long> stopMode;
    std::optional<long long> fadeTime;

    bool empty() const { return !startMode && !stopMode && !fadeTime; }
};

// Every present field is checked against the manual's list before the first
// byte moves, so a refused edit leaves the body untouched. An empty edit is
// a caller bug, not a no-op.
inline std::string apply(std::string_view slotBody, const Edits& edits)
{
    if (edits.empty())
        throw Error("no start/stop setting to change");
    if (edits.startMode)
        startModeName(*edits.startMode);
    if (edits.stopMode)
        stopModeName(*edits.stopMode);
    if (edits.fadeTime)
        fadeTimeName(*edits.fadeTime);

    std::string body(slotBody);
    if (edits.startMode)
        body = rc0::setSectionField(body, rc0::kSectionTrack1, "StrtMod", *edits.startMode);
    if (edits.stopMode)
        body = rc0::setSectionField(body, rc0::kSectionTrack1, "StpMod", *edits.stopMode);
    if (edits.fadeTime)
        body = rc0::setSectionField(body, rc0::kSectionMaster, "FadeTime", *edits.fadeTime);
    return body;
}

// The edit in the pedal's own words, for the history and the banner:
// "start FADE IN", "stop LOOP END", "fade time 2MEAS".
inline std::string describe(const Edits& edits)
{
    std::string out;
    const auto add = [&out](const std::string& text) {
        if (!out.empty())
            out += ", ";
        out += text;
    };
    if (edits.startMode)
        add("start " + std::string(startModeName(*edits.startMode)));
    if (edits.stopMode)
        add("stop " + std::string(stopModeName(*edits.stopMode)));
    if (edits.fadeTime)
        add("fade time " + fadeTimeName(*edits.fadeTime));
    return out;
}

} // namespace loopercat::usecases::playstop
