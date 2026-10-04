// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What a file name on the card may be (FatName.hpp), from FAT's own rules
// and Windows's: the reserved characters and the control range, the 255
// UTF-16 units a long name holds — counted as units, not bytes — and the
// device names Windows will not open, judged the way Windows judges them.

#include "support.hpp"

#include <loopercat/FatName.hpp>

#include <string>

using namespace loopercat;

int main()
{
    // --- the limit is FAT's: 255 units ---
    CHECK_EQ(fatname::kMaxUnits, std::size_t { 255 });

    // --- units, not bytes: ASCII one each, BMP one each, astral two ---
    CHECK_EQ(fatname::utf16Units(""), std::size_t { 0 });
    CHECK_EQ(fatname::utf16Units("abc.wav"), std::size_t { 7 });
    CHECK_EQ(fatname::utf16Units("caf\xc3\xa9"), std::size_t { 4 });              // é: 2 bytes, 1 unit
    CHECK_EQ(fatname::utf16Units("\xe2\x86\x92"), std::size_t { 1 });             // →: 3 bytes, 1 unit
    CHECK_EQ(fatname::utf16Units("\xf0\x9f\x98\x80"), std::size_t { 2 });         // 😀: 4 bytes, 2 units
    CHECK_EQ(fatname::utf16Units("a\xf0\x9f\x98\x80z"), std::size_t { 4 });
    CHECK_EQ(fatname::utf16Units(std::string(255, 'a')), std::size_t { 255 });
    CHECK_EQ(fatname::utf16Units(std::string(256, 'a')), std::size_t { 256 });

    // --- the reserved nine and the control range are refused by name ---
    CHECK_EQ(std::string(fatname::kReserved), std::string("/\\:*?\"<>|"));
    for (const char c : std::string("/\\:*?\"<>|")) {
        const auto found = fatname::forbiddenCharacter(std::string("ta") + c + "ke");
        CHECK(found.has_value());
        if (found.has_value()) {
            CHECK_EQ(*found, c);
            CHECK_EQ(fatname::describeCharacter(*found), std::string("\"") + c + "\"");
        }
    }
    for (const char c : { '\0', '\t', '\n', '\x1f' }) {
        const auto found = fatname::forbiddenCharacter(std::string("ta") + c + "ke");
        CHECK(found.has_value());
        if (found.has_value()) {
            CHECK(fatname::isControl(*found));
            CHECK_EQ(fatname::describeCharacter(*found), std::string("a control character"));
        }
    }
    // the first offender is the one named
    {
        const auto found = fatname::forbiddenCharacter("a?b:c");
        CHECK(found.has_value() && *found == '?');
    }

    // --- what a FAT name may hold is not refused: spaces, dots, unicode, 0x7f ---
    CHECK(!fatname::forbiddenCharacter("").has_value());
    CHECK(!fatname::forbiddenCharacter("My Song (take 2).wav").has_value());
    CHECK(!fatname::forbiddenCharacter("caf\xc3\xa9 \xe2\x80\x93 live.wav").has_value());
    CHECK(!fatname::forbiddenCharacter("v1.2 mix-pedal.wav").has_value());
    CHECK(!fatname::forbiddenCharacter(std::string("a\x7f", 2)).has_value());

    // --- Windows device names, before the first dot, in any case ---
    for (const char* reserved : { "CON", "con", "Con.wav", "PRN.wav", "aux.wav", "NUL.wav",
                                  "COM1", "com9.wav", "LPT1.wav", "lpt9", "Com1.take.wav" })
        CHECK(fatname::isDeviceName(reserved));
    for (const char* plain : { "", "CONSOLE.wav", "COM10.wav", "COM0.wav", "LPT.wav", "COM.wav",
                               "aux-pedal.wav", "nul1.wav", "song.wav", ".wav" })
        CHECK(!fatname::isDeviceName(plain));

    return testkit::summary("fat_name");
}
