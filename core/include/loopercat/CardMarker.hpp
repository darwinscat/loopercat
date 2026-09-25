// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The card marker: LooperCat's own identity for a pedal's storage —
// /loopercat-card.json at the volume root.
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
// The pedal tolerates it: written on both RC-5s and the RC-500, ejected,
// power-cycled, re-read byte for byte (sha256 equal) while the pedal rewrote
// its own SYSTEM banks around it — it neither removes nor touches a foreign
// file at the root (it has booted beside macOS's .Spotlight-V100 there since
// January 2025). The one hazard is macOS itself: every write plants a
// ._loopercat-card.json sidecar beside the file, so every write here ends
// with volume::sweepJunk, which walks the root for exactly this reason.
//
// The file is JSON — six flat fields — written once (mint) and rewritten
// only to change the name (rename); the id never changes after mint. The
// parser is this header's own: the app never links a JSON library (nlohmann
// is test-tier only, by the CMake rule), and six flat fields do not earn a
// dependency. It is strict — anything that is not a flat object of strings,
// numbers and literals is refused with the reason — and fields this build
// does not know survive a rename untouched, so a later format can add some
// without an older build destroying them.
//
//   {
//     "loopercat_card": 1,                 format magic + schema version
//     "id": "527a2b7a-da5c-4910-...",      the card's identity, minted once
//     "name": "RC-5 Kitty",                the player's name for the pedal
//     "model": "RC-5",                     the card's own MEMORY1.RC0 root element
//     "created": "2026-09-24T21:34:33Z",   UTC, when the id was minted
//     "by": "LooperCat"
//   }

#pragma once

#include "Error.hpp"
#include "Rc0.hpp"
#include "Volume.hpp"

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
#include <vector>

namespace loopercat::marker {

namespace fs = std::filesystem;

inline constexpr std::string_view kFileName = "loopercat-card.json";
inline constexpr std::string_view kFormatKey = "loopercat_card";
inline constexpr std::int64_t kFormat = 1;
inline constexpr std::string_view kIdKey = "id";
inline constexpr std::string_view kNameKey = "name";
inline constexpr std::string_view kModelKey = "model";
inline constexpr std::string_view kCreatedKey = "created";
inline constexpr std::string_view kByKey = "by";
inline constexpr std::string_view kWriter = "LooperCat";

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

// ---------------------------------------------------------------------------
// The flat JSON the marker is written in: an object of string keys whose
// values are strings, numbers or the literals true/false/null. Nested objects
// and arrays are refused — the marker has no use for them, and a reader that
// "supports" what it will never write is a reader nobody tests.
// ---------------------------------------------------------------------------
namespace json {

    enum class Kind { string, number, literal }; // literal: true, false, null — kept as written

    struct Field {
        std::string key;
        Kind kind;
        std::string value; // decoded text for a string, the raw token otherwise
    };

    using Document = std::vector<Field>;

    namespace detail {

        struct Cursor {
            std::string_view text;
            std::size_t at = 0;
            bool done() const { return at >= text.size(); }
            char peek() const { return text[at]; }
        };

        [[noreturn]] inline void fail(const Cursor& c, const std::string& what)
        {
            throw Error(std::string(kFileName) + ": " + what + " at byte " + std::to_string(c.at));
        }

        inline void skipWs(Cursor& c)
        {
            while (!c.done()
                   && (c.peek() == ' ' || c.peek() == '\t' || c.peek() == '\n' || c.peek() == '\r'))
                ++c.at;
        }

        inline void expect(Cursor& c, char ch, const char* what)
        {
            if (c.done() || c.peek() != ch)
                fail(c, std::string("expected ") + what);
            ++c.at;
        }

        inline unsigned hexDigit(Cursor& c)
        {
            if (c.done())
                fail(c, "unterminated \\u escape");
            const char ch = c.peek();
            ++c.at;
            if (ch >= '0' && ch <= '9')
                return static_cast<unsigned>(ch - '0');
            if (ch >= 'a' && ch <= 'f')
                return static_cast<unsigned>(ch - 'a' + 10);
            if (ch >= 'A' && ch <= 'F')
                return static_cast<unsigned>(ch - 'A' + 10);
            fail(c, "bad hex digit in a \\u escape");
        }

        inline unsigned hex4(Cursor& c)
        {
            unsigned v = 0;
            for (int i = 0; i < 4; ++i)
                v = (v << 4) | hexDigit(c);
            return v;
        }

        inline void appendUtf8(std::string& out, unsigned cp)
        {
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        inline std::string parseString(Cursor& c)
        {
            expect(c, '"', "a string");
            std::string out;
            while (true) {
                if (c.done())
                    fail(c, "unterminated string");
                const char ch = c.peek();
                ++c.at;
                if (ch == '"')
                    return out;
                if (static_cast<unsigned char>(ch) < 0x20)
                    fail(c, "control character inside a string");
                if (ch != '\\') {
                    out.push_back(ch);
                    continue;
                }
                if (c.done())
                    fail(c, "unterminated escape");
                const char e = c.peek();
                ++c.at;
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = hex4(c);
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // A high surrogate: its low half must follow as \uXXXX.
                        if (c.text.compare(c.at, 2, "\\u") != 0)
                            fail(c, "a high surrogate without its low half");
                        c.at += 2;
                        const unsigned low = hex4(c);
                        if (low < 0xDC00 || low > 0xDFFF)
                            fail(c, "a high surrogate followed by a non-surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail(c, "a low surrogate on its own");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail(c, "unknown escape");
                }
            }
        }

        inline std::string parseNumber(Cursor& c)
        {
            const std::size_t start = c.at;
            if (!c.done() && c.peek() == '-')
                ++c.at;
            const auto digits = [&c] {
                std::size_t n = 0;
                while (!c.done() && c.peek() >= '0' && c.peek() <= '9') {
                    ++c.at;
                    ++n;
                }
                return n;
            };
            const std::size_t intStart = c.at;
            if (digits() == 0)
                fail(c, "expected digits");
            if (c.text[intStart] == '0' && c.at - intStart > 1)
                fail(c, "a number with a leading zero");
            if (!c.done() && c.peek() == '.') {
                ++c.at;
                if (digits() == 0)
                    fail(c, "expected digits after '.'");
            }
            if (!c.done() && (c.peek() == 'e' || c.peek() == 'E')) {
                ++c.at;
                if (!c.done() && (c.peek() == '+' || c.peek() == '-'))
                    ++c.at;
                if (digits() == 0)
                    fail(c, "expected digits in the exponent");
            }
            return std::string(c.text.substr(start, c.at - start));
        }

        inline std::string parseLiteral(Cursor& c)
        {
            for (const std::string_view word : { "true", "false", "null" })
                if (c.text.compare(c.at, word.size(), word) == 0) {
                    c.at += word.size();
                    return std::string(word);
                }
            fail(c, "unexpected token");
        }

    } // namespace detail

    // Strict, as the header says. A UTF-8 BOM is skipped: Windows editors leave
    // one on a hand-edited file, and it is an encoding mark, not a value.
    // Duplicate keys are refused — one name, one value.
    inline Document parse(std::string_view text)
    {
        detail::Cursor c { text };
        if (text.starts_with("\xEF\xBB\xBF"))
            c.at = 3;
        detail::skipWs(c);
        detail::expect(c, '{', "'{' \xe2\x80\x94 a card marker is a JSON object");
        Document doc;
        detail::skipWs(c);
        if (!c.done() && c.peek() == '}') {
            ++c.at;
        } else {
            while (true) {
                detail::skipWs(c);
                Field field;
                field.key = detail::parseString(c);
                for (const auto& known : doc)
                    if (known.key == field.key)
                        detail::fail(c, "duplicate field \"" + field.key + "\"");
                detail::skipWs(c);
                detail::expect(c, ':', "':' after a field name");
                detail::skipWs(c);
                if (c.done())
                    detail::fail(c, "missing value");
                const char ch = c.peek();
                if (ch == '"') {
                    field.kind = Kind::string;
                    field.value = detail::parseString(c);
                } else if (ch == '{' || ch == '[') {
                    detail::fail(c, "nested values are not part of a card marker");
                } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
                    field.kind = Kind::number;
                    field.value = detail::parseNumber(c);
                } else {
                    field.kind = Kind::literal;
                    field.value = detail::parseLiteral(c);
                }
                doc.push_back(std::move(field));
                detail::skipWs(c);
                if (c.done())
                    detail::fail(c, "unterminated object");
                if (c.peek() == ',') {
                    ++c.at;
                    continue;
                }
                if (c.peek() == '}') {
                    ++c.at;
                    break;
                }
                detail::fail(c, "expected ',' or '}'");
            }
        }
        detail::skipWs(c);
        if (!c.done())
            detail::fail(c, "trailing bytes after the object");
        return doc;
    }

    // A JSON string literal for `s`: the two characters JSON must escape, the
    // control characters, and nothing else — UTF-8 goes through as it is.
    inline std::string quote(std::string_view s)
    {
        std::string out = "\"";
        for (const char ch : s) {
            const auto u = static_cast<unsigned char>(ch);
            switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (u < 0x20) {
                    // The remaining control characters, as \u00XX.
                    static constexpr char digits[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(digits[static_cast<std::size_t>(u >> 4)]);
                    out.push_back(digits[static_cast<std::size_t>(u & 0x0F)]);
                } else {
                    out.push_back(ch);
                }
            }
        }
        out += '"';
        return out;
    }

    // Two-space indent, one field per line, a trailing newline — the shape
    // the first markers were written in (2026-09-24), so a rename reproduces
    // those files byte for byte outside the name.
    inline std::string serialize(const Document& doc)
    {
        std::string out = "{\n";
        for (std::size_t i = 0; i < doc.size(); ++i) {
            out += "  " + quote(doc[i].key) + ": ";
            out += doc[i].kind == Kind::string ? quote(doc[i].value) : doc[i].value;
            out += i + 1 < doc.size() ? ",\n" : "\n";
        }
        out += "}\n";
        return out;
    }

    inline const Field* find(const Document& doc, std::string_view key)
    {
        for (const auto& field : doc)
            if (field.key == key)
                return &field;
        return nullptr;
    }

} // namespace json

// ---------------------------------------------------------------------------
// The marker itself.
// ---------------------------------------------------------------------------
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
        for (const char ch : name) {
            const auto u = static_cast<unsigned char>(ch);
            if (u < 0x20 || u == 0x7F)
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

    // The file's bytes, or no value when there is no such file. Anything else
    // in the way — a directory under the name, an unreadable file, one too
    // large to be a marker — is an error, not an absence.
    inline std::optional<std::string> readIfPresent(const fs::path& file)
    {
        std::error_code ec;
        const auto status = fs::status(file, ec);
        if (!fs::exists(status))
            return std::nullopt;
        if (ec)
            throw Error("cannot read " + file.string() + ": " + ec.message());
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

    inline const std::string& requireString(const json::Document& doc, std::string_view key)
    {
        const auto* field = json::find(doc, key);
        if (field == nullptr)
            throw Error(std::string(kFileName) + ": no \"" + std::string(key) + "\" field");
        if (field->kind != json::Kind::string)
            throw Error(std::string(kFileName) + ": \"" + std::string(key) + "\" is not a string");
        return field->value;
    }

    // Ours, and of a format this build reads. A format number above ours is
    // named as such: the file is fine, the build is old.
    inline void assertFormat(const json::Document& doc)
    {
        const auto* field = json::find(doc, kFormatKey);
        if (field == nullptr)
            throw Error(std::string(kFileName) + ": not a LooperCat card marker (no \""
                        + std::string(kFormatKey) + "\" field)");
        if (field->kind != json::Kind::number)
            throw Error(std::string(kFileName) + ": \"" + std::string(kFormatKey)
                        + "\" is not a number");
        if (field->value == std::to_string(kFormat))
            return;
        bool digitsOnly = !field->value.empty();
        for (const char ch : field->value)
            digitsOnly = digitsOnly && ch >= '0' && ch <= '9';
        // Digits only and not ours: longer than ours or lexically above it is a
        // later format (ours is a single digit; "10" and "2" are both later).
        if (digitsOnly && (field->value.size() > 1 || field->value > std::to_string(kFormat)))
            throw Error(std::string(kFileName) + ": written by a newer LooperCat (format "
                        + field->value + "); this build reads format " + std::to_string(kFormat));
        throw Error(std::string(kFileName) + ": unsupported format \"" + field->value
                    + "\" (this build reads format " + std::to_string(kFormat) + ")");
    }

    inline Card cardOf(const json::Document& doc)
    {
        assertFormat(doc);
        Card card { requireString(doc, kIdKey), requireString(doc, kNameKey),
                    requireString(doc, kModelKey), requireString(doc, kCreatedKey) };
        if (card.id.empty())
            throw Error(std::string(kFileName) + ": the \"" + std::string(kIdKey) + "\" field is empty");
        if (card.model.empty())
            throw Error(std::string(kFileName) + ": the \"" + std::string(kModelKey) + "\" field is empty");
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
    const auto bytes = detail::readIfPresent(markerPath(volume));
    if (!bytes)
        return std::nullopt;
    return detail::cardOf(json::parse(*bytes));
}

struct Written {
    Card card;
    volume::SweepResult sweep; // the sidecar macOS plants beside the write, removed — or, in `failed`, not
};

namespace detail {

    // Write, read back, compare — then sweep the sidecar the write may have
    // planted. A write that reads back differently is an error, never a
    // shrug: the id in this file is what the history knows the card by.
    inline Written writeAndVerify(const fs::path& volume, const json::Document& doc)
    {
        const fs::path file = markerPath(volume);
        const std::string bytes = json::serialize(doc);
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out)
                throw Error("cannot write " + file.string());
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.flush();
            if (!out.good())
                throw Error("cannot write " + file.string());
        }
        const auto back = readIfPresent(file);
        if (!back || *back != bytes)
            throw Error(file.string() + " read back differently from what was written");
        return Written { cardOf(json::parse(*back)), volume::sweepJunk(volume) };
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
    if (const auto existing = read(volume))
        throw Error("this card already carries a marker (id " + existing->id + ", name \""
                    + existing->name + "\"); the id is written once and never rewritten");
    const std::string model = detail::modelOf(volume);
    const json::Document doc {
        { std::string(kFormatKey), json::Kind::number, std::to_string(kFormat) },
        { std::string(kIdKey), json::Kind::string, detail::uuid4() },
        { std::string(kNameKey), json::Kind::string, std::string(name) },
        { std::string(kModelKey), json::Kind::string, model },
        { std::string(kCreatedKey), json::Kind::string, detail::isoUtcNow() },
        { std::string(kByKey), json::Kind::string, std::string(kWriter) },
    };
    return detail::writeAndVerify(volume, doc);
}

// Change the name and nothing else: id, created, model, and every field this
// build does not know, stay exactly as they were — in their order.
inline Written rename(const fs::path& volume, std::string_view newName)
{
    detail::assertName(newName);
    const auto bytes = detail::readIfPresent(markerPath(volume));
    if (!bytes)
        throw Error("this card has no marker yet \xe2\x80\x94 nothing to rename");
    json::Document doc = json::parse(*bytes);
    const Card before = detail::cardOf(doc);
    for (auto& field : doc)
        if (field.key == kNameKey) {
            field.kind = json::Kind::string;
            field.value = std::string(newName);
        }
    Written result = detail::writeAndVerify(volume, doc);
    if (result.card.id != before.id || result.card.created != before.created
        || result.card.model != before.model || result.card.name != newName)
        throw Error(markerPath(volume).string() + ": the rename changed more than the name");
    return result;
}

} // namespace loopercat::marker
