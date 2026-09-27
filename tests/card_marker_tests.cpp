// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The card marker, tested from what the file IS — the TOML table in
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

// A canonical marker with a synthetic id. The file contract is independent
// of the parser/writer implementation.
const std::string kKittyFile = R"([loopercat_card]
format = 1
id = "11111111-2222-4333-8444-555555555555"
name = "RC-5 Kitty"
model = "RC-5"
created = "2026-09-24T21:34:33Z"
by = "LooperCat"
)";

std::string replace(std::string text, std::string_view from, std::string_view to)
{
    text.replace(text.find(from), from.size(), to);
    return text;
}

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

    // --- reading canonical TOML ---

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
        const auto parsed = felitronics::toml::parse(kKittyFile);
        CHECK(std::holds_alternative<felitronics::toml::Table>(parsed));
        CHECK_EQ(felitronics::toml::write(std::get<felitronics::toml::Table>(parsed)), kKittyFile);
        // Escapes are decoded before the name's byte limit and control rule.
        put(marker::markerPath(tmp.path), replace(kKittyFile, "RC-5 Kitty", "caf\\u00e9 \\U0001F431"));
        CHECK_EQ(marker::read(tmp.path)->name, std::string("caf\xc3\xa9 \xf0\x9f\x90\xb1"));
        // The courtesy writer field is optional, as before.
        put(marker::markerPath(tmp.path), replace(kKittyFile, "by = \"LooperCat\"\n", ""));
        CHECK_EQ(marker::read(tmp.path)->model, "RC-5");
    }

    // --- every refusal names the file, reason and source position ---

    {
        TempDir tmp;
        const auto refuse = [&](std::string_view bytes, std::string_view reason, std::string_view position) {
            put(marker::markerPath(tmp.path), bytes);
            const std::string expected = "loopercat.toml: " + std::string(reason) + " at " + std::string(position);
            CHECK_THROWS(marker::read(tmp.path), expected);
            // Neither mint nor rename may repair or overwrite an unreadable marker.
            CHECK_THROWS(marker::mint(tmp.path, "new"), expected);
            CHECK_THROWS(marker::rename(tmp.path, "new"), expected);
            CHECK_EQ(slurp(marker::markerPath(tmp.path)), bytes);
        };
        refuse("[loopercat_card]\nname = 'Kitty'\n", "UnsupportedValue", "2:8");
        refuse("[loopercat_card]\ncreated = 2026-09-24T21:34:33Z\n", "InvalidNumber", "2:11");
        refuse("[loopercat_card]\nid = \"a\"\nid = \"b\"\n", "DuplicateKey", "3:1");
        refuse("garbage", "ExpectedEquals", "1:8");
        refuse("{\"loopercat_card\": 1}", "ExpectedKey", "1:1");
        refuse("", "not a LooperCat card marker (no [loopercat_card] table)", "1:1");
        refuse("[another]\nformat = 1\n", "not a LooperCat card marker (no [loopercat_card] table)", "1:1");
        refuse("loopercat_card = 1\n", "not a LooperCat card marker (no [loopercat_card] table)", "1:18");
        refuse(replace(kKittyFile, "format = 1", "format = 2"),
               "written by a newer LooperCat (format 2); this build reads format 1", "2:10");
        refuse(replace(kKittyFile, "format = 1", "format = 10"),
               "written by a newer LooperCat (format 10); this build reads format 1", "2:10");
        refuse(replace(kKittyFile, "format = 1", "format = \"1\""), "\"format\" is not an integer", "2:10");
        refuse(replace(kKittyFile, "format = 1", "format = 1.0"), "\"format\" is not an integer", "2:10");
        refuse(replace(kKittyFile, "format = 1\n", ""), "no \"format\" field", "1:1");
        refuse(replace(kKittyFile, "format = 1", "format = 0"), "unsupported format \"0\"", "2:10");
        refuse(replace(kKittyFile, "format = 1", "format = -1"), "unsupported format \"-1\"", "2:10");
        refuse("[loopercat_card]\nformat = 1\n", "no \"id\" field", "1:1");
        refuse(replace(kKittyFile, "\"11111111-2222-4333-8444-555555555555\"", "7"), "\"id\" is not a string", "3:6");
        refuse(replace(kKittyFile, "11111111-2222-4333-8444-555555555555", ""), "the \"id\" field is empty", "3:6");
        refuse(replace(kKittyFile, "name = \"RC-5 Kitty\"\n", ""), "no \"name\" field", "1:1");
        refuse(replace(kKittyFile, "\"RC-5 Kitty\"", "true"), "\"name\" is not a string", "4:8");
        refuse(replace(kKittyFile, "model = \"RC-5\"\n", ""), "no \"model\" field", "1:1");
        refuse(replace(kKittyFile, "model = \"RC-5\"", "model = 5"), "\"model\" is not a string", "5:9");
        refuse(replace(kKittyFile, "model = \"RC-5\"", "model = \"\""), "the \"model\" field is empty", "5:9");
        refuse(replace(kKittyFile, "created = \"2026-09-24T21:34:33Z\"\n", ""), "no \"created\" field", "1:1");
        refuse(replace(kKittyFile, "\"2026-09-24T21:34:33Z\"", "9"), "\"created\" is not a string", "6:11");
        refuse(replace(kKittyFile, "RC-5 Kitty", ""), "a pedal name cannot be empty", "4:8");
        refuse(replace(kKittyFile, "RC-5 Kitty", std::string(65, 'a')),
               "a pedal name is at most 64 bytes of UTF-8; this one is 65", "4:8");
        for (const auto escape : { "\\u0007", "\\t", "\\n", "\\u007f" })
            refuse(replace(kKittyFile, "RC-5 Kitty", escape), "a pedal name cannot contain control characters", "4:8");
    }

    // --- the file's shape on disk ---

    {
        TempDir tmp;
        fs::create_directories(marker::markerPath(tmp.path));
        CHECK_THROWS(marker::read(tmp.path), "is a directory");
        CHECK_THROWS(marker::mint(tmp.path, "RC-5"), "is a directory");
        fs::remove_all(marker::markerPath(tmp.path));
        put(marker::markerPath(tmp.path), std::string(64 * 1024 + 1, ' '));
        CHECK_THROWS(marker::read(tmp.path), "too large to be a card marker");
        CHECK_THROWS(marker::mint(tmp.path, "RC-5"), "too large to be a card marker");
        const auto atLimit = kKittyFile + "#" + std::string(64 * 1024 - kKittyFile.size() - 1, ' ');
        put(marker::markerPath(tmp.path), atLimit);
        CHECK_EQ(marker::read(tmp.path)->name, "RC-5 Kitty");
    }

#if !defined(_WIN32)
    // Permission bits are not a portable Windows fault seam. These tests run
    // unprivileged on POSIX, like the command suites' write-failure tests.
    {
        TempDir tmp;
        const auto file = marker::markerPath(tmp.path);
        put(file, kKittyFile);
        fs::permissions(file, fs::perms::none);
        CHECK_THROWS(marker::read(tmp.path), "cannot read");
        CHECK_THROWS(marker::mint(tmp.path, "RC-5"), "cannot read");
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write);
        CHECK_EQ(slurp(file), kKittyFile);
        fs::remove(file);
        fs::create_symlink(tmp.path / "missing", file);
        CHECK_THROWS(marker::mint(tmp.path, "RC-5"), "cannot read");
        CHECK(fs::is_symlink(file));
        CHECK(!fs::exists(tmp.path / "missing"));
    }
#endif

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

        // Canonical bytes, in the documented field order.
        const std::string expected = "[loopercat_card]\nformat = 1\nid = \"" + written.card.id
                                   + "\"\nname = \"RC-5 Kitty\"\nmodel = \"RC-5\"\ncreated = \""
                                   + written.card.created + "\"\nby = \"LooperCat\"\n";
        const auto parsed = felitronics::toml::parse(slurp(marker::markerPath(tmp.path)));
        CHECK(std::holds_alternative<felitronics::toml::Table>(parsed));
        CHECK_EQ(felitronics::toml::write(std::get<felitronics::toml::Table>(parsed)), expected);
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
        put(tmp.path / "._loopercat.toml", "sidecar"); // what macOS plants beside the write
        put(tmp.path / ".DS_Store", "finder");
        put(tmp.path / "ROLAND" / "WAVE" / "001_1" / "._01 - Loop.wav", "sidecar");
        put(tmp.path / ".Spotlight-V100" / "._store", "not ours");
        put(tmp.path / "System Volume Information" / "._sys", "not ours");
        put(tmp.path / "README.txt", "a player's own note");   // the root is a lived-in place
        put(tmp.path / "notes.md", "and another");
        put(tmp.path / "._README.txt", "macOS's dropping beside it");
        const auto written = marker::mint(tmp.path, "RC-5 Kitty");
        CHECK_EQ(written.sweep.removed.size(), 4u);
        CHECK(fs::exists(tmp.path / "README.txt"));
        CHECK(fs::exists(tmp.path / "notes.md"));
        CHECK(!fs::exists(tmp.path / "._README.txt"));
        CHECK(written.sweep.failed.empty());
        CHECK(!fs::exists(tmp.path / "._loopercat.toml"));
        CHECK(!fs::exists(tmp.path / ".DS_Store"));
        CHECK(!fs::exists(tmp.path / "ROLAND" / "WAVE" / "001_1" / "._01 - Loop.wav"));
        CHECK(fs::exists(tmp.path / ".Spotlight-V100" / "._store"));
        CHECK(fs::exists(tmp.path / "System Volume Information" / "._sys"));
        CHECK(fs::exists(marker::markerPath(tmp.path)));

        put(tmp.path / "._loopercat.toml", "sidecar again");
        const auto renamed = marker::rename(tmp.path, "RC-5 Drummer");
        CHECK_EQ(renamed.sweep.removed.size(), 1u);
        CHECK(!fs::exists(tmp.path / "._loopercat.toml"));
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
        const std::string foreign = replace(kKittyFile, "name =", "color = \"violet\"\nname =")
                                  + "lives = 9\nasleep = true\ngain = -1.250\n"
                                    "tags = [\"cat\", \"pedal\"]\n"
                                    "\n[loopercat_card.extra]\nkeep = true\n"
                                    "\n[another]\nvalue = \"untouched\"\n";
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
        put(marker::markerPath(tmp.path), replace(kKittyFile, "name = \"RC-5 Kitty\"\n", ""));
        CHECK_THROWS(marker::rename(tmp.path, "RC-5 Drummer"), "no \"name\" field");
        put(marker::markerPath(tmp.path), replace(kKittyFile, "format = 1", "format = 2"));
        CHECK_THROWS(marker::rename(tmp.path, "RC-5 Drummer"), "newer LooperCat");
    }

    // --- the marker is never half-written: staged, verified, renamed over ---

    {
        // A leftover .part from an interrupted attempt neither blocks nor
        // pollutes: the next write overwrites it, and the rename consumes it.
        TempDir tmp;
        makeCard(tmp.path);
        put(tmp.path / "loopercat.toml.part", "[loopercat_card]\nname = \"torn");
        const auto written = marker::mint(tmp.path, "RC-5 Kitty");
        CHECK(!fs::exists(tmp.path / "loopercat.toml.part"));
        CHECK_EQ(marker::read(tmp.path)->id, written.card.id);
    }
    {
        // Something in the way of the staging name: the write fails BEFORE the
        // marker is touched — a card keeps its id, a rename keeps everything.
        TempDir tmp;
        makeCard(tmp.path);
        fs::create_directories(tmp.path / "loopercat.toml.part");
        CHECK_THROWS(marker::mint(tmp.path, "RC-5 Kitty"), "cannot write");
        CHECK(!fs::exists(marker::markerPath(tmp.path)));
        fs::remove_all(tmp.path / "loopercat.toml.part");
        marker::mint(tmp.path, "RC-5 Kitty");
        const std::string before = slurp(marker::markerPath(tmp.path));
        fs::create_directories(tmp.path / "loopercat.toml.part");
        CHECK_THROWS(marker::rename(tmp.path, "RC-5 Drummer"), "cannot write");
        CHECK_EQ(slurp(marker::markerPath(tmp.path)), before);
        CHECK_EQ(marker::read(tmp.path)->name, "RC-5 Kitty");
    }
    {
        // The staging file's own sidecar is swept with the marker's.
        TempDir tmp;
        makeCard(tmp.path);
        marker::mint(tmp.path, "RC-5 Kitty");
        put(tmp.path / "._loopercat.toml.part", "sidecar");
        put(tmp.path / "._loopercat.toml", "sidecar");
        const auto renamed = marker::rename(tmp.path, "RC-5 Drummer");
        CHECK_EQ(renamed.sweep.removed.size(), 2u);
        CHECK(!fs::exists(tmp.path / "._loopercat.toml.part"));
        CHECK(!fs::exists(tmp.path / "._loopercat.toml"));
    }

    // --- no volume is not "no marker" ---

    {
        TempDir tmp;
        const fs::path gone = tmp.path / "BOSS RC-5";
        CHECK_THROWS(marker::read(gone), "no volume at");
        CHECK_THROWS(marker::mint(gone, "RC-5 Kitty"), "no volume at");
        CHECK_THROWS(marker::rename(gone, "RC-5 Kitty"), "no volume at");
        CHECK(!fs::exists(gone));
        // A file where the volume should be is not a volume either.
        put(gone, "not a directory");
        CHECK_THROWS(marker::read(gone), "no volume at");
    }

    // --- comments and blank lines are discarded; values and order survive ---

    {
        TempDir tmp;
        const auto edited = "# My pedal\n\n" + replace(kKittyFile, "format = 1", "format = 1 # current")
                          + "\n# Keep the color\ncolor = \"violet\"\n";
        put(marker::markerPath(tmp.path), edited);
        CHECK_EQ(marker::read(tmp.path)->name, "RC-5 Kitty");
        marker::rename(tmp.path, "Drummer");
        CHECK_EQ(slurp(marker::markerPath(tmp.path)),
                 replace(kKittyFile, "RC-5 Kitty", "Drummer") + "color = \"violet\"\n");
    }

    // --- the obsolete JSON marker is a stranger, never read or removed ---

    {
        TempDir tmp;
        makeCard(tmp.path);
        const auto legacy = tmp.path / "loopercat-card." "json";
        const std::string oldBytes = "{\"loopercat_card\":1,\"id\":\"old\"}\n";
        put(legacy, oldBytes);
        CHECK(!marker::read(tmp.path));
        CHECK_EQ(marker::mint(tmp.path, "RC-5").card.name, "RC-5");
        CHECK(fs::exists(tmp.path / "loopercat.toml"));
        CHECK_EQ(slurp(legacy), oldBytes);
    }

    // --- read-back faults: equal parsed values are not equal bytes ---

    for (const bool stagedFault : { true, false }) {
        for (const bool missing : { true, false }) {
            TempDir tmp;
            put(marker::markerPath(tmp.path), kKittyFile);
            put(tmp.path / "._loopercat.toml", "sidecar");
            const auto doc = std::get<felitronics::toml::Table>(felitronics::toml::parse(
                replace(kKittyFile, "RC-5 Kitty", "Drummer")));
            const auto faultyRead = [&](const fs::path& path) -> std::optional<std::string> {
                const bool isPart = path.extension() == ".part";
                if (isPart == stagedFault) {
                    if (missing)
                        return std::nullopt;
                    // Valid TOML with identical fields, but the bytes did not stick.
                    return slurp(path) + "# unexpected bytes\n";
                }
                return slurp(path);
            };
            CHECK_THROWS(marker::detail::writeAndVerify(tmp.path, doc, faultyRead),
                         "read back differently from what was written");
            CHECK(fs::exists(tmp.path / "._loopercat.toml")); // never sweep after failure
            CHECK_EQ(slurp(marker::markerPath(tmp.path)),
                     stagedFault ? kKittyFile : replace(kKittyFile, "RC-5 Kitty", "Drummer"));
        }
    }

    return testkit::summary("card_marker");
}
