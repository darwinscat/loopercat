// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// MEMORY*.RC0 model — byte-safe surgical editing.
//
// The RC-5's own parser is strict and every memory file carries a trailer
// after </database>: "\n" plus a 4-byte little-endian WRITE COUNTER. The
// factory-fresh pair is MEMORY1=0x38, MEMORY2=0x39; a save on the pedal
// stamps the freshly written bank one generation past the other (observed
// live 2026-07-24: a pedal-side recording produced 0x3a next to 0x39).
// The counter is a full uint32, not a byte: field files from a fw 1.10
// (build 0050) pedal carried SYSTEM 0x0524/0x0525 and MEMORY
// 0x3e65736e/0x3e65736f — pairs consecutive as 32-bit values, high bytes
// nowhere near zero (2026-08-09). The pedal never validates the VALUE —
// it increments whatever it finds; only a structurally broken trailer
// (wrong shape after </database>) triggers "LOOPER DATA READ ERR" on boot.
// The invariant of this module: any byte we were not explicitly asked to
// change is reproduced exactly. Files are handled as raw byte strings
// (std::string carries arbitrary bytes) so the trailer and any non-ASCII
// bytes round-trip unharmed.
//
// Format knowledge source: rc5cat (lib/rc0.js, macos/RC5Kit), values verified
// against real hardware — see fixtures/golden.json.

#pragma once

#include "DeviceProfile.hpp"
#include "Error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace loopercat::rc0 {

inline constexpr int kSlotCount = 99;
inline constexpr int kNameLength = 12;

// The RHYTHM value map, read back from the pedal's screens (issue #34,
// hardware 2026-08-10): State=1 recalls the memory with its rhythm already
// enabled, PlayCount=1 shows as "PLAY COUNT: 1MEAS", Pattern=57 as
// "PATTERN: Blank" — a 0-based index into the reference manual's pattern
// list. Together the triple is the count-in preset: the pedal plays one
// measure of count-in at the memory tempo, then the track with the rhythm
// silent. The count-in is its own sound, independent of the pattern.
inline constexpr long long kRhythmStateOn = 1;
inline constexpr long long kRhythmPlayCount1Meas = 1;
inline constexpr long long kRhythmPatternBlank = 57;

// TRACK1.WavStat, the pedal's own index state for the slot's take: 0 = no take,
// 1 = a take indexed and playable, 2 = a file present the pedal would not
// index (a non-float32 upload). Pedal-owned; values seen on hardware
// (docs/pedal-settings.md). push sets 1 together with the audio it writes, the
// catalog and doctor read 1 as "this slot holds a take".
inline constexpr long long kWavStatNone = 0;
inline constexpr long long kWavStatIndexed = 1;

// The factory-fresh generation pair (golden.json tailMarkers). The trailer
// is really a write-generation counter — see setTailGeneration; this
// pair is where a pedal starts counting and where a healed volume restarts.
inline std::uint32_t tailMarkerFor(int fileNo)
{
    switch (fileNo) {
    case 1: return 0x38;
    case 2: return 0x39;
    default: throw Error("fileNo must be 1 or 2, got " + std::to_string(fileNo));
    }
}

inline constexpr std::string_view kClosingTag = "</database>";

struct SplitFile {
    std::string document; // up to and including </database>
    std::string tail;     // everything after, byte-preserved
};

inline SplitFile splitFile(std::string_view text)
{
    const auto at = text.find(kClosingTag);
    if (at == std::string_view::npos)
        throw Error("not an RC0 memory file: missing </database>");
    const auto end = at + kClosingTag.size();
    return { std::string(text.substr(0, end)), std::string(text.substr(end)) };
}

namespace detail {

    inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

    // Digits at [begin, …) parsed as long long; a run too long for the type is
    // a malformed file, reported as our own error rather than std::out_of_range.
    inline long long parseInteger(std::string_view digits, std::string_view what)
    {
        try {
            return std::stoll(std::string(digits));
        } catch (const std::out_of_range&) {
            throw Error(std::string(what) + " value out of range: " + std::string(digits));
        }
    }

    // All occurrences of the exact shape <tag>integer</tag>, in text order.
    // Anything else between the tags (nested elements, non-digits) is not a
    // match — mirrors the reference implementation's strict field regex.
    struct FieldMatch {
        std::size_t begin; // offset of '<' of the opening tag
        std::size_t end;   // offset one past '>' of the closing tag
        long long value;
    };

    inline std::vector<FieldMatch> findFields(std::string_view body, std::string_view tag)
    {
        const std::string open = "<" + std::string(tag) + ">";
        const std::string close = "</" + std::string(tag) + ">";
        std::vector<FieldMatch> out;
        std::size_t from = 0;
        while (true) {
            const auto at = body.find(open, from);
            if (at == std::string_view::npos)
                break;
            from = at + 1;
            std::size_t digits = at + open.size();
            if (digits < body.size() && body[digits] == '-')
                ++digits;
            const std::size_t firstDigit = digits;
            while (digits < body.size() && isDigit(body[digits]))
                ++digits;
            if (digits == firstDigit)
                continue; // no integer here — not a field match
            if (body.compare(digits, close.size(), close) != 0)
                continue;
            const auto valueBegin = at + open.size();
            out.push_back({ at, digits + close.size(),
                            parseInteger(body.substr(valueBegin, digits - valueBegin), open) });
        }
        return out;
    }

    inline FieldMatch theOneField(std::string_view body, std::string_view tag)
    {
        const auto matches = findFields(body, tag);
        if (matches.size() != 1)
            throw Error("<" + std::string(tag) + "> occurs " + std::to_string(matches.size())
                        + " times, expected exactly 1");
        return matches.front();
    }

} // namespace detail

// --- the family guard (issue #35), by profile ---
//
// Every RC-series pedal exports the same ROLAND/DATA card layout, and a
// two-track model's MEMORY*.RC0 is near-identical to the RC-5's: the same 99
// <mem id="0..98"> entries, the same <NAME>/C01..C12 block, the same TRACK
// field names, even the same `Measure = MeasLen + 7` — hardware dumps of the
// two-track family obey it on every recorded track. What differs is the
// shape: a second track per memory, with its own <TRACK2> section and its own
// audio directory, plus sections the RC-5 does not have. Mutating such a card
// with one-track semantics is data corruption — a swap would carry one
// track's audio across and leave the other behind — so a card is identified
// at the door, by its profile (DeviceProfile.hpp), and everything after asks
// that profile what it is allowed to do. The one honest discriminator is the
// root element's name attribute: the RC-5 writes
// `<database name="RC-5" revision="0">` (hardware dumps, mirrored by the test
// fixtures), the two-track model writes `name="RC-500"` plus a <TRACK2>
// section per memory (its card, and the boss-rc500-editor template quoted in
// issue #35, agree).

// The family this app was written for — the pedal it speaks to on the bus
// (PedalPortName.h) and the profile every one-track path means.
inline constexpr std::string_view kFamilyName = profile::kRc5.familyName;

// The root element opener and the exact attribute shape the pedal writes.
inline constexpr std::string_view kDatabaseOpen = "<database";
inline constexpr std::string_view kNameAttribute = " name=\"";

// The XML declaration that may precede the root opener (the pedal writes one).
inline constexpr std::string_view kXmlDeclOpen = "<?xml";
inline constexpr std::string_view kXmlDeclClose = "?>";

// One track's section: <TRACK1> on every model, <TRACK2> on the two-track one.
inline std::string trackSectionName(int track) { return "TRACK" + std::to_string(track); }
inline std::string trackSectionOpen(int track) { return "<" + trackSectionName(track) + ">"; }

// The model name the root element carries ("RC-5", "RC-500", or whatever a
// card says). Structure is judged here; what that name MEANS is the caller's
// question — familyOf below turns it into a profile for the app's own
// document reads, the card marker (CardMarker.hpp) records it exactly as
// the card says.
//
// The name attribute is read from the ROOT opener only, and the root opener
// must be the first element of the document (only the XML declaration and
// whitespace may precede it) — scanning any further would let a comment or a
// memory body vouch for a foreign root. No case folding, no whitespace
// forgiveness: the pedal's XML is machine-written, so any variant is foreign
// or damaged.
inline std::string_view rootDatabaseName(std::string_view document)
{
    std::size_t at = 0;
    if (document.starts_with(kXmlDeclOpen)) {
        const auto declClose = document.find(kXmlDeclClose);
        if (declClose == std::string_view::npos)
            throw Error("not an RC0 memory file: unterminated XML declaration");
        at = declClose + kXmlDeclClose.size();
    }
    while (at < document.size()
           && (document[at] == '\n' || document[at] == '\r' || document[at] == '\t'
               || document[at] == ' '))
        ++at;
    // The tag name must end right after "<database": a space (attributes
    // follow) or '>' — anything else is a different element wearing a prefix.
    const std::size_t afterTag = at + kDatabaseOpen.size();
    if (document.compare(at, kDatabaseOpen.size(), kDatabaseOpen) != 0
        || afterTag >= document.size()
        || (document[afterTag] != ' ' && document[afterTag] != '>'))
        throw Error("not an RC0 memory file: no <database ...> root element");
    const auto openerClose = document.find('>', at);
    if (openerClose == std::string_view::npos)
        throw Error("not an RC0 memory file: unterminated <database ...> opener");
    const std::string_view opener = document.substr(at, openerClose + 1 - at);
    const auto nameAt = opener.find(kNameAttribute);
    if (nameAt == std::string_view::npos)
        throw Error("not an RC0 memory file: the <database ...> root has no name attribute");
    const auto valueBegin = nameAt + kNameAttribute.size();
    const auto valueClose = opener.find('"', valueBegin);
    if (valueClose == std::string_view::npos)
        throw Error("not an RC0 memory file: unterminated name attribute in the root element");
    return opener.substr(valueBegin, valueClose - valueBegin);
}

// The profile a document was written by, read from its root element — and a
// refusal, by name, of anything else.
inline const profile::DeviceProfile& familyOf(std::string_view document)
{
    return profile::byFamilyName(rootDatabaseName(document));
}

// The same, for a whole file (document plus trailer).
inline const profile::DeviceProfile& profileOf(std::string_view text)
{
    return familyOf(splitFile(text).document);
}

// A structurally sound memory file, and whose it is: a known family (the
// guard above); exactly the 99 distinct <mem id="0..98"> entries (the
// reference counts distinct ids the same way — and a settings file, which
// has none, is refused as such); then memories shaped for the family's track
// count — <TRACK{n}> present and <TRACK{n+1}> absent, so an "RC-5" carrying
// a second track is refused, and a two-track root carrying a third track, or
// none, is refused the same way.
inline const profile::DeviceProfile& assertMemoryFile(std::string_view text)
{
    const auto document = splitFile(text).document;
    const profile::DeviceProfile& family = familyOf(document);
    std::set<long long> seen;
    static constexpr std::string_view kOpen = "<mem id=\"";
    std::size_t from = 0;
    while (true) {
        const auto at = document.find(kOpen, from);
        if (at == std::string::npos)
            break;
        from = at + 1;
        const std::size_t p = at + kOpen.size();
        std::size_t digits = p;
        while (digits < document.size() && detail::isDigit(document[digits]))
            ++digits;
        if (digits == p || document.compare(digits, 2, "\">") != 0)
            continue;
        seen.insert(detail::parseInteger(std::string_view(document).substr(p, digits - p),
                                         "<mem id>"));
    }
    if (seen.size() != static_cast<std::size_t>(kSlotCount))
        throw Error("expected " + std::to_string(kSlotCount) + " <mem> entries, found "
                    + std::to_string(seen.size()));
    for (int id = 0; id < kSlotCount; ++id)
        if (!seen.contains(id))
            throw Error("missing <mem id=\"" + std::to_string(id) + "\">");
    const std::string name(family.familyName);
    const int tracks = family.trackCount;
    if (document.find(trackSectionOpen(tracks)) == std::string::npos)
        throw Error("this card carries no " + trackSectionOpen(tracks) + " section \xe2\x80\x94 an"
                    " \"" + name + "\" memory holds " + std::to_string(tracks)
                    + (tracks == 1 ? " track" : " tracks") + ", each in a section of its own");
    if (document.find(trackSectionOpen(tracks + 1)) != std::string::npos)
        throw Error("this card carries " + trackSectionOpen(tracks + 1)
                    + " sections \xe2\x80\x94 a memory with more tracks than an \"" + name
                    + "\" has; " + profile::onlySpeaks());
    return family;
}

namespace detail {

    struct MemRegion {
        std::size_t bodyStart;
        std::size_t bodyEnd;
    };

    inline MemRegion memRegion(std::string_view text, int slot)
    {
        if (slot < 1 || slot > kSlotCount)
            throw Error("slot out of range 1.." + std::to_string(kSlotCount) + ": "
                        + std::to_string(slot));
        const std::string open = "<mem id=\"" + std::to_string(slot - 1) + "\">";
        const auto start = text.find(open);
        if (start == std::string_view::npos)
            throw Error("missing <mem id=\"" + std::to_string(slot - 1) + "\">");
        const auto end = text.find("</mem>", start);
        if (end == std::string_view::npos)
            throw Error("unterminated <mem id=\"" + std::to_string(slot - 1) + "\">");
        return { start + open.size(), end };
    }

} // namespace detail

inline std::string slotBody(std::string_view text, int slot)
{
    const auto region = detail::memRegion(text, slot);
    return std::string(text.substr(region.bodyStart, region.bodyEnd - region.bodyStart));
}

inline std::string replaceSlotBody(std::string_view text, int slot, std::string_view newBody)
{
    const auto region = detail::memRegion(text, slot);
    std::string out;
    out.reserve(text.size() - (region.bodyEnd - region.bodyStart) + newBody.size());
    out.append(text.substr(0, region.bodyStart));
    out.append(newBody);
    out.append(text.substr(region.bodyEnd));
    return out;
}

inline long long field(std::string_view body, std::string_view tag)
{
    return detail::theOneField(body, tag).value;
}

inline std::string setField(std::string_view body, std::string_view tag, long long value)
{
    const auto match = detail::theOneField(body, tag);
    std::string out;
    out.reserve(body.size());
    out.append(body.substr(0, match.begin));
    out.append("<").append(tag).append(">").append(std::to_string(value))
       .append("</").append(tag).append(">");
    out.append(body.substr(match.end));
    return out;
}

// --- section-scoped fields ---
//
// A memory body is a run of flat sections — <NAME>, <TRACK1>, <MASTER> and
// <RHYTHM> on the RC-5 (fixtures/golden.json) — and SYSTEM*.RC0 is the same
// shape one level up (<SETUP>, <MIDI>, <CTL>). Tag names are NOT unique across
// sections: <Level> is MASTER's and RHYTHM's both, and a memory with a second
// track carries every TRACK field twice. So a field is addressed by its
// section first, and inside the section field()'s one-occurrence rule holds
// exactly as before.
//
// The section itself must occur exactly once in the text it is looked up in.
// Asked for <TRACK1> across a whole memory file, the 99 of them are refused
// rather than the first one quietly taken — the caller meant one memory's.

inline constexpr std::string_view kSectionTrack1 = "TRACK1";
inline constexpr std::string_view kSectionMaster = "MASTER";
inline constexpr std::string_view kSectionRhythm = "RHYTHM";

namespace detail {

    struct SectionRegion {
        std::size_t bodyStart; // one past '>' of the opening tag
        std::size_t bodyEnd;   // offset of '<' of the closing tag
    };

    inline SectionRegion sectionRegion(std::string_view text, std::string_view section)
    {
        const std::string open = "<" + std::string(section) + ">";
        const std::string close = "</" + std::string(section) + ">";
        const auto start = text.find(open);
        if (start == std::string_view::npos)
            throw Error("missing <" + std::string(section) + "> section");
        if (text.find(open, start + open.size()) != std::string_view::npos)
            throw Error("<" + std::string(section) + "> section occurs more than once");
        const auto end = text.find(close, start + open.size());
        if (end == std::string_view::npos)
            throw Error("unterminated <" + std::string(section) + "> section");
        return { start + open.size(), end };
    }

} // namespace detail

inline long long sectionField(std::string_view text, std::string_view section,
                              std::string_view tag)
{
    const auto region = detail::sectionRegion(text, section);
    return field(text.substr(region.bodyStart, region.bodyEnd - region.bodyStart), tag);
}

// Rewrite one field inside one section; every byte outside that field's
// <tag>value</tag> is reproduced exactly.
inline std::string setSectionField(std::string_view text, std::string_view section,
                                   std::string_view tag, long long value)
{
    const auto region = detail::sectionRegion(text, section);
    std::string out;
    out.reserve(text.size());
    out.append(text.substr(0, region.bodyStart));
    out.append(setField(text.substr(region.bodyStart, region.bodyEnd - region.bodyStart), tag,
                        value));
    out.append(text.substr(region.bodyEnd));
    return out;
}

// --- slot names: 12 chars stored as decimal char codes in <C01>..<C12> ---

inline std::string encodeName(std::string_view name)
{
    if (name.empty())
        throw Error("name must be a non-empty string");
    if (name.size() > static_cast<std::size_t>(kNameLength))
        throw Error("name longer than " + std::to_string(kNameLength) + " characters: \""
                    + std::string(name) + "\"");
    for (const char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
            throw Error("name must be printable ASCII (the pedal display cannot show anything"
                        " else): \"" + std::string(name) + "\"");
    std::string padded(name);
    padded.resize(static_cast<std::size_t>(kNameLength), ' ');
    return padded;
}

inline std::string decodeName(std::string_view body)
{
    // Every <Cxx>digits</Cyy> occurrence in order; exactly kNameLength expected.
    std::string out;
    int found = 0;
    std::size_t from = 0;
    while (true) {
        const auto at = body.find("<C", from);
        if (at == std::string_view::npos)
            break;
        from = at + 1;
        if (at + 5 > body.size() || !detail::isDigit(body[at + 2])
            || !detail::isDigit(body[at + 3]) || body[at + 4] != '>')
            continue;
        const std::size_t p = at + 5;
        std::size_t digits = p;
        while (digits < body.size() && detail::isDigit(body[digits]))
            ++digits;
        if (digits == p)
            continue;
        if (digits + 6 > body.size() || body.compare(digits, 3, "</C") != 0
            || !detail::isDigit(body[digits + 3]) || !detail::isDigit(body[digits + 4])
            || body[digits + 5] != '>')
            continue;
        const long long code = detail::parseInteger(body.substr(p, digits - p), "<Cxx>");
        // A name is stored as byte char codes; anything beyond one byte cannot
        // come from the pedal and cannot be represented losslessly — refuse.
        if (code < 0 || code > 0xff)
            throw Error("name char code out of byte range: " + std::to_string(code));
        out.push_back(static_cast<char>(static_cast<unsigned char>(code)));
        ++found;
    }
    if (found != kNameLength)
        throw Error("expected " + std::to_string(kNameLength) + " <Cxx> name entries, found "
                    + std::to_string(found));
    return out;
}

namespace detail {

    inline std::string nameBlock(std::string_view name)
    {
        const std::string padded = encodeName(name);
        std::string out = "<NAME>\n";
        for (int i = 0; i < kNameLength; ++i) {
            const char tens = static_cast<char>('0' + (i + 1) / 10);
            const char ones = static_cast<char>('0' + (i + 1) % 10);
            const std::string tag = std::string("C") + tens + ones;
            out += "\t<" + tag + ">"
                 + std::to_string(static_cast<unsigned char>(padded[static_cast<std::size_t>(i)]))
                 + "</" + tag + ">\n";
        }
        return out + "</NAME>";
    }

} // namespace detail

inline std::string setName(std::string_view body, std::string_view name)
{
    const auto start = body.find("<NAME>");
    if (start == std::string_view::npos)
        throw Error("missing <NAME> block");
    const auto close = body.find("</NAME>", start);
    if (close == std::string_view::npos)
        throw Error("missing <NAME> block");
    const auto end = close + std::string_view("</NAME>").size();
    std::string out;
    out.reserve(body.size());
    out.append(body.substr(0, start));
    out.append(detail::nameBlock(name));
    out.append(body.substr(end));
    return out;
}

inline std::string defaultSlotName(int slot)
{
    const std::string n = std::to_string(slot);
    return "Memory" + std::string(n.size() < 2 ? "0" : "") + n;
}

// The body of a never-touched memory of a model, exactly as its pedal formats
// it: the default name, then the profile's factory sections. For the RC-5 that
// is what MEMORY CLEAR on the device leaves, and every factory-empty memory of
// a real card reproduces it byte for byte (DeviceProfile.hpp). A model whose
// factory body is not on record does not get one made up.
inline std::string factorySlotBody(const profile::DeviceProfile& family, int slot)
{
    if (slot < 1 || slot > kSlotCount)
        throw Error("slot out of range 1.." + std::to_string(kSlotCount) + ": "
                    + std::to_string(slot));
    if (!family.hasFactoryBody())
        throw Error("the factory memory of the \"" + std::string(family.familyName)
                    + "\" model is not on record \xe2\x80\x94 nothing can be cleared to it");
    return "\n" + detail::nameBlock(defaultSlotName(slot)) + std::string(family.factorySections);
}

// The RC-5's.
inline std::string factorySlotBody(int slot)
{
    return factorySlotBody(profile::kRc5, slot);
}

// --- trailer ---

// The write-generation counter if the tail has the pedal's shape — exactly
// "\n" plus 4 bytes, decoded little-endian — otherwise no value. Every
// observed firmware writes this shape (SYSTEM and MEMORY alike); the VALUE
// is never judged here, the pedal itself accepts and increments any of it.
inline std::optional<std::uint32_t> tailMarker(std::string_view text)
{
    const auto tail = splitFile(text).tail;
    if (tail.size() != 5 || tail[0] != '\n')
        return std::nullopt;
    std::uint32_t generation = 0;
    for (std::size_t i = 4; i >= 1; --i)
        generation = (generation << 8) | static_cast<unsigned char>(tail[i]);
    return generation;
}

// Restamp the trailer with an explicit write generation. The document bytes
// are untouched; a structurally unrecognized trailer is refused, never
// silently rewritten.
inline std::string setTailGeneration(std::string_view text, std::uint32_t generation)
{
    if (!tailMarker(text).has_value())
        throw Error("unrecognized trailer after </database>; refusing to rewrite it");
    std::string out(text.substr(0, text.size() - 4));
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((generation >> (8 * i)) & 0xff));
    return out;
}

// The factory-pair stamp (fixtures and fresh volumes).
inline std::string setTailMarker(std::string_view text, int fileNo)
{
    return setTailGeneration(text, tailMarkerFor(fileNo));
}

} // namespace loopercat::rc0
