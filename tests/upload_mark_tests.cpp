// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The name a pushed file lands under (issue #139), from the rule as stated:
// a rebuilt upload is <source stem><suffix>.wav, a repacked one is
// <source stem>.wav, an untouched one keeps its name, the stem keeps its own
// dots, the suffix may be empty, a suffix a FAT name cannot hold is refused —
// not stripped — and only where it is used, and a name longer than a FAT name
// can be is refused, never trimmed.

#include "support.hpp"

#include "../app/UploadMark.h"

#include <exception>
#include <string>

using namespace loopercat;

int run()
{
    using uploadmark::Audio;

    // --- the default is the spelling existing cards already carry ---
    CHECK_EQ(std::string(uploadmark::kDefaultSuffix), std::string("-pedal"));

    // --- a rebuilt upload: stem + suffix + .wav, the extension changed on purpose ---
    CHECK_EQ(uploadmark::landedName("song.mp3", "-pedal", Audio::rebuilt), std::string("song-pedal.wav"));
    CHECK_EQ(uploadmark::landedName("take.wav", "-pedal", Audio::rebuilt), std::string("take-pedal.wav"));
    CHECK_EQ(uploadmark::landedName("Take.WAV", "-pedal", Audio::rebuilt), std::string("Take-pedal.wav"));

    // --- an empty suffix: the extension still changes, nothing else ---
    CHECK_EQ(uploadmark::landedName("song.mp3", "", Audio::rebuilt), std::string("song.wav"));
    CHECK_EQ(uploadmark::landedName("loop.aiff", "", Audio::rebuilt), std::string("loop.wav"));

    // --- a repacked upload: its own stem, the honest extension, no mark whatever the suffix ---
    CHECK_EQ(uploadmark::landedName("song.mp3", "-pedal", Audio::repackaged), std::string("song.wav"));
    CHECK_EQ(uploadmark::landedName("Take.WAV", "-pedal", Audio::repackaged), std::string("Take.wav"));
    CHECK_EQ(uploadmark::landedName("v1.2 mix.aif", "-pedal", Audio::repackaged),
             std::string("v1.2 mix.wav"));
    CHECK_EQ(uploadmark::landedName("export.wav", "", Audio::repackaged), std::string("export.wav"));

    // --- an untouched upload keeps its name whatever the suffix says ---
    CHECK_EQ(uploadmark::landedName("take.wav", "-pedal", Audio::untouched), std::string("take.wav"));
    CHECK_EQ(uploadmark::landedName("take.wav", "", Audio::untouched), std::string("take.wav"));
    CHECK_EQ(uploadmark::landedName("Take.WAV", "-pedal", Audio::untouched), std::string("Take.WAV"));

    // --- the stem keeps its own dots; only the last one is the extension ---
    CHECK_EQ(uploadmark::landedName("v1.2 mix.aif", "-pedal", Audio::rebuilt),
             std::string("v1.2 mix-pedal.wav"));
    CHECK_EQ(uploadmark::landedName("set.final.v3.flac", " live", Audio::rebuilt),
             std::string("set.final.v3 live.wav"));

    // --- no extension at all: the whole name is the stem ---
    CHECK_EQ(uploadmark::landedName("song", "-pedal", Audio::rebuilt), std::string("song-pedal.wav"));
    // a leading dot is not an extension either
    CHECK_EQ(uploadmark::landedName(".hidden", "-pedal", Audio::rebuilt), std::string(".hidden-pedal.wav"));
    // a trailing dot is an empty extension: the stem ends before it
    CHECK_EQ(uploadmark::landedName("song.", "-pedal", Audio::rebuilt), std::string("song-pedal.wav"));

    // --- a UTF-8 name survives byte for byte ---
    CHECK_EQ(uploadmark::landedName("caf\xc3\xa9 take.mp3", "\xe2\x80\x93pedal", Audio::rebuilt),
             std::string("caf\xc3\xa9 take\xe2\x80\x93pedal.wav"));

    // --- a suffix a FAT name cannot hold is refused, with the character named ---
    for (const char c : std::string("/\\:*?\"<>|")) {
        const std::string bad = std::string("-ped") + c + "al";
        CHECK_THROWS(uploadmark::assertSuffix(bad), std::string(1, c));
        CHECK_THROWS(uploadmark::landedName("song.mp3", bad, Audio::rebuilt), std::string(1, c));
        // ...only where the mark is used: a bad mark hand-edited into the
        // settings file must not stop an upload that never wears it
        CHECK_EQ(uploadmark::landedName("take.wav", bad, Audio::untouched), std::string("take.wav"));
        CHECK_EQ(uploadmark::landedName("take.wav", bad, Audio::repackaged), std::string("take.wav"));
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

    // --- the card's limit: 238 UTF-16 units for the whole name, counted, never trimmed ---
    //
    // FAT's own 255 is not the binding limit: the card is read on Windows,
    // whose MAX_PATH (260, terminator included) leaves 238 under the
    // 21-character card folder (fatname::kMaxLandedUnits). The review's
    // reproductions — a 247-character stem with "-pedal", a 252-character
    // stem, a 260-character mark, and the 239-unit name that passed the
    // first cap — are all refused here, before anything reaches the card.
    CHECK_EQ(uploadmark::kMaxSuffixUnits, std::size_t { 233 });
    {
        const std::string stem228(228, 's');
        CHECK_EQ(uploadmark::landedName(stem228 + ".mp3", "-pedal", Audio::rebuilt).size(),
                 std::size_t { 238 }); // exactly the limit: allowed
        const std::string stem229(229, 's');
        CHECK_THROWS(uploadmark::landedName(stem229 + ".mp3", "-pedal", Audio::rebuilt), "at most 238");
        const std::string stem230(230, 's');
        CHECK_THROWS(uploadmark::landedName(stem230 + ".mp3", "-pedal", Audio::rebuilt), "or the mark");
        // the same stem with no mark fits: it was the mark that pushed it over
        CHECK_EQ(uploadmark::landedName(stem230 + ".mp3", "", Audio::rebuilt).size(), std::size_t { 234 });
        const std::string stem247(247, 's');
        CHECK_THROWS(uploadmark::landedName(stem247 + ".mp3", "-pedal", Audio::rebuilt), "at most 238");
        // repacked names are measured too, and the advice names the file only
        const std::string stem235(235, 's');
        CHECK_THROWS(uploadmark::landedName(stem235 + ".mp3", "", Audio::repackaged), "shorten the file name");
        const std::string stem252(252, 's');
        CHECK_THROWS(uploadmark::landedName(stem252 + ".mp3", "", Audio::repackaged), "at most 238");
        // an untouched name is the host's own and is not measured here (the core does)
        CHECK_EQ(uploadmark::landedName(std::string(300, 'u') + ".wav", "-pedal", Audio::untouched).size(),
                 std::size_t { 304 });
    }
    {
        const std::string mark260(260, 'm');
        CHECK_THROWS(uploadmark::assertSuffix(mark260), "at most 233");
        CHECK_THROWS(uploadmark::landedName("song.mp3", mark260, Audio::rebuilt), "at most 233");
        const std::string mark234(234, 'm');
        CHECK_THROWS(uploadmark::assertSuffix(mark234), "at most 233");
        const std::string mark233(233, 'm');
        uploadmark::assertSuffix(mark233);
        ++testkit::checksRun; // the longest mark a name can carry
        // ...and with the shortest stem it is exactly the limit
        CHECK_EQ(uploadmark::landedName("s.mp3", mark233, Audio::rebuilt).size(), std::size_t { 238 });
    }
    {
        // units, not bytes: 100 "é" are 200 bytes and 100 units
        std::string accented;
        for (int i = 0; i < 100; ++i)
            accented += "\xc3\xa9";
        const std::string landed = uploadmark::landedName(accented + ".mp3", "-pedal", Audio::rebuilt);
        CHECK_EQ(landed.size(), std::size_t { 210 });
        // the limit is in units: a name of 238 units may be 239 bytes...
        const std::string stem233(233, 's');
        CHECK_EQ(uploadmark::landedName(stem233 + ".mp3", "\xc3\xa9", Audio::rebuilt).size(),
                 std::size_t { 239 }); // 233 + 1 unit + 4 = 238 units in 239 bytes: allowed
        // ...or 240 bytes, when the extra character is a surrogate pair (2 units)
        const std::string stem232(232, 's');
        CHECK_EQ(uploadmark::landedName(stem232 + ".mp3", "\xf0\x9f\x98\x80", Audio::rebuilt).size(),
                 std::size_t { 240 }); // 232 + 2 units (4 bytes) + 4 = 238 units in 240 bytes: allowed
        // and the pair on a stem one longer is the 239th unit
        CHECK_THROWS(uploadmark::landedName(stem233 + ".mp3", "\xf0\x9f\x98\x80", Audio::rebuilt),
                     "at most 238");
    }

    return testkit::summary("upload_mark");
}

// A refusal the suite did not expect must name itself, not abort the process
// with no line (the Windows lane reports an abort as a bare exit code).
int main()
{
    try {
        return run();
    } catch (const std::exception& e) {
        testkit::fail(std::string("escaped: ") + e.what(), __FILE__, __LINE__);
        return testkit::summary("upload_mark");
    }
}
