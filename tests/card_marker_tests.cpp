// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The card marker, tested from what the file IS — the six-field format in
// CardMarker.hpp and the facts measured on the pedals (2026-09-24) — not
// from how the module is written. The rules that matter most, and that the
// mutation checks in the PR broke on purpose:
//
//   - a card's id is minted ONCE: a second mint is refused and the file is
//     left byte for byte as it was;
//   - a rename changes the name and nothing else — id, created, model, and
//     fields this build has never heard of all survive, in their order;
//   - the model is read from the card's own MEMORY1.RC0 root element, never
//     guessed: a card that does not say gets no marker;
//   - a file that is not ours, or is broken, is refused with the reason —
//     and never overwritten;
//   - every write ends with the sidecar macOS plants at the ROOT swept away,
//     while the foreign directories a host keeps there are left alone.

#include "support.hpp"

#include <loopercat/CardMarker.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

// A scratch tree that cleans up after itself.
struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-marker-test-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

void put(const fs::path& p, std::string_view bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A card as the pedal lays it out: the memory bank pair with the given root
// name, and the WAVE tree. Only the root opener matters to the marker.
fs::path makeCard(const fs::path& root, std::string_view family = "RC-5")
{
    const std::string bank = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<database name=\""
                           + std::string(family) + "\" revision=\"0\">\n<mem id=\"0\">\n</mem>\n</database>\n";
    put(root / "ROLAND" / "DATA" / "MEMORY1.RC0", bank);
    put(root / "ROLAND" / "DATA" / "MEMORY2.RC0", bank);
    fs::create_directories(root / "ROLAND" / "WAVE" / "001_1");
    return root;
}

// The bytes the first markers were written in (2026-09-24), ids replaced by
// a synthetic one: the shape this module must read and reproduce.
const std::string kKittyFile = "{\n"
                               "  \"loopercat_card\": 1,\n"
                               "  \"id\": \"11111111-2222-4333-8444-555555555555\",\n"
                               "  \"name\": \"RC-5 Kitty\",\n"
                               "  \"model\": \"RC-5\",\n"
                               "  \"created\": \"2026-09-24T21:34:33Z\",\n"
                               "  \"by\": \"LooperCat\"\n"
                               "}\n";

bool isHexLower(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

// RFC 4122, version 4: 8-4-4-4-12 lowercase hex, '4' leading the third group,
// one of 8/9/a/b leading the fourth.
bool looksLikeUuid4(const std::string& s)
{
    if (s.size() != 36)
        return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !isHexLower(s[i]))
            return false;
    }
    return s[14] == '4' && (s[19] == '8' || s[19] == '9' || s[19] == 'a' || s[19] == 'b');
}

// "YYYY-MM-DDTHH:MM:SSZ"
bool looksLikeIsoUtc(const std::string& s)
{
    if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':'
        || s[19] != 'Z')
        return false;
    for (const std::size_t i : { 0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u })
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

} // namespace

int main()
{
    // --- reading: a card that has never met LooperCat has no marker ---

    {
        TempDir tmp;
        makeCard(tmp.path);
        CHECK(!marker::read(tmp.path).has_value());
    }

    // --- reading the file as it was written on the pedals ---

    {
        TempDir tmp;
        put(marker::markerPath(tmp.path), kKittyFile);
        const auto card = marker::read(tmp.path);
        CHECK(card.has_value());
        if (card) {
            CHECK_EQ(card->id, "11111111-2222-4333-8444-555555555555");
            CHECK_EQ(card->name, "RC-5 Kitty");
            CHECK_EQ(card->model, "RC-5");
            CHECK_EQ(card->created, "2026-09-24T21:34:33Z");
        }
        // The parser and the writer agree on that exact shape: a round trip
        // reproduces the file byte for byte.
        CHECK_EQ(marker::json::serialize(marker::json::parse(kKittyFile)), kKittyFile);
        // A Windows editor's BOM is not a value.
        put(marker::markerPath(tmp.path), "\xEF\xBB\xBF" + kKittyFile);
        CHECK_EQ(marker::read(tmp.path)->name, "RC-5 Kitty");
        // Whitespace is free, as JSON says.
        put(marker::markerPath(tmp.path),
            "{\"loopercat_card\":1,\"id\":\"x\",\"name\":\"n\",\"model\":\"RC-5\",\"created\":\"c\"}");
        CHECK_EQ(marker::read(tmp.path)->id, "x");
        // The "by" field is a courtesy, not a requirement: another writer may omit it.
        CHECK_EQ(marker::read(tmp.path)->model, "RC-5");
    }

    // --- a name in any script: escapes decode, raw UTF-8 passes through ---

    {
        TempDir tmp;
        // "caf\u00e9 \ud83d\udc31" — an accent and a surrogate pair (U+1F431).
        put(marker::markerPath(tmp.path),
            "{\"loopercat_card\":1,\"id\":\"x\",\"name\":\"caf\\u00e9 \\ud83d\\udc31\","
            "\"model\":\"RC-5\",\"created\":\"c\"}");
        CHECK_EQ(marker::read(tmp.path)->name, std::string("caf\xc3\xa9 \xf0\x9f\x90\xb1"));
        put(marker::markerPath(tmp.path),
            "{\"loopercat_card\":1,\"id\":\"x\",\"name\":\"K\xc3\xa4tzchen \\\"Kitty\\\" \\\\ \\/ \\t\","
            "\"model\":\"RC-5\",\"created\":\"c\"}");
        CHECK_EQ(marker::read(tmp.path)->name, std::string("K\xc3\xa4tzchen \"Kitty\" \\ / \t"));
    }

    // --- not ours, or not readable by this build: refused, with the reason ---

    {
        TempDir tmp;
        const auto refuse = [&](std::string_view bytes, const char* reason) {
            put(marker::markerPath(tmp.path), bytes);
            CHECK_THROWS(marker::read(tmp.path), reason);
        };
        refuse("{\"foo\": 1}", "not a LooperCat card marker");
        refuse("{}", "not a LooperCat card marker");
        refuse("", "a card marker is a JSON object");
        refuse("[]", "a card marker is a JSON object");
        refuse("null", "a card marker is a JSON object");
        refuse("{\"loopercat_card\": 2, \"id\": \"x\", \"name\": \"n\", \"model\": \"RC-5\", \"created\": \"c\"}",
               "newer LooperCat (format 2)");
        refuse("{\"loopercat_card\": 10, \"id\": \"x\"}", "newer LooperCat (format 10)");
        refuse("{\"loopercat_card\": \"1\", \"id\": \"x\"}", "is not a number");
        refuse("{\"loopercat_card\": 1.5, \"id\": \"x\"}", "unsupported format \"1.5\"");
        refuse("{\"loopercat_card\": 0, \"id\": \"x\"}", "unsupported format \"0\"");
        refuse("{\"loopercat_card\": -1, \"id\": \"x\"}", "unsupported format \"-1\"");
        refuse("{\"loopercat_card\": 1}", "no \"id\" field");
        refuse("{\"loopercat_card\": 1, \"id\": 7}", "\"id\" is not a string");
        refuse("{\"loopercat_card\": 1, \"id\": \"\", \"name\": \"n\", \"model\": \"RC-5\", \"created\": \"c\"}",
               "\"id\" field is empty");
        refuse("{\"loopercat_card\": 1, \"id\": \"x\"}", "no \"name\" field");
        refuse("{\"loopercat_card\": 1, \"id\": \"x\", \"name\": \"n\"}", "no \"model\" field");
        refuse("{\"loopercat_card\": 1, \"id\": \"x\", \"name\": \"n\", \"model\": \"\", \"created\": \"c\"}",
               "\"model\" field is empty");
        refuse("{\"loopercat_card\": 1, \"id\": \"x\", \"name\": \"n\", \"model\": \"RC-5\"}",
               "no \"created\" field");
        refuse("{\"loopercat_card\": 1, \"id\": \"x\", \"name\": null, \"model\": \"RC-5\", \"created\": \"c\"}",
               "\"name\" is not a string");
    }

    // --- broken JSON: every break is named ---

    {
        TempDir tmp;
        const auto refuse = [&](std::string_view bytes, const char* reason) {
            put(marker::markerPath(tmp.path), bytes);
            CHECK_THROWS(marker::read(tmp.path), reason);
        };
        refuse("{\"loopercat_card\": 1, \"id\": \"x\"", "unterminated object");
        refuse("{\"loopercat_card\": 1, \"id\": \"x", "unterminated string");
        refuse("{\"loopercat_card\": 1, \"id\": \"x\"} x", "trailing bytes");
        refuse("{\"loopercat_card\": 1, \"id\": \"x\"}}", "trailing bytes");
        refuse("{\"loopercat_card\": 1, \"id\": {\"a\": 1}}", "nested values");
        refuse("{\"loopercat_card\": 1, \"id\": [1]}", "nested values");
        refuse("{\"loopercat_card\": 1, \"id\": \"a\", \"id\": \"b\"}", "duplicate field \"id\"");
        refuse("{\"loopercat_card\": 1, \"id\": \"\\x\"}", "unknown escape");
        refuse("{\"loopercat_card\": 1, \"id\": \"a\nb\"}", "control character");
        refuse("{\"loopercat_card\": 1, \"id\": \"\\ud83d\"}", "high surrogate without its low half");
        refuse("{\"loopercat_card\": 1, \"id\": \"\\ud83d\\u0041\"}", "followed by a non-surrogate");
        refuse("{\"loopercat_card\": 1, \"id\": \"\\udc31\"}", "low surrogate on its own");
        refuse("{\"loopercat_card\": 1, \"id\": \"\\u12\"}", "bad hex digit");
        refuse("{\"loopercat_card\": 01}", "leading zero");
        refuse("{\"loopercat_card\": 1.}", "digits after '.'");
        refuse("{\"loopercat_card\": 1e}", "digits in the exponent");
        refuse("{\"loopercat_card\": tru}", "unexpected token");
        refuse("{loopercat_card: 1}", "expected a string");
        refuse("{\"loopercat_card\" 1}", "':' after a field name");
        refuse("{\"loopercat_card\": 1 \"id\": \"x\"}", "expected ',' or '}'");
        refuse("{\"loopercat_card\": }", "unexpected token");
        refuse("{\"loopercat_card\": 1,}", "expected a string");
    }

    // --- the file's shape on disk ---

    {
        TempDir tmp;
        fs::create_directories(marker::markerPath(tmp.path));
        CHECK_THROWS(marker::read(tmp.path), "is a directory");
        fs::remove_all(marker::markerPath(tmp.path));
        put(marker::markerPath(tmp.path), std::string(64 * 1024 + 1, ' '));
        CHECK_THROWS(marker::read(tmp.path), "too large to be a card marker");
    }

    // --- minting: an id, the name, the model the card itself declares ---

    {
        TempDir tmp;
        makeCard(tmp.path);
        const auto written = marker::mint(tmp.path, "RC-5 Kitty");
        CHECK(looksLikeUuid4(written.card.id));
        CHECK_EQ(written.card.name, "RC-5 Kitty");
        CHECK_EQ(written.card.model, "RC-5");
        CHECK(looksLikeIsoUtc(written.card.created));
        CHECK(written.sweep.failed.empty());

        // On disk, in exactly the shape the first markers had.
        const std::string expected = "{\n  \"loopercat_card\": 1,\n  \"id\": \"" + written.card.id
                                   + "\",\n  \"name\": \"RC-5 Kitty\",\n  \"model\": \"RC-5\",\n  \"created\": \""
                                   + written.card.created + "\",\n  \"by\": \"LooperCat\"\n}\n";
        CHECK_EQ(slurp(marker::markerPath(tmp.path)), expected);

        // And read() sees the same card.
        const auto back = marker::read(tmp.path);
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(back->id, written.card.id);
            CHECK_EQ(back->created, written.card.created);
        }

        // Once. The second mint is refused and the file is untouched.
        CHECK_THROWS(marker::mint(tmp.path, "RC-5 Drummer"), "already carries a marker");
        CHECK_THROWS(marker::mint(tmp.path, "RC-5 Drummer"), written.card.id);
        CHECK_EQ(slurp(marker::markerPath(tmp.path)), expected);
    }

    // --- the model is the card's word, never a guess ---

    {
        TempDir tmp;
        makeCard(tmp.path, "RC-500");
        CHECK_EQ(marker::mint(tmp.path, "RC-500 Bob").card.model, "RC-500");
    }
    {
        // No memory files at all: no model, no marker, nothing written.
        TempDir tmp;
        fs::create_directories(tmp.path / "ROLAND" / "DATA");
        CHECK_THROWS(marker::mint(tmp.path, "RC-5 Kitty"), "cannot tell the card's model");
        CHECK(!fs::exists(marker::markerPath(tmp.path)));
    }
    {
        // MEMORY1 unreadable, its bank twin fine: the twin answers.
        TempDir tmp;
        makeCard(tmp.path, "RC-500");
        fs::remove(tmp.path / "ROLAND" / "DATA" / "MEMORY1.RC0");
        CHECK_EQ(marker::mint(tmp.path, "RC-500 Bob").card.model, "RC-500");
    }
    {
        // MEMORY1 damaged, its twin fine: the twin answers.
        TempDir tmp;
        makeCard(tmp.path, "RC-500");
        put(tmp.path / "ROLAND" / "DATA" / "MEMORY1.RC0", "<garbage>");
        CHECK_EQ(marker::mint(tmp.path, "RC-500 Bob").card.model, "RC-500");
    }
    {
        // Both damaged: refused, and MEMORY1's reason is the one named.
        TempDir tmp;
        makeCard(tmp.path);
        put(tmp.path / "ROLAND" / "DATA" / "MEMORY1.RC0", "<database revision=\"0\">");
        put(tmp.path / "ROLAND" / "DATA" / "MEMORY2.RC0", "<garbage>");
        CHECK_THROWS(marker::mint(tmp.path, "RC-5 Kitty"), "MEMORY1.RC0: not an RC0 memory file");
        CHECK(!fs::exists(marker::markerPath(tmp.path)));
    }

    // --- two cards, two ids ---

    {
        TempDir a;
        TempDir b;
        makeCard(a.path);
        makeCard(b.path);
        CHECK(marker::mint(a.path, "one").card.id != marker::mint(b.path, "two").card.id);
    }

    // --- a broken marker is never overwritten ---

    {
        TempDir tmp;
        makeCard(tmp.path);
        put(marker::markerPath(tmp.path), "{\"loopercat_card\": 1, \"id\": \"x\"");
        CHECK_THROWS(marker::mint(tmp.path, "RC-5 Kitty"), "unterminated object");
        CHECK_EQ(slurp(marker::markerPath(tmp.path)), "{\"loopercat_card\": 1, \"id\": \"x\"");
        put(marker::markerPath(tmp.path), "{\"something\": \"else\"}");
        CHECK_THROWS(marker::mint(tmp.path, "RC-5 Kitty"), "not a LooperCat card marker");
        CHECK_EQ(slurp(marker::markerPath(tmp.path)), "{\"something\": \"else\"}");
    }

    // --- what a name may be ---

    {
        TempDir tmp;
        makeCard(tmp.path);
        CHECK_THROWS(marker::mint(tmp.path, ""), "cannot be empty");
        CHECK_THROWS(marker::mint(tmp.path, std::string(65, 'a')), "at most 64 bytes");
        CHECK_THROWS(marker::mint(tmp.path, "a\nb"), "control characters");
        CHECK_THROWS(marker::mint(tmp.path, "a\x7f"), "control characters");
        CHECK_THROWS(marker::mint(tmp.path, "\xff"), "valid UTF-8");
        CHECK_THROWS(marker::mint(tmp.path, "\xc0\xaf"), "valid UTF-8");         // overlong '/'
        CHECK_THROWS(marker::mint(tmp.path, "\xed\xa0\x80"), "valid UTF-8");     // a surrogate
        CHECK_THROWS(marker::mint(tmp.path, "\xf0\x9f\x90"), "valid UTF-8");     // cut short
        CHECK(!fs::exists(marker::markerPath(tmp.path)));
        // The name is judged before anything else — a card with no memory
        // files still hears about its empty name first.
        TempDir bare;
        CHECK_THROWS(marker::mint(bare.path, ""), "cannot be empty");
        // 64 bytes exactly is a name; so are 16 cats (4 bytes each).
        std::string cats;
        for (int i = 0; i < 16; ++i)
            cats += "\xf0\x9f\x90\xb1";
        CHECK_EQ(marker::mint(tmp.path, cats).card.name, cats);
        TempDir other;
        makeCard(other.path);
        CHECK_EQ(marker::mint(other.path, std::string(64, 'a')).card.name, std::string(64, 'a'));
        CHECK_THROWS(marker::rename(other.path, cats + "\xf0\x9f\x90\xb1"), "at most 64 bytes");
    }

    // --- every write sweeps the root, and only the root and ROLAND ---

    {
        TempDir tmp;
        makeCard(tmp.path);
        put(tmp.path / "._loopercat-card.json", "sidecar"); // what macOS plants beside the write
        put(tmp.path / ".DS_Store", "finder");
        put(tmp.path / "ROLAND" / "WAVE" / "001_1" / "._01 - Loop.wav", "sidecar");
        put(tmp.path / ".Spotlight-V100" / "._store", "not ours");
        put(tmp.path / "System Volume Information" / "._sys", "not ours");
        const auto written = marker::mint(tmp.path, "RC-5 Kitty");
        CHECK_EQ(written.sweep.removed.size(), 3u);
        CHECK(written.sweep.failed.empty());
        CHECK(!fs::exists(tmp.path / "._loopercat-card.json"));
        CHECK(!fs::exists(tmp.path / ".DS_Store"));
        CHECK(!fs::exists(tmp.path / "ROLAND" / "WAVE" / "001_1" / "._01 - Loop.wav"));
        CHECK(fs::exists(tmp.path / ".Spotlight-V100" / "._store"));
        CHECK(fs::exists(tmp.path / "System Volume Information" / "._sys"));
        CHECK(fs::exists(marker::markerPath(tmp.path)));

        put(tmp.path / "._loopercat-card.json", "sidecar again");
        const auto renamed = marker::rename(tmp.path, "RC-5 Drummer");
        CHECK_EQ(renamed.sweep.removed.size(), 1u);
        CHECK(!fs::exists(tmp.path / "._loopercat-card.json"));
    }

    // --- rename: the name, and nothing else ---

    {
        TempDir tmp;
        makeCard(tmp.path);
        const auto minted = marker::mint(tmp.path, "RC-5 Kitty");
        const std::string before = slurp(marker::markerPath(tmp.path));
        const auto renamed = marker::rename(tmp.path, "RC-5 Drummer");
        CHECK_EQ(renamed.card.name, "RC-5 Drummer");
        CHECK_EQ(renamed.card.id, minted.card.id);
        CHECK_EQ(renamed.card.created, minted.card.created);
        CHECK_EQ(renamed.card.model, "RC-5");
        // Byte for byte, the old file with one value swapped.
        std::string expected = before;
        expected.replace(expected.find("\"RC-5 Kitty\""), std::string("\"RC-5 Kitty\"").size(),
                         "\"RC-5 Drummer\"");
        CHECK_EQ(slurp(marker::markerPath(tmp.path)), expected);
        CHECK_EQ(marker::read(tmp.path)->name, "RC-5 Drummer");
    }
    {
        // Fields this build has never heard of — a later format's, another
        // tool's — survive, in their order, with their values.
        TempDir tmp;
        const std::string foreign = "{\n"
                                    "  \"loopercat_card\": 1,\n"
                                    "  \"id\": \"11111111-2222-4333-8444-555555555555\",\n"
                                    "  \"colour\": \"orange\",\n"
                                    "  \"name\": \"RC-5 Kitty\",\n"
                                    "  \"model\": \"RC-5\",\n"
                                    "  \"created\": \"2026-09-24T21:34:33Z\",\n"
                                    "  \"lives\": 9,\n"
                                    "  \"asleep\": true,\n"
                                    "  \"collar\": null,\n"
                                    "  \"by\": \"LooperCat\"\n"
                                    "}\n";
        put(marker::markerPath(tmp.path), foreign);
        marker::rename(tmp.path, "RC-5 Drummer");
        std::string expected = foreign;
        expected.replace(expected.find("\"RC-5 Kitty\""), std::string("\"RC-5 Kitty\"").size(),
                         "\"RC-5 Drummer\"");
        CHECK_EQ(slurp(marker::markerPath(tmp.path)), expected);
    }
    {
        // A name that needs escaping goes in escaped and comes back whole.
        TempDir tmp;
        makeCard(tmp.path);
        marker::mint(tmp.path, "plain");
        marker::rename(tmp.path, "Say \"hi\" \\ back");
        CHECK(slurp(marker::markerPath(tmp.path)).find("\"Say \\\"hi\\\" \\\\ back\"") != std::string::npos);
        CHECK_EQ(marker::read(tmp.path)->name, "Say \"hi\" \\ back");
    }
    {
        TempDir tmp;
        makeCard(tmp.path);
        CHECK_THROWS(marker::rename(tmp.path, "RC-5 Drummer"), "no marker yet");
        CHECK(!fs::exists(marker::markerPath(tmp.path)));
        marker::mint(tmp.path, "RC-5 Kitty");
        const std::string before = slurp(marker::markerPath(tmp.path));
        CHECK_THROWS(marker::rename(tmp.path, ""), "cannot be empty");
        CHECK_THROWS(marker::rename(tmp.path, "a\rb"), "control characters");
        CHECK_EQ(slurp(marker::markerPath(tmp.path)), before);
        // A marker without a name field is not renamed into one: strict, both ways.
        put(marker::markerPath(tmp.path), "{\"loopercat_card\": 1, \"id\": \"x\", \"model\": \"RC-5\", \"created\": \"c\"}");
        CHECK_THROWS(marker::rename(tmp.path, "RC-5 Drummer"), "no \"name\" field");
        // Nor is a foreign or newer file.
        put(marker::markerPath(tmp.path), "{\"loopercat_card\": 2, \"id\": \"x\", \"name\": \"n\", \"model\": \"RC-5\", \"created\": \"c\"}");
        CHECK_THROWS(marker::rename(tmp.path, "RC-5 Drummer"), "newer LooperCat");
    }

    // --- the writer's escaping ---

    CHECK_EQ(marker::json::quote("a\"b\\c\n\x01"), "\"a\\\"b\\\\c\\n\\u0001\"");
    CHECK_EQ(marker::json::quote("caf\xc3\xa9"), "\"caf\xc3\xa9\"");
    CHECK_EQ(marker::json::serialize({}), "{\n}\n");

    return testkit::summary("card_marker");
}
