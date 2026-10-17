// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>
#include <loopercat/FatName.hpp>

#include <cstddef>
#include <string>
#include <string_view>

//==============================================================================
// loopercat::uploadmark — the name a pushed file lands under on the card
// (issue #139). Pure: no files, no JUCE, so the rule is tested as a rule.
//
// An upload the pedal takes as-is keeps its own name. One whose samples had
// to be rebuilt — resampled, spread to two channels, re-quantised,
// gain-adjusted — lands as <source stem><suffix>.wav: the bytes in that slot
// are not the bytes of the player's file, and a take pulled back a month
// later should say so at a glance. One that was only repacked into the
// pedal's container — a float export wearing a header the pedal's gate does
// not read, every sample intact — lands as <source stem>.wav: the extension
// is changed on purpose, so an imported song.mp3 becomes song.wav rather than
// keeping an extension that lies about what is inside, but there is nothing
// to mark.
//
// The suffix is the player's (Settings -> Import); empty means no mark. Before
// this rule existed the suffix was the name of the conversion's temp file,
// travelling to the card by accident — the default keeps that spelling so an
// existing card keeps the names it already has.
//==============================================================================
namespace loopercat::uploadmark
{

inline constexpr std::string_view kDefaultSuffix = "-pedal";
inline constexpr std::string_view kExtension = ".wav";

// A mark longer than this leaves no room for a name in front of it: the
// card's limit (fatname::kMaxLandedUnits, what Windows's MAX_PATH leaves
// under the card folder), less the extension, less one character of stem.
inline constexpr std::size_t kMaxSuffixUnits = fatname::kMaxLandedUnits - kExtension.size() - 1;
static_assert(kMaxSuffixUnits == 233);

// What happened to the audio on its way to the card.
enum class Audio {
    untouched,  // the source file itself goes to the card: its name stays
    repackaged, // the same samples in the pedal's own container: <stem>.wav, no mark
    rebuilt     // the samples changed: <stem><suffix>.wav
};

// Refuses, with the character named, rather than stripping it: a mark the
// player typed and a mark the app silently rewrote are two different marks.
// The cap is a named one so the Settings field can refuse a mark no name
// could carry.
inline void assertSuffix(std::string_view suffix)
{
    if (const auto c = fatname::forbiddenCharacter(suffix))
        throw Error("the mark cannot contain " + fatname::describeCharacter(*c)
                    + " — a file name on the card cannot hold it");
    const std::size_t units = fatname::utf16Units(suffix);
    if (units > kMaxSuffixUnits)
        throw Error("the mark is " + std::to_string(units) + " characters long; it can be at most "
                    + std::to_string(kMaxSuffixUnits) + " — a file name on the card holds "
                    + std::to_string(fatname::kMaxLandedUnits) + ", and the name needs room in front");
}

// The stem is everything before the last dot; a name with no dot, or only a
// leading one, is all stem — "v1.2 mix.aif" keeps its "1.2". Done by hand
// rather than through std::filesystem so a UTF-8 name survives byte-exact on
// every platform.
inline std::string stem(std::string_view fileName)
{
    const auto dot = fileName.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return std::string(fileName);
    return std::string(fileName.substr(0, dot));
}

// The file name on the card for `sourceFileName`, by what happened to its
// audio. The suffix is judged only where it is used — a mark that was
// hand-edited into the settings file must not stop an untouched upload.
// A name longer than a FAT name can be is refused, never trimmed: the core
// would refuse it before a byte moved, and the player should hear why from
// the app, with the name it would have been.
inline std::string landedName(std::string_view sourceFileName, std::string_view suffix,
                              Audio audio)
{
    if (audio == Audio::untouched)
        return std::string(sourceFileName);
    if (audio == Audio::rebuilt)
        assertSuffix(suffix);
    const std::string name = stem(sourceFileName)
                           + (audio == Audio::rebuilt ? std::string(suffix) : std::string())
                           + std::string(kExtension);
    const std::size_t units = fatname::utf16Units(name);
    if (units > fatname::kMaxLandedUnits)
        throw Error("\"" + name + "\" is " + std::to_string(units)
                    + " characters long; a file name on the card holds at most "
                    + std::to_string(fatname::kMaxLandedUnits) + " — shorten the file name"
                    + (audio == Audio::rebuilt ? " or the mark" : ""));
    return name;
}

} // namespace loopercat::uploadmark
