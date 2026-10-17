// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// BEAT, the time signature of a memory's rhythm (RC-5 reference manual
// p. 10), shared by the two features that write into the RHYTHM section:
// the rhythm itself (Rhythm.hpp) and Play Count-In (CountIn.hpp). It lives
// apart from both because of one hardware fact both have to obey (#147,
// 2026-09-30 and 2026-10-01): the number in <Pattern> indexes the pattern
// list of the memory's CURRENT BEAT, and only the 4/4 list is charted —
// Rock2 chosen on the pedal is stored as 12 at 4/4 and as 3 at 6/4, and a
// 19 written at 6/4 shows a blank name on the screen. So a PATTERN number,
// whichever feature writes it, is a 4/4 number, and at every other beat the
// write is refused (#149). The range cannot tell: a short list's numbers all
// fall inside the 4/4 list. Only the beat can.

#pragma once

#include "../Error.hpp"
#include "Choice.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace loopercat::usecases::beat {

// BEAT: the manual prints "2/4–4/4–7/4, 5/8–15/8" with 4/4 the default, and
// never lists the steps. Anchor: 2 = 4/4 (hardware), which fits 2/4, 3/4, 4/4.
// Every step inside both ranges, the /8 block after the /4 block: inferred.
inline constexpr std::array<Choice, 17> kBeats { {
    { 0, "2/4" }, { 1, "3/4" }, { 2, "4/4" }, { 3, "5/4" }, { 4, "6/4" }, { 5, "7/4" },
    { 6, "5/8" }, { 7, "6/8" }, { 8, "7/8" }, { 9, "8/8" }, { 10, "9/8" }, { 11, "10/8" },
    { 12, "11/8" }, { 13, "12/8" }, { 14, "13/8" }, { 15, "14/8" }, { 16, "15/8" },
} };

// The one beat whose pattern list is charted: 4/4, the manual's printed list
// (hardware: Beat 2 reads 4/4 on the screen, and Rock2 written there is
// stored as 12, where the printed list has it — 2026-10-01, #147).
inline constexpr long long kBeatFourFour = 2;

// Does this beat have a pattern list we can name numbers from? Only at 4/4.
// A beat outside kBeats has no charted list either — the answer is no, not
// an error, so a memory can always be read; acting on it is what fails.
inline constexpr bool patternListCharted(long long beat) { return beat == kBeatFourFour; }

// The fact behind every refusal that follows from it, one sentence for both
// features, so the banner and the log never say it two ways.
inline constexpr std::string_view kOnlyFourFourCharted
    = "only the 4/4 list is charted (hardware, 2026-10-01)";

// The name at `n` — "6/4" — or the typed error carrying the number: for an
// action on BEAT itself, where a number outside the list must fail loudly.
inline std::string_view beatName(long long n) { return usecases::nameIn(kBeats, "RHYTHM BEAT", n); }

// The same name, or nothing for a number the list does not have: for a label.
inline std::optional<std::string_view> nameIfListed(long long n)
{
    for (const Choice& choice : kBeats)
        if (choice.number == n)
            return choice.name;
    return std::nullopt;
}

// The beat as a sentence names it: as the screen prints it ("6/4"), or the
// truth about a number the manual's list does not have. kBeats is inferred
// past its one anchor, so a memory can hold a number it lacks, and a sentence
// about that memory must not throw where an action would: a caption is
// painted on every snapshot, a refusal is thrown once.
inline std::string label(long long n)
{
    if (const auto named = nameIfListed(n))
        return std::string(*named);
    return "BEAT " + std::to_string(n) + " (not in the manual's list)";
}

} // namespace loopercat::usecases::beat
