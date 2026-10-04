// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The name a pushed file lands under (issue #139), from the rule as stated:
// a rebuilt upload is <source stem><suffix>.wav, an untouched one keeps its
// name, the stem keeps its own dots, the suffix may be empty, and a suffix
// a FAT name cannot hold is refused — not stripped.

#include "support.hpp"

#include "../app/UploadMark.h"

#include <string>

using namespace loopercat;

int main()
{
    // --- the default is the spelling existing cards already carry ---
    CHECK_EQ(std::string(uploadmark::kDefaultSuffix), std::string("-pedal"));

    // --- a rebuilt upload: stem + suffix + .wav, the extension changed on purpose ---
    CHECK_EQ(uploadmark::landedName("song.mp3", "-pedal", true), std::string("song-pedal.wav"));
    CHECK_EQ(uploadmark::landedName("take.wav", "-pedal", true), std::string("take-pedal.wav"));
    CHECK_EQ(uploadmark::landedName("Take.WAV", "-pedal", true), std::string("Take-pedal.wav"));

    // --- an empty suffix: the extension still changes, nothing else ---
    CHECK_EQ(uploadmark::landedName("song.mp3", "", true), std::string("song.wav"));
    CHECK_EQ(uploadmark::landedName("loop.aiff", "", true), std::string("loop.wav"));

    // --- an untouched upload keeps its name whatever the suffix says ---
    CHECK_EQ(uploadmark::landedName("take.wav", "-pedal", false), std::string("take.wav"));
    CHECK_EQ(uploadmark::landedName("take.wav", "", false), std::string("take.wav"));
    CHECK_EQ(uploadmark::landedName("Take.WAV", "-pedal", false), std::string("Take.WAV"));

    // --- the stem keeps its own dots; only the last one is the extension ---
    CHECK_EQ(uploadmark::landedName("v1.2 mix.aif", "-pedal", true),
             std::string("v1.2 mix-pedal.wav"));
    CHECK_EQ(uploadmark::landedName("set.final.v3.flac", " live", true),
             std::string("set.final.v3 live.wav"));

    // --- no extension at all: the whole name is the stem ---
    CHECK_EQ(uploadmark::landedName("song", "-pedal", true), std::string("song-pedal.wav"));
    // a leading dot is not an extension either
    CHECK_EQ(uploadmark::landedName(".hidden", "-pedal", true), std::string(".hidden-pedal.wav"));
    // a trailing dot is an empty extension: the stem ends before it
    CHECK_EQ(uploadmark::landedName("song.", "-pedal", true), std::string("song-pedal.wav"));

    // --- a UTF-8 name survives byte for byte ---
    CHECK_EQ(uploadmark::landedName("caf\xc3\xa9 take.mp3", "\xe2\x80\x93pedal", true),
             std::string("caf\xc3\xa9 take\xe2\x80\x93pedal.wav"));

    // --- a suffix a FAT name cannot hold is refused, with the character named ---
    for (const char c : std::string("/\\:*?\"<>|")) {
        const std::string bad = std::string("-ped") + c + "al";
        CHECK_THROWS(uploadmark::assertSuffix(bad), std::string(1, c));
        CHECK_THROWS(uploadmark::landedName("song.mp3", bad, true), std::string(1, c));
        // the refusal holds for an untouched upload too: a bad mark is a
        // bad setting, whichever file meets it first
        CHECK_THROWS(uploadmark::landedName("take.wav", bad, false), std::string(1, c));
    }
    CHECK_THROWS(uploadmark::assertSuffix(std::string("-pedal\t")), "control");
    CHECK_THROWS(uploadmark::assertSuffix(std::string("-pedal\n")), "control");
    CHECK_THROWS(uploadmark::assertSuffix(std::string("\x01", 1)), "control");

    // --- what a FAT name can hold is accepted as typed: spaces, dots, unicode ---
    uploadmark::assertSuffix("");
    uploadmark::assertSuffix(" (pedal)");
    uploadmark::assertSuffix("-v1.2");
    uploadmark::assertSuffix("\xe2\x80\x93pedal");
    uploadmark::assertSuffix("_PEDAL_");
    ++testkit::checksRun; // the five lines above threw nothing

    return testkit::summary("upload_mark");
}
