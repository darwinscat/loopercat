// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The rhythm as one feature: will drums play with this memory, and which. On
// the RC-5 that is the RHYTHM screen of a memory (reference manual p. 10) —
// PATTERN, KIT, BEAT, VARIATION, LEVEL, REVERB, TONE LOW, TONE HIGH — and one
// fact the screen shows as "rhythm: on", the <State> field: the memory is
// recalled with its rhythm already running (hardware, 2026-08-10).
//
// Ownership, shared with Play Count-In (CountIn.hpp), whose boundary is
// already drawn there: the count-in owns PlayCount always, and State/Pattern
// only while the rhythm is otherwise silent. This feature owns State — its
// switch — and Pattern as the groove, plus the six fields above that no
// other feature touches. Where the two meet, the pedal's own model decides:
//
//   on  = State on AND a groove, i.e. Pattern is not Blank. Blank is the
//         count-in's word ("and silence after the count"); it is never a
//         groove this feature chooses, and it never reads as drums playing.
//   off, count-in off: State goes off, Pattern stays. That is the pedal's
//         own shape — State is the switch, Pattern the selection — and the
//         count-in card keeps warning about a chosen-but-silent pattern.
//   off, count-in on: State must stay on or the count dies with it, so the
//         groove becomes Blank. The pattern is forgotten, and the UI says so
//         before the click (patternLostOnOff). No hidden state anywhere.
//
// The lists are the manual's, in its order, counted from zero. Three points
// are measured on the pedal: Pattern 57 = Blank (kRhythmPatternBlank), Beat 2
// = 4/4, PlayCount 1 = 1MEAS. The rest — every other name, the interior of
// the BEAT range the manual prints as "2/4–4/4–7/4, 5/8–15/8", the +10 the
// file adds to TONE LOW/HIGH (screen −10..+10, factory body 10) — is per the
// manual and the factory body, not yet read off the screen. A number outside
// a list is a typed error carrying the number: a control that shows "?" for
// it would hide exactly the fact that says the rule is wrong.
//
// One rule the manual states outright (p. 10): BEAT cannot be changed after
// a track is recorded. A slot with an indexed take refuses a Beat edit; the
// bar arithmetic that assumes 4/4 (Params.hpp, Commands.hpp) is #92's, and
// this feature leaves every length field alone.

#pragma once

#include "../Error.hpp"
#include "../Rc0.hpp"
#include "CountIn.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace loopercat::usecases::rhythm {

// --- the lists (RC-5 reference manual p. 10, in print order, from zero) ---

// One entry of a list: the number the card stores, the name the screen shows.
struct Choice {
    long long number;
    std::string_view name;
};

// PATTERN: 57 grooves in the manual's fourteen groups, then Blank. Anchors:
// 0 is the factory value and the printed default (SimpleBeat1); 57 is Blank
// on the screen (hardware). Interior boundaries follow from the group sizes.
inline constexpr std::array<Choice, 58> kPatterns { {
    { 0, "SimpleBeat1" }, { 1, "SimpleBeat2" }, { 2, "SimpleBeat3" }, { 3, "SimpleBeat4" },
    { 4, "GrooveBeat1" }, { 5, "GrooveBeat2" }, { 6, "GrooveBeat3" }, { 7, "GrooveBeat4" },
    { 8, "GrooveBeat5" }, { 9, "GrooveBeat6" }, { 10, "GrooveBeat7" },
    { 11, "Rock1" }, { 12, "Rock2" }, { 13, "Rock3" }, { 14, "Rock4" },
    { 15, "Funk1" }, { 16, "Funk2" }, { 17, "Funk3" }, { 18, "Funk4" },
    { 19, "Shuffle1" }, { 20, "Shuffle2" }, { 21, "Shuffle3" }, { 22, "Shuffle4" },
    { 23, "Shuffle5" },
    { 24, "Swing1" }, { 25, "Swing2" }, { 26, "Swing3" }, { 27, "Swing4" }, { 28, "Swing5" },
    { 29, "SideStick1" }, { 30, "SideStick2" }, { 31, "SideStick3" }, { 32, "SideStick4" },
    { 33, "SideStick5" },
    { 34, "PercusBeat1" }, { 35, "PercusBeat2" }, { 36, "PercusBeat3" }, { 37, "PercusBeat4" },
    { 38, "LatinBeat1" }, { 39, "LatinBeat2" }, { 40, "LatinBeat3" }, { 41, "LatinBeat4" },
    { 42, "Conga1" }, { 43, "Conga2" }, { 44, "Conga3" },
    { 45, "Bossa1" }, { 46, "Bossa2" },
    { 47, "Samba1" }, { 48, "Samba2" },
    { 49, "DanceBeat1" }, { 50, "DanceBeat2" }, { 51, "DanceBeat3" }, { 52, "DanceBeat4" },
    { 53, "Metronome1" }, { 54, "Metronome2" }, { 55, "Metronome3" }, { 56, "Metronome4" },
    { rc0::kRhythmPatternBlank, "Blank" },
} };

// The groove this feature reaches for when it has none: the printed default,
// the factory value of every untouched slot.
inline constexpr long long kPatternDefault = 0;

// KIT: seven, "Rhythm Kit: 7 types" on the specification page. Anchor: 0 is
// the factory value and the printed default (Studio).
inline constexpr std::array<Choice, 7> kKits { {
    { 0, "Studio" }, { 1, "Rock" }, { 2, "Jazz" }, { 3, "Brush" }, { 4, "Cajon" },
    { 5, "R&B" }, { 6, "808+909" },
} };

// BEAT: the manual prints "2/4–4/4–7/4, 5/8–15/8" with 4/4 the default, and
// never lists the steps. Anchor: 2 = 4/4 (hardware), which fits 2/4, 3/4, 4/4.
// Every step inside both ranges, the /8 block after the /4 block: inferred.
inline constexpr std::array<Choice, 17> kBeats { {
    { 0, "2/4" }, { 1, "3/4" }, { 2, "4/4" }, { 3, "5/4" }, { 4, "6/4" }, { 5, "7/4" },
    { 6, "5/8" }, { 7, "6/8" }, { 8, "7/8" }, { 9, "8/8" }, { 10, "9/8" }, { 11, "10/8" },
    { 12, "11/8" }, { 13, "12/8" }, { 14, "13/8" }, { 15, "14/8" }, { 16, "15/8" },
} };

// VARIATION: A or B. Anchor: 0 is the factory value and the printed default.
inline constexpr std::array<Choice, 2> kVariations { { { 0, "A" }, { 1, "B" } } };

// --- the ranges (p. 10; the file stores them as printed, except TONE) ---

inline constexpr long long kLevelMax = 200;  // LEVEL 0–200, default 100
inline constexpr long long kReverbMax = 100; // REVERB 0–100, default 30
// TONE LOW / TONE HIGH: the screen prints −10–0–10; the factory body carries
// 10 where the screen shows 0. So the file holds screen + 10, 0..20.
inline constexpr long long kToneMin = -10;
inline constexpr long long kToneMax = 10;
inline constexpr long long kToneFileOffset = 10;

// --- names ---

namespace detail {

    inline std::string_view nameIn(std::span<const Choice> list, std::string_view what,
                                   long long number)
    {
        for (const Choice& choice : list)
            if (choice.number == number)
                return choice.name;
        throw Error("RHYTHM " + std::string(what) + " " + std::to_string(number)
                    + " is not in the manual's list of " + std::to_string(list.size())
                    + " (0.." + std::to_string(list.size() - 1) + ")");
    }

    inline long long rhythm(std::string_view slotBody, std::string_view tag)
    {
        return rc0::sectionField(slotBody, rc0::kSectionRhythm, tag);
    }

    inline std::string setRhythm(std::string_view slotBody, std::string_view tag, long long value)
    {
        return rc0::setSectionField(slotBody, rc0::kSectionRhythm, tag, value);
    }

    inline void requireRange(std::string_view what, long long value, long long min, long long max)
    {
        if (value < min || value > max)
            throw Error("RHYTHM " + std::string(what) + " " + std::to_string(value)
                        + " is outside " + std::to_string(min) + ".." + std::to_string(max));
    }

} // namespace detail

inline std::string_view patternName(long long n) { return detail::nameIn(kPatterns, "PATTERN", n); }
inline std::string_view kitName(long long n) { return detail::nameIn(kKits, "KIT", n); }
inline std::string_view beatName(long long n) { return detail::nameIn(kBeats, "BEAT", n); }
inline std::string_view variationName(long long n)
{
    return detail::nameIn(kVariations, "VARIATION", n);
}

// TONE as the screen shows it, from the file's number and back.
inline long long toneOnScreen(long long fileValue) { return fileValue - kToneFileOffset; }
inline long long toneInFile(long long screenValue) { return screenValue + kToneFileOffset; }

// --- reading ---

// Will the player hear drums with this memory? State alone is not enough:
// with Pattern at Blank the section is on and silent — the count-in's
// arrangement, not a groove.
inline bool isOn(std::string_view slotBody)
{
    return detail::rhythm(slotBody, "State") == rc0::kRhythmStateOn
        && detail::rhythm(slotBody, "Pattern") != rc0::kRhythmPatternBlank;
}

// The manual's one hard rule: BEAT cannot be changed after a track is
// recorded. Every track of the memory counts — a two-track model with a
// take on either one is locked.
inline bool beatLocked(std::string_view slotBody)
{
    for (int track = 1; slotBody.find(rc0::trackSectionOpen(track)) != std::string_view::npos;
         ++track)
        if (rc0::sectionField(slotBody, rc0::trackSectionName(track), "WavStat")
            == rc0::kWavStatIndexed)
            return true;
    return false;
}

// The groove that switching the rhythm OFF would forget, if any: only with
// the count-in on does off mean "Blank" rather than "State off", and only a
// playing groove is at stake. Pattern 0 is a real groove here (SimpleBeat1
// is what the player hears), unlike in the count-in's warning, where it is
// the factory value nobody chose.
inline std::optional<long long> patternLostOnOff(std::string_view slotBody)
{
    if (!isOn(slotBody) || !countin::isOn(slotBody))
        return std::nullopt;
    return detail::rhythm(slotBody, "Pattern");
}

// Everything the card shows, read in one go. Tone values are the screen's.
struct Values {
    bool on;
    long long pattern; // an index into kPatterns — Blank included, it is what the file says
    long long kit;
    long long beat;
    long long variation;
    long long level;
    long long reverb;
    long long toneLow;  // −10..+10, as on the screen
    long long toneHigh; // −10..+10, as on the screen
    bool beatLocked;    // a take is recorded: BEAT is read-only (manual p. 10)

    bool operator==(const Values&) const = default;
};

inline Values read(std::string_view slotBody)
{
    return { isOn(slotBody),
             detail::rhythm(slotBody, "Pattern"),
             detail::rhythm(slotBody, "Kit"),
             detail::rhythm(slotBody, "Beat"),
             detail::rhythm(slotBody, "Variation"),
             detail::rhythm(slotBody, "Level"),
             detail::rhythm(slotBody, "Reverb"),
             toneOnScreen(detail::rhythm(slotBody, "ToneLow")),
             toneOnScreen(detail::rhythm(slotBody, "ToneHigh")),
             beatLocked(slotBody) };
}

// --- the switch ---

// Switch the drums for one slot body. On: State on, and a groove where there
// was Blank — the printed default, since the silence was never a choice of
// groove. Off: State off, unless the count-in needs it, in which case the
// groove becomes Blank. Already there is already there: nothing moves.
inline std::string applySwitch(std::string_view slotBody, bool on)
{
    std::string body(slotBody);
    if (on == isOn(body))
        return body;
    if (on) {
        if (detail::rhythm(body, "Pattern") == rc0::kRhythmPatternBlank)
            body = detail::setRhythm(body, "Pattern", kPatternDefault);
        return detail::setRhythm(body, "State", rc0::kRhythmStateOn);
    }
    if (countin::isOn(body))
        return detail::setRhythm(body, "Pattern", rc0::kRhythmPatternBlank);
    return detail::setRhythm(body, "State", 0);
}

// --- edits ---

// One edit: any subset of the card's fields. An absent field's bytes are
// reproduced exactly. Tone values are the screen's; the file offset is ours.
struct Edits {
    std::optional<bool> on;
    std::optional<long long> pattern;
    std::optional<long long> kit;
    std::optional<long long> beat;
    std::optional<long long> variation;
    std::optional<long long> level;
    std::optional<long long> reverb;
    std::optional<long long> toneLow;
    std::optional<long long> toneHigh;

    bool empty() const
    {
        return !on && !pattern && !kit && !beat && !variation && !level && !reverb && !toneLow
            && !toneHigh;
    }
};

// Apply an edit to a slot body. Every present field is checked before the
// first byte moves — against the manual's lists and ranges, and against the
// slot itself for BEAT — so a refused edit leaves the body untouched. An
// empty edit is a caller bug, not a no-op.
inline std::string apply(std::string_view slotBody, const Edits& edits)
{
    if (edits.empty())
        throw Error("no rhythm setting to change");
    if (edits.pattern) {
        patternName(*edits.pattern);
        if (*edits.pattern == rc0::kRhythmPatternBlank)
            throw Error("Blank is the count-in's silence, not a groove: switch the rhythm off "
                        "instead");
    }
    if (edits.kit)
        kitName(*edits.kit);
    if (edits.beat) {
        beatName(*edits.beat);
        if (beatLocked(slotBody))
            throw Error("BEAT cannot be changed once a track is recorded (RC-5 reference "
                        "manual p. 10)");
    }
    if (edits.variation)
        variationName(*edits.variation);
    if (edits.level)
        detail::requireRange("LEVEL", *edits.level, 0, kLevelMax);
    if (edits.reverb)
        detail::requireRange("REVERB", *edits.reverb, 0, kReverbMax);
    if (edits.toneLow)
        detail::requireRange("TONE LOW", *edits.toneLow, kToneMin, kToneMax);
    if (edits.toneHigh)
        detail::requireRange("TONE HIGH", *edits.toneHigh, kToneMin, kToneMax);

    std::string body(slotBody);
    if (edits.on)
        body = applySwitch(body, *edits.on);
    if (edits.pattern)
        body = detail::setRhythm(body, "Pattern", *edits.pattern);
    if (edits.kit)
        body = detail::setRhythm(body, "Kit", *edits.kit);
    if (edits.beat)
        body = detail::setRhythm(body, "Beat", *edits.beat);
    if (edits.variation)
        body = detail::setRhythm(body, "Variation", *edits.variation);
    if (edits.level)
        body = detail::setRhythm(body, "Level", *edits.level);
    if (edits.reverb)
        body = detail::setRhythm(body, "Reverb", *edits.reverb);
    if (edits.toneLow)
        body = detail::setRhythm(body, "ToneLow", toneInFile(*edits.toneLow));
    if (edits.toneHigh)
        body = detail::setRhythm(body, "ToneHigh", toneInFile(*edits.toneHigh));
    return body;
}

// The edit in the pedal's own words, for the history and the banner:
// "switched on", "pattern Rock1, kit Jazz", "tone low -3". Names are looked
// up, so a number outside a list fails here as loudly as in apply().
inline std::string describe(const Edits& edits)
{
    std::string out;
    const auto add = [&out](std::string_view text) {
        if (!out.empty())
            out += ", ";
        out += text;
    };
    const auto signedNumber = [](long long value) {
        return (value > 0 ? "+" : "") + std::to_string(value);
    };
    if (edits.on)
        add(*edits.on ? "switched on" : "switched off");
    if (edits.pattern)
        add("pattern " + std::string(patternName(*edits.pattern)));
    if (edits.kit)
        add("kit " + std::string(kitName(*edits.kit)));
    if (edits.beat)
        add("beat " + std::string(beatName(*edits.beat)));
    if (edits.variation)
        add("variation " + std::string(variationName(*edits.variation)));
    if (edits.level)
        add("level " + std::to_string(*edits.level));
    if (edits.reverb)
        add("reverb " + std::to_string(*edits.reverb));
    if (edits.toneLow)
        add("tone low " + signedNumber(*edits.toneLow));
    if (edits.toneHigh)
        add("tone high " + signedNumber(*edits.toneHigh));
    return out;
}

} // namespace loopercat::usecases::rhythm
