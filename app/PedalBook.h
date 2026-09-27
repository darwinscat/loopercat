// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//==============================================================================
// loopercat::pedalbook — what the app remembers about the pedals it has met
// (issue #98): for each MIDI endpoint, the card it carried the last time and
// the name written on that card.
//
// Why a book at all: a pedal's name lives on its card, in the marker
// loopercat.toml (CardMarker.hpp), readable only once the pedal is in
// storage mode — which is the very thing Connect is about to ask for. So
// the app keeps what it learned at the last connection, keyed by the one
// per-pedal signal that exists before a card is mounted: the OS's endpoint
// id (CoreMIDI uniqueID on macOS, measured to survive a power cycle on the
// same port). First meeting asks without a name; every one after that asks
// with it.
//
// The book is a convenience, never the truth: the card is. A book that
// cannot be parsed is an error its reader must say out loud — and then
// start with an empty one, because a stale cache must not stop Connect.
//==============================================================================
namespace loopercat::pedalbook
{

struct Entry {
    std::string endpoint; // the OS endpoint id, as JUCE hands it out (MidiDeviceInfo::identifier)
    std::string cardId;   // the marker's id
    std::string name;     // the marker's name, as last seen
    std::string family;   // "RC-5", "RC-500" — from the port name at the time
    std::int64_t seenMs = 0;
};

namespace detail {
    inline void assertField(std::string_view what, std::string_view value)
    {
        if (value.empty())
            throw Error("a pedal book entry needs its " + std::string(what));
        for (const char c : value)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
                throw Error("a pedal book " + std::string(what) + " cannot hold control characters");
    }
} // namespace detail

class Book
{
public:
    // Remember a pedal: a second sighting of the same endpoint replaces the
    // first — the card in that pedal now is the card that counts.
    void remember(Entry entry)
    {
        detail::assertField("endpoint", entry.endpoint);
        detail::assertField("card id", entry.cardId);
        detail::assertField("name", entry.name);
        detail::assertField("family", entry.family);
        if (entry.seenMs < 0)
            throw Error("a pedal book entry cannot be seen before the epoch");
        for (Entry& known : entries_)
            if (known.endpoint == entry.endpoint) {
                known = std::move(entry);
                return;
            }
        entries_.push_back(std::move(entry));
    }

    std::optional<Entry> find(std::string_view endpoint) const
    {
        for (const Entry& known : entries_)
            if (known.endpoint == endpoint)
                return known;
        return std::nullopt;
    }

    // A rename on the card reaches every endpoint that carried that card.
    void renamed(std::string_view cardId, std::string_view name)
    {
        detail::assertField("name", name);
        for (Entry& known : entries_)
            if (known.cardId == cardId)
                known.name = std::string(name);
    }

    const std::vector<Entry>& entries() const { return entries_; }

    // One line per pedal, tab-separated: endpoint, card id, name, family,
    // last seen. Names come from the marker, which refuses control
    // characters, so a tab never occurs inside a field.
    std::string serialize() const
    {
        std::string out;
        for (const Entry& e : entries_)
            out += e.endpoint + '\t' + e.cardId + '\t' + e.name + '\t' + e.family + '\t'
                 + std::to_string(e.seenMs) + '\n';
        return out;
    }

    static Book parse(std::string_view text)
    {
        Book book;
        std::size_t lineNo = 0;
        while (!text.empty()) {
            ++lineNo;
            const std::size_t eol = text.find('\n');
            std::string_view line = text.substr(0, eol);
            text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
            if (line.empty())
                continue;
            std::vector<std::string_view> fields;
            while (true) {
                const std::size_t tab = line.find('\t');
                fields.push_back(line.substr(0, tab));
                if (tab == std::string_view::npos)
                    break;
                line = line.substr(tab + 1);
            }
            if (fields.size() != 5)
                throw Error("pedal book line " + std::to_string(lineNo) + ": expected 5 fields, found "
                            + std::to_string(fields.size()));
            Entry entry;
            entry.endpoint = std::string(fields[0]);
            entry.cardId = std::string(fields[1]);
            entry.name = std::string(fields[2]);
            entry.family = std::string(fields[3]);
            const std::string seen(fields[4]);
            if (seen.empty() || seen.find_first_not_of("0123456789") != std::string::npos)
                throw Error("pedal book line " + std::to_string(lineNo) + ": last seen is not a number: \""
                            + seen + "\"");
            entry.seenMs = std::stoll(seen);
            book.remember(std::move(entry)); // the same checks a live entry passes
        }
        return book;
    }

private:
    std::vector<Entry> entries_;
};

// The line a player chooses a pedal by: the name from its card when the
// app has met this endpoint, otherwise something honest — the model and
// the endpoint id, said to be a first meeting — never a made-up name.
inline std::string choiceLabel(std::string_view family, std::string_view endpoint,
                               const std::optional<Entry>& known)
{
    if (known)
        return known->name;
    return std::string(family) + " \xe2\x80\x94 first meeting (endpoint " + std::string(endpoint) + ")";
}

} // namespace loopercat::pedalbook
