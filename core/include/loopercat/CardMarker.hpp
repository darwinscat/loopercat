// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The card marker: LooperCat's own identity for a pedal's storage —
// /loopercat.toml at the volume root.
//
// Why the app needs one: two RC-5s are identical to every signal a computer
// can read. Measured with both on one bus (2026-09-24): the USB serial is the
// placeholder 0123456789ABCDEF on both (and on the RC-500); the MIDI Identity
// Reply is the same byte for byte (F0 7E 10 06 02 41 76 03 00 00 00 00 00 00
// F7 — no firmware version, no serial); the volume label is BOSS RC-5 on
// both; and the FAT volume serial is a constant of the family's formatter
// (4EA1-0000), so macOS derives one volume UUID for both RC-5s AND the
// RC-500. Nothing in the pedal's own files carries an id either. What a
// player recognises is the name they gave the pedal — so the app writes one
// small file at the card root: a uuid the history keys cards by, and a name
// for the corner and the "which pedal?" question (issues #98, #99).
//
// A root marker was tested on both RC-5s and the RC-500: ejected,
// power-cycled, re-read byte for byte (sha256 equal) while the pedal rewrote
// its own SYSTEM banks around it — it neither removes nor touches a foreign
// file at the root (it has booted beside macOS's .Spotlight-V100 there since
// January 2025). The one hazard is macOS itself: every write plants a
// ._loopercat.toml sidecar beside the file, so every write here ends
// with volume::sweepJunk, which walks the root for exactly this reason.
//
// The file is TOML, parsed and written by felitronics-toml. The identity is
// minted once; rename changes only the name in the parsed document, keeping
// unknown values. The canonical writer can reorder dotted keys and inline tables
// and drops comments.
// Writes are staged, verified, renamed over the marker, and verified again.
//
//   [loopercat_card]
//   format = 1
//   id = "527a2b7a-da5c-4910-..."
//   name = "RC-5 Kitty"
//   model = "RC-5"
//   created = "2026-09-24T21:34:33Z"
//   by = "LooperCat"

#pragma once

#include "Error.hpp"
#include "Rc0.hpp"
#include "Volume.hpp"

#include <felitronics/toml/Toml.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace loopercat::marker {

namespace fs = std::filesystem;
namespace toml = felitronics::toml;

inline constexpr std::string_view kFileName = "loopercat.toml";
inline constexpr std::string_view kTableKey = "loopercat_card";
inline constexpr std::string_view kFormatKey = "format";
inline constexpr std::int64_t kFormat = 1;
inline constexpr std::string_view kIdKey = "id";
inline constexpr std::string_view kNameKey = "name";
inline constexpr std::string_view kModelKey = "model";
inline constexpr std::string_view kCreatedKey = "created";
inline constexpr std::string_view kByKey = "by";
inline constexpr std::string_view kWriter = "LooperCat";

// Every write lands here first and is renamed over the marker only once it
// has been read back whole: a cable pulled mid-write leaves a torn .part,
// never a torn marker (and never an id-less card).
inline constexpr std::string_view kPartSuffix = ".part";

// A name fits the window's corner and stays a name: at most 64 bytes of
// UTF-8, no control characters. (The pedal's own memory names are 12
// characters; this names the whole pedal — "RC-500 Bob".)
inline constexpr std::size_t kMaxNameBytes = 64;

// Larger than this is not a marker, whatever it is called: ours is under 200
// bytes, and reading a stray gigabyte to find that out is not a service.
inline constexpr std::uintmax_t kMaxFileBytes = 64 * 1024;

// How much of MEMORY1.RC0 the model needs: the root opener sits inside the
// first 100 bytes of a real dump. 4 KiB is generous and never a whole bank.
inline constexpr std::size_t kHeaderProbeBytes = 4096;

struct Card {
    std::string id;
    std::string name;
    std::string model;
    std::string created;
};

inline fs::path markerPath(const fs::path& volume) { return volume / kFileName; }

namespace detail {

    // Well-formed UTF-8: no overlong forms, no surrogates, nothing past U+10FFFF.
    inline bool validUtf8(std::string_view s)
    {
        std::size_t i = 0;
        while (i < s.size()) {
            const auto b0 = static_cast<unsigned char>(s[i]);
            std::size_t need = 0;
            unsigned cp = 0;
            if (b0 < 0x80) { ++i; continue; }
            if (b0 >= 0xC2 && b0 <= 0xDF) { need = 1; cp = b0 & 0x1Fu; }
            else if (b0 >= 0xE0 && b0 <= 0xEF) { need = 2; cp = b0 & 0x0Fu; }
            else if (b0 >= 0xF0 && b0 <= 0xF4) { need = 3; cp = b0 & 0x07u; }
            else return false;
            if (i + need >= s.size()) return false; // the continuation bytes are missing
            for (std::size_t k = 1; k <= need; ++k) {
                const auto b = static_cast<unsigned char>(s[i + k]);
                if ((b & 0xC0) != 0x80) return false;
                cp = (cp << 6) | (b & 0x3Fu);
            }
            if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000)) return false; // overlong
            if (cp >= 0xD800 && cp <= 0xDFFF) return false;
            if (cp > 0x10FFFF) return false;
            i += need + 1;
        }
        return true;
    }

    inline void assertName(std::string_view name)
    {
        if (name.empty())
            throw Error("a pedal name cannot be empty");
        if (name.size() > kMaxNameBytes)
            throw Error("a pedal name is at most " + std::to_string(kMaxNameBytes)
                        + " bytes of UTF-8; this one is " + std::to_string(name.size()));
        for (std::size_t i = 0; i < name.size(); ++i) {
            const auto u = static_cast<unsigned char>(name[i]);
            const bool c1 = u == 0xC2 && i + 1 < name.size()
                         && static_cast<unsigned char>(name[i + 1]) >= 0x80
                         && static_cast<unsigned char>(name[i + 1]) <= 0x9F;
            if (u < 0x20 || u == 0x7F || c1)
                throw Error("a pedal name cannot contain control characters");
        }
        if (!validUtf8(name))
            throw Error("a pedal name must be valid UTF-8");
    }

    // RFC 4122 version 4: 122 random bits from the platform's entropy source.
    inline std::string uuid4()
    {
        std::random_device entropy;
        std::array<std::uint8_t, 16> bytes {};
        for (std::size_t i = 0; i < bytes.size(); i += 4) {
            const std::uint32_t word = entropy();
            for (std::size_t k = 0; k < 4; ++k)
                bytes[i + k] = static_cast<std::uint8_t>((word >> (8 * k)) & 0xFF);
        }
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40); // version 4
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80); // RFC 4122 variant
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                out.push_back('-');
            out.push_back(digits[static_cast<std::size_t>(bytes[i] >> 4)]);
            out.push_back(digits[static_cast<std::size_t>(bytes[i] & 0x0F)]);
        }
        return out;
    }

    // "YYYY-MM-DDTHH:MM:SSZ" — UTC, so two computers write comparable stamps.
    inline std::string isoUtcNow()
    {
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm parts {};
#if defined(_WIN32)
        gmtime_s(&parts, &now);
#else
        gmtime_r(&now, &parts);
#endif
        const auto padded = [](int value, std::size_t width) {
            std::string text = std::to_string(value);
            return text.size() < width ? std::string(width - text.size(), '0') + text : text;
        };
        return padded(parts.tm_year + 1900, 4) + '-' + padded(parts.tm_mon + 1, 2) + '-'
             + padded(parts.tm_mday, 2) + 'T' + padded(parts.tm_hour, 2) + ':' + padded(parts.tm_min, 2)
             + ':' + padded(parts.tm_sec, 2) + 'Z';
    }

    // A card that is not mounted is not a card without a marker: the dialog
    // that asks "which pedal?" calls mint right after the pedal enters
    // STORAGE, where the race with the mount is real, and the answer must
    // say "not mounted", not "no marker" or "cannot write".
    inline void requireVolume(const fs::path& volume)
    {
        std::error_code ec;
        if (!fs::is_directory(volume, ec))
            throw Error("no volume at " + volume.string()
                        + " \xe2\x80\x94 is the pedal in storage mode and mounted?");
    }

    // The file's bytes, or no value when there is no such file. Anything else
    // in the way — a directory under the name, an unreadable file, one too
    // large to be a marker — is an error, not an absence.
    inline std::optional<std::string> readIfPresent(const fs::path& file)
    {
        std::error_code ec;
        const auto status = fs::symlink_status(file, ec);
        if (ec == std::errc::no_such_file_or_directory)
            return std::nullopt;
        if (ec)
            throw Error("cannot read " + file.string() + ": " + ec.message());
        if (!fs::exists(status))
            return std::nullopt;
        if (fs::is_directory(status))
            throw Error(file.string() + " is a directory, not a card marker");
        const auto size = fs::file_size(file, ec);
        if (ec)
            throw Error("cannot read " + file.string() + ": " + ec.message());
        if (size > kMaxFileBytes)
            throw Error(file.string() + " is " + std::to_string(size)
                        + " bytes \xe2\x80\x94 too large to be a card marker");
        std::ifstream in(file, std::ios::binary);
        if (!in)
            throw Error("cannot read " + file.string());
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.good() && !in.eof())
            throw Error("cannot read " + file.string());
        return bytes;
    }

    [[noreturn]] inline void fail(std::string reason, toml::Position position)
    {
        throw Error(std::string(kFileName) + ": " + reason + " at "
                    + std::to_string(position.line) + ":" + std::to_string(position.column));
    }

    inline toml::Table parse(std::string_view bytes)
    {
        auto result = toml::parse(bytes);
        if (const auto* error = std::get_if<toml::Error>(&result))
            fail(toml::codeName(error->code), { error->line, error->column, 0 });
        return std::get<toml::Table>(std::move(result));
    }

    inline const toml::Table& cardTable(const toml::Table& doc)
    {
        const auto* value = doc.find(kTableKey);
        const auto* table = value ? std::get_if<toml::Table>(&value->data) : nullptr;
        if (table == nullptr)
            fail("not a LooperCat card marker (no [loopercat_card] table)",
                 value ? value->position : doc.position);
        return *table;
    }

    inline const toml::Value& requireField(const toml::Table& table, std::string_view key)
    {
        const auto* value = table.find(key);
        if (value == nullptr)
            fail("no \"" + std::string(key) + "\" field", table.position);
        return *value;
    }

    inline const std::string& requireString(const toml::Table& table, std::string_view key)
    {
        const auto& value = requireField(table, key);
        const auto* text = std::get_if<std::string>(&value.data);
        if (text == nullptr)
            fail("\"" + std::string(key) + "\" is not a string", value.position);
        return *text;
    }

    inline void assertFormat(const toml::Table& table)
    {
        const auto& value = requireField(table, kFormatKey);
        const auto* format = std::get_if<std::int64_t>(&value.data);
        if (format == nullptr)
            fail("\"format\" is not an integer", value.position);
        if (*format > kFormat)
            fail("written by a newer LooperCat (format " + std::to_string(*format)
                 + "); this build reads format " + std::to_string(kFormat), value.position);
        if (*format != kFormat)
            fail("unsupported format \"" + std::to_string(*format) + "\"", value.position);
    }

    inline bool canonicalUuid(std::string_view id)
    {
        if (id.size() != 36) return false;
        for (std::size_t i = 0; i < id.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (id[i] != '-') return false;
            } else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    inline bool canonicalCreated(std::string_view created)
    {
        constexpr std::string_view shape = "0000-00-00T00:00:00Z";
        if (created.size() != shape.size()) return false;
        for (std::size_t i = 0; i < shape.size(); ++i) {
            if (shape[i] == '0') {
                if (created[i] < '0' || created[i] > '9') return false;
            } else if (created[i] != shape[i]) {
                return false;
            }
        }
        return true;
    }

    inline Card cardOf(const toml::Table& doc)
    {
        const auto& table = cardTable(doc);
        assertFormat(table);
        Card card { requireString(table, kIdKey), requireString(table, kNameKey),
                    requireString(table, kModelKey), requireString(table, kCreatedKey) };
        for (const auto key : { kIdKey, kModelKey })
            if (requireString(table, key).empty())
                fail("the \"" + std::string(key) + "\" field is empty", requireField(table, key).position);
        if (!canonicalUuid(card.id))
            fail("the \"id\" field must be a canonical lowercase UUID", requireField(table, kIdKey).position);
        if (!canonicalCreated(card.created))
            fail("the \"created\" field must be YYYY-MM-DDTHH:MM:SSZ", requireField(table, kCreatedKey).position);
        // TOML decodes escaped controls. Validate the decoded name, on every read.
        try {
            assertName(card.name);
        } catch (const Error& error) {
            fail(error.what(), requireField(table, kNameKey).position);
        }
        return card;
    }

    // The model, from the card's own root element — MEMORY1.RC0, or its bank
    // twin when that one cannot be read. Never guessed: a card whose memory
    // files do not say gets no marker.
    inline std::string modelOf(const fs::path& volume)
    {
        std::string firstProblem;
        for (const int fileNo : { 1, 2 }) {
            const fs::path bank = volume::memoryPath(volume, fileNo);
            std::ifstream in(bank, std::ios::binary);
            if (!in) {
                if (firstProblem.empty())
                    firstProblem = "cannot read " + bank.string();
                continue;
            }
            std::string head(kHeaderProbeBytes, '\0');
            in.read(head.data(), static_cast<std::streamsize>(head.size()));
            head.resize(static_cast<std::size_t>(in.gcount()));
            try {
                return std::string(rc0::rootDatabaseName(head));
            } catch (const Error& e) {
                if (firstProblem.empty())
                    firstProblem = bank.string() + ": " + e.what();
            }
        }
        throw Error("cannot tell the card's model: " + firstProblem);
    }

} // namespace detail

// The marker on this volume: no value when the card has none (a card that
// has never met LooperCat), the card when it has one, and an Error naming the
// reason when the file is there but is not a marker this build can read —
// foreign, damaged, or of a later format.
inline std::optional<Card> read(const fs::path& volume)
{
    detail::requireVolume(volume);
    const auto bytes = detail::readIfPresent(markerPath(volume));
    if (!bytes)
        return std::nullopt;
    return detail::cardOf(detail::parse(*bytes));
}

struct Written {
    Card card;
    volume::SweepResult sweep; // the sidecar macOS plants beside the write, removed — or, in `failed`, not
};

namespace detail {

    // Write to the staging name, read back, compare, rename over the marker,
    // read the marker back once more — then sweep the sidecars the writes may
    // have planted. The marker itself is never opened for writing: the old
    // one stays whole until the new one is proven whole, so a cable pulled
    // mid-write costs a .part and nothing else. A write that reads back
    // differently is an error, never a shrug: the id in this file is what
    // the history knows the card by.
    // The reader parameter lets fault tests model a drive returning different bytes.
    template <typename ReadBack = decltype(&readIfPresent)>
    inline Written writeAndVerify(const fs::path& volume, const toml::Table& doc,
                                  ReadBack readBack = readIfPresent)
    {
        const fs::path file = markerPath(volume);
        fs::path part = file;
        part += kPartSuffix;
        const std::string bytes = toml::write(doc);
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            if (!out)
                throw Error("cannot write " + part.string());
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.flush();
            if (!out.good())
                throw Error("cannot write " + part.string());
        }
        const auto staged = readBack(part);
        if (!staged || *staged != bytes)
            throw Error(part.string() + " read back differently from what was written");
        std::error_code ec;
        fs::rename(part, file, ec);
        if (ec)
            throw Error("cannot replace " + file.string() + " with the new marker: " + ec.message());
        const auto back = readBack(file);
        if (!back || *back != bytes)
            throw Error(file.string() + " read back differently from what was written");
        return Written { cardOf(parse(*back)), volume::sweepJunk(volume) };
    }

} // namespace detail

// Give a card its identity: a fresh uuid, the player's name, the model read
// from the card itself, a UTC stamp. Once. A card that already carries a
// marker is refused — its id is what the history knows it by, and a second
// mint would orphan every row — and so is a card whose marker this build
// cannot read: a file it does not understand is not a file it overwrites.
inline Written mint(const fs::path& volume, std::string_view name)
{
    detail::assertName(name);
    detail::requireVolume(volume);
    if (const auto existing = read(volume))
        throw Error("this card already carries a marker (id " + existing->id + ", name \""
                    + existing->name + "\"); the id is written once and never rewritten");
    const std::string model = detail::modelOf(volume);
    toml::Table table;
    (void) table.insert(std::string(kFormatKey), kFormat);
    (void) table.insert(std::string(kIdKey), detail::uuid4());
    (void) table.insert(std::string(kNameKey), std::string(name));
    (void) table.insert(std::string(kModelKey), model);
    (void) table.insert(std::string(kCreatedKey), detail::isoUtcNow());
    (void) table.insert(std::string(kByKey), std::string(kWriter));
    toml::Table doc;
    (void) doc.insert(std::string(kTableKey), std::move(table));
    return detail::writeAndVerify(volume, doc);
}

// Change the name and nothing else: id, created, model, and every field this
// build does not know, keep their values. The canonical writer may reorder
// dotted keys and inline tables.
inline Written rename(const fs::path& volume, std::string_view newName)
{
    detail::assertName(newName);
    detail::requireVolume(volume);
    const auto bytes = detail::readIfPresent(markerPath(volume));
    if (!bytes)
        throw Error("this card has no marker yet \xe2\x80\x94 nothing to rename");
    toml::Table doc = detail::parse(*bytes);
    const Card before = detail::cardOf(doc);
    auto& table = std::get<toml::Table>(doc.find(kTableKey)->data);
    table.find(kNameKey)->data = std::string(newName);
    Written result = detail::writeAndVerify(volume, doc);
    if (result.card.id != before.id || result.card.created != before.created
        || result.card.model != before.model || result.card.name != newName)
        throw Error(markerPath(volume).string() + ": the rename changed more than the name");
    return result;
}

} // namespace loopercat::marker
