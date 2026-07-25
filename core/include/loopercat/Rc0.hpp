// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// MEMORY*.RC0 model — byte-safe surgical editing.
//
// The RC-5's own parser is strict and every memory file carries a trailer
// after </database>: "<generation>\0\0\0", where the generation byte is a
// WRITE COUNTER, incremented as a raw byte with no decimal carry
// ('8','9',':',';',…). The factory-fresh pair is MEMORY1="8", MEMORY2="9";
// a save on the pedal stamps the freshly written bank one generation past
// the other (observed live 2026-07-24: a pedal-side recording produced
// MEMORY1=0x3a next to MEMORY2=0x39). A structurally broken trailer
// triggers "LOOPER DATA READ ERR" on boot.
// The invariant of this module: any byte we were not explicitly asked to
// change is reproduced exactly. Files are handled as raw byte strings
// (std::string carries arbitrary bytes) so the trailer and any non-ASCII
// bytes round-trip unharmed.
//
// Format knowledge source: rc5cat (lib/rc0.js, macos/RC5Kit), values verified
// against real hardware — see fixtures/golden.json.

#pragma once

#include "Error.hpp"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace loopercat::rc0 {

inline constexpr int kSlotCount = 99;
inline constexpr int kNameLength = 12;

// The factory-fresh generation pair (golden.json tailMarkers). The trailer
// byte is really a write-generation counter — see setTailGeneration; this
// pair is where a pedal starts counting and where a healed volume restarts.
inline unsigned char tailMarkerFor(int fileNo)
{
    switch (fileNo) {
    case 1: return 0x38; // '8'
    case 2: return 0x39; // '9'
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

// A structurally sound memory file: exactly the 99 distinct <mem id="0..98">
// entries (the reference counts distinct ids the same way).
inline void assertMemoryFile(std::string_view text)
{
    const auto document = splitFile(text).document;
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
// A slot body is fixed sections (<NAME>, <TRACK1>, <MASTER>, <RHYTHM>) and
// tag names are NOT unique across them: <Level> lives in both MASTER and
// RHYTHM (docs/pedal-settings.md). Field surgery on any ambiguous tag must be
// scoped to its section — the unscoped field/setField refuse ambiguity by
// design (theOneField), so this is the only correct path to such fields.

namespace detail {

    struct SectionRegion {
        std::size_t bodyStart; // one past '>' of the opening tag
        std::size_t bodyEnd;   // offset of '<' of the closing tag
    };

    inline SectionRegion sectionRegion(std::string_view body, std::string_view section)
    {
        const std::string open = "<" + std::string(section) + ">";
        const std::string close = "</" + std::string(section) + ">";
        const auto start = body.find(open);
        if (start == std::string_view::npos)
            throw Error("missing <" + std::string(section) + "> section");
        if (body.find(open, start + 1) != std::string_view::npos)
            throw Error("<" + std::string(section) + "> occurs more than once");
        const auto end = body.find(close, start);
        if (end == std::string_view::npos)
            throw Error("unterminated <" + std::string(section) + "> section");
        return { start + open.size(), end };
    }

} // namespace detail

inline long long sectionField(std::string_view body, std::string_view section,
                              std::string_view tag)
{
    const auto region = detail::sectionRegion(body, section);
    return field(body.substr(region.bodyStart, region.bodyEnd - region.bodyStart), tag);
}

inline std::string setSectionField(std::string_view body, std::string_view section,
                                   std::string_view tag, long long value)
{
    const auto region = detail::sectionRegion(body, section);
    std::string out;
    out.reserve(body.size());
    out.append(body.substr(0, region.bodyStart));
    out.append(setField(body.substr(region.bodyStart, region.bodyEnd - region.bodyStart),
                        tag, value));
    out.append(body.substr(region.bodyEnd));
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

// The body of a never-touched slot, exactly as the pedal formats it —
// captured from real hardware, where every virgin slot is byte-identical
// (note the non-obvious factory values: Measure=1, Reverb=30, Fill=1,
// Part4=0, rhythm Stop=1). This is what MEMORY CLEAR on the device leaves.
inline std::string factorySlotBody(int slot)
{
    if (slot < 1 || slot > kSlotCount)
        throw Error("slot out of range 1.." + std::to_string(kSlotCount) + ": "
                    + std::to_string(slot));
    return "\n" + detail::nameBlock(defaultSlotName(slot)) + R"(
<TRACK1>
	<Rev>0</Rev>
	<PlyLvl>100</PlyLvl>
	<Pan>50</Pan>
	<One>0</One>
	<StrtMod>0</StrtMod>
	<StpMod>0</StpMod>
	<Measure>1</Measure>
	<MeasMod>1</MeasMod>
	<MeasLen>0</MeasLen>
	<MeasBtLp>0</MeasBtLp>
	<RecTmp>1200</RecTmp>
	<WavStat>0</WavStat>
	<WavLen>0</WavLen>
</TRACK1>
<MASTER>
	<Tempo>1200</Tempo>
	<DubMode>0</DubMode>
	<RecAction>1</RecAction>
	<AutoRec>0</AutoRec>
	<FadeTime>5</FadeTime>
	<Level>100</Level>
	<LpMod>0</LpMod>
	<LpLen>0</LpLen>
	<TrkMod>1</TrkMod>
	<Sync>0</Sync>
</MASTER>
<RHYTHM>
	<Level>100</Level>
	<Reverb>30</Reverb>
	<Pattern>0</Pattern>
	<Variation>0</Variation>
	<VariationChange>0</VariationChange>
	<Kit>0</Kit>
	<Beat>2</Beat>
	<Fill>1</Fill>
	<Part1>1</Part1>
	<Part2>1</Part2>
	<Part3>1</Part3>
	<Part4>0</Part4>
	<RecCount>0</RecCount>
	<PlayCount>0</PlayCount>
	<Start>0</Start>
	<Stop>1</Stop>
	<ToneLow>10</ToneLow>
	<ToneHigh>10</ToneHigh>
	<State>0</State>
</RHYTHM>
)";
}

// --- trailer ---

// The trailer byte if the tail ends with <byte> 0 0 0, otherwise no value.
inline std::optional<unsigned char> tailMarker(std::string_view text)
{
    const auto tail = splitFile(text).tail;
    if (tail.size() < 4)
        return std::nullopt;
    const auto last4 = std::string_view(tail).substr(tail.size() - 4);
    if (last4[1] != '\0' || last4[2] != '\0' || last4[3] != '\0')
        return std::nullopt;
    return static_cast<unsigned char>(last4[0]);
}

// Restamp the trailer with an explicit write generation. The document bytes
// are untouched; a structurally unrecognized trailer is refused, never
// silently rewritten.
inline std::string setTailGeneration(std::string_view text, unsigned char generation)
{
    if (!tailMarker(text).has_value())
        throw Error("unrecognized trailer after </database>; refusing to rewrite it");
    std::string out(text.substr(0, text.size() - 4));
    out.push_back(static_cast<char>(generation));
    out.append("\0\0\0", 3);
    return out;
}

// The factory-pair stamp (fixtures and fresh volumes).
inline std::string setTailMarker(std::string_view text, int fileNo)
{
    return setTailGeneration(text, tailMarkerFor(fileNo));
}

} // namespace loopercat::rc0
