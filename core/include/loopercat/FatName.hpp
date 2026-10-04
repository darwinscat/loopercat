// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace loopercat::fatname {

// What a file name on the card may be. The card is FAT and is read by every
// host OS the pedal meets, so the rules are FAT's long-name rules plus the
// names Windows refuses to open. One place for them: the core checks a name
// before a byte moves (Commands.hpp, push), the app checks a mark before it
// is stored (app/UploadMark.h), and both must agree.

// Characters a FAT long file name cannot hold: the reserved nine (Microsoft,
// "Naming Files, Paths, and Namespaces"), plus the control range 0x00-0x1F
// from the same page.
inline constexpr std::string_view kReserved = "/\\:*?\"<>|";

inline bool isControl(char c) { return static_cast<unsigned char>(c) < 0x20; }

// A FAT long name holds at most 255 UTF-16 units (Microsoft FAT32 File
// System Specification, long directory entries: 13 units per entry, 20
// entries at most, 255 after the terminator).
inline constexpr std::size_t kMaxUnits = 255;

// The first character `text` holds that a FAT name cannot; empty when none.
inline std::optional<char> forbiddenCharacter(std::string_view text)
{
    for (const char c : text)
        if (kReserved.find(c) != std::string_view::npos || isControl(c))
            return c;
    return std::nullopt;
}

// How a refusal names such a character: the character itself, quoted, or
// "a control character" for one that has no face.
inline std::string describeCharacter(char c)
{
    return isControl(c) ? std::string("a control character") : std::string("\"") + c + "\"";
}

// The UTF-16 units of a UTF-8 string — what the FAT limit counts. A lead
// byte starts a code point (one unit); a 4-byte lead starts a supplementary
// one (a surrogate pair, two units); continuation bytes add nothing. The
// string is taken as well-formed UTF-8: every name here comes from a path the
// host OS produced or from a juce::String.
inline std::size_t utf16Units(std::string_view utf8)
{
    std::size_t units = 0;
    for (const char ch : utf8) {
        const auto b = static_cast<unsigned char>(ch);
        if ((b & 0xC0) == 0x80)
            continue;
        units += (b & 0xF8) == 0xF0 ? 2 : 1;
    }
    return units;
}

// The names Windows reserves for devices — CON, PRN, AUX, NUL, COM1-9,
// LPT1-9 — judged on the part before the FIRST dot, case-insensitively,
// which is Windows's own rule: "con.wav" is as unopenable as "CON".
inline bool isDeviceName(std::string_view name)
{
    std::string head(name.substr(0, name.find('.')));
    for (char& c : head)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (head == "CON" || head == "PRN" || head == "AUX" || head == "NUL")
        return true;
    return head.size() == 4 && (head.starts_with("COM") || head.starts_with("LPT"))
        && head[3] >= '1' && head[3] <= '9';
}

} // namespace loopercat::fatname
