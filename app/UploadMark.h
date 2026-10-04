// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <string>
#include <string_view>

//==============================================================================
// loopercat::uploadmark — the name a pushed file lands under on the card
// (issue #139). Pure: no files, no JUCE, so the rule is tested as a rule.
//
// An upload the pedal takes as-is keeps its own name. One that had to be
// rebuilt — resampled, spread to two channels, re-quantised, gain-adjusted —
// lands as <source stem><suffix>.wav: the bytes in that slot are not the
// bytes of the player's file, and a take pulled back a month later should
// say so at a glance. The extension changes on purpose, so an imported
// song.mp3 becomes song-pedal.wav rather than keeping an extension that lies
// about what is inside.
//
// The suffix is the player's (Settings -> Import); empty means no mark. Before
// this rule existed the suffix was the name of the conversion's temp file,
// travelling to the card by accident — the default keeps that spelling so an
// existing card keeps the names it already has.
//==============================================================================
namespace loopercat::uploadmark
{

inline constexpr std::string_view kDefaultSuffix = "-pedal";

// Characters a FAT long file name cannot hold (Microsoft, "Naming Files,
// Paths, and Namespaces": the reserved set, plus the control range). The
// card is FAT; a suffix with one of these would fail at the write, on the
// pedal, after the conversion — so it is refused at the field instead.
inline constexpr std::string_view kForbidden = "/\\:*?\"<>|";

// Refuses, with the character named, rather than stripping it: a mark the
// player typed and a mark the app silently rewrote are two different marks.
inline void assertSuffix(std::string_view suffix)
{
    for (const char c : suffix) {
        if (kForbidden.find(c) != std::string_view::npos)
            throw Error(std::string("the mark cannot contain \"") + c
                        + "\" — a file name on the card cannot hold it");
        if (static_cast<unsigned char>(c) < 0x20)
            throw Error("the mark cannot contain a control character");
    }
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

// The file name on the card for `sourceFileName`: unchanged when the audio
// passed the pedal's gate untouched; <stem><suffix>.wav when it was rebuilt.
inline std::string landedName(std::string_view sourceFileName, std::string_view suffix,
                              bool converted)
{
    assertSuffix(suffix);
    if (!converted)
        return std::string(sourceFileName);
    return stem(sourceFileName) + std::string(suffix) + ".wav";
}

} // namespace loopercat::uploadmark
