// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// SYSTEM*.RC0 — the pedal's settings for itself, as opposed to one memory.
//
// The card carries two files of each kind, and both kinds are the same shape:
// the RC-5 family header, a document, and a trailer of "\n" plus a 4-byte
// little-endian write counter. What differs is the body. A memory file holds
// 99 <mem> entries; this one holds a single <sys> with three sections:
//
//   SETUP  the pedal's own state and display (MemoryNumber is the memory it
//          currently has selected — the pedal's property, never ours)
//   MIDI   channels, clock sync, program change, thru
//   CTL    what each footswitch, the expression pedal and CC#80..87 do
//
// Read off real hardware (fw 1.10) — fixtures/rc5-system.RC0 is that file.
// The whole document is 717 bytes, so nothing here needs to be clever; what
// it needs to be is strict, because this file is how a pedal knows how to
// behave, and a structurally wrong one is a pedal that will not boot.
//
// Nothing in the app writes a SYSTEM file yet. This module is the reader and
// the gate: it says what a SYSTEM file IS, so that the day something writes
// one, "it parsed" and "it is a SYSTEM file" are the same statement.

#pragma once

#include "Error.hpp"
#include "Rc0.hpp"

#include <string>
#include <string_view>

namespace loopercat::sysfile {

// The single element inside <database> — and the three sections inside it.
inline constexpr std::string_view kRoot = "sys";
inline constexpr std::string_view kSectionSetup = "SETUP";
inline constexpr std::string_view kSectionMidi = "MIDI";
inline constexpr std::string_view kSectionCtl = "CTL";

// The memory the pedal currently has selected, kept in SETUP. It belongs to
// the pedal: we read it, we never decide it, and an edit of anything else
// must leave it exactly as found.
inline constexpr std::string_view kCurrentMemoryField = "MemoryNumber";

// A structurally sound SYSTEM file of a known family (the same guard the
// memory files pass — the root names the model), one <sys> element, and the
// three sections — each exactly once, which rc0::sectionField enforces on
// every lookup.
//
// A memory file is refused here, and a system file is refused by
// rc0::assertMemoryFile: the two are told apart by what they contain, not by
// the name they were loaded from. Reading one where the other is expected is
// how a mutation ends up in the wrong file.
inline void assertSystemFile(std::string_view text)
{
    const std::string document = rc0::splitFile(text).document;
    rc0::familyOf(document); // a known model, or a refusal by name
    if (document.find("<mem id=\"") != std::string::npos)
        throw Error("this is a memory file, not a SYSTEM file: it carries <mem> entries");
    const std::string open = "<" + std::string(kRoot) + ">";
    if (document.find(open) == std::string::npos)
        throw Error("not a SYSTEM file: no <sys> element");
    for (const std::string_view section : { kSectionSetup, kSectionMidi, kSectionCtl })
        rc0::detail::sectionRegion(document, section); // throws if missing, doubled or unclosed
}

// One field of one section, by the same rule as a memory's fields.
inline long long field(std::string_view text, std::string_view section, std::string_view tag)
{
    return rc0::sectionField(text, section, tag);
}

// Rewrite one field of one section; every other byte of the file, the trailer
// and MemoryNumber included, is reproduced exactly.
inline std::string setField(std::string_view text, std::string_view section,
                            std::string_view tag, long long value)
{
    return rc0::setSectionField(text, section, tag, value);
}

// The memory the pedal has selected right now.
inline long long currentMemory(std::string_view text)
{
    return field(text, kSectionSetup, kCurrentMemoryField);
}

} // namespace loopercat::sysfile
