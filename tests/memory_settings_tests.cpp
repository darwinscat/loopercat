// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Memory Settings (Tier 1) and the section-scoped field surgery underneath,
// attacked from the format's theory (docs/pedal-settings.md, the factory body
// captured from hardware):
//
//   - <Level> exists in BOTH MASTER and RHYTHM: unscoped surgery must refuse,
//     scoped surgery must hit exactly the requested section and never bleed
//   - ranges come from the manual (levels 0..200, pan 0..100) and out-of-range
//     values are typed errors, not clamps
//   - a no-op edit set is a caller bug, not a silent success
//   - writing the values a slot already has reproduces the body byte-for-byte
//   - malformed bodies (missing/duplicated sections) are refused, never
//     "repaired"
//   - the end-to-end command keeps every unrelated byte of the volume intact

#include "support.hpp"

#include <loopercat/Commands.hpp>

#include <chrono>
#include <filesystem>
#include <map>

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-memset-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

fs::path makePedal(const fs::path& root)
{
    const fs::path volume = root / "PEDAL";
    fs::create_directories(volume / "ROLAND" / "WAVE");
    fs::create_directories(volume::dataDir(volume));
    const std::string text = testkit::syntheticMemoryText();
    for (const int fileNo : { 1, 2 })
        commands::writeFileBytes(volume::memoryPath(volume, fileNo),
                                 rc0::setTailMarker(text, fileNo));
    return volume;
}

std::map<std::string, std::string> volumeBytes(const fs::path& volume)
{
    std::map<std::string, std::string> map;
    for (fs::recursive_directory_iterator it(volume), end; it != end; ++it)
        if (!it->is_directory())
            map[fs::relative(it->path(), volume).string()] = commands::readFileBytes(it->path());
    return map;
}

} // namespace

int main()
{
    const std::string factory = rc0::factorySlotBody(1);

    // --- the double-<Level> trap: the reason section scoping exists ---

    // Unscoped surgery on an ambiguous tag must refuse loudly.
    CHECK_THROWS(rc0::field(factory, "Level"), "occurs 2 times");
    CHECK_THROWS(rc0::setField(factory, "Level", 55), "occurs 2 times");

    // Scoped reads see two different fields under the same tag.
    {
        std::string body = rc0::setSectionField(factory, "MASTER", "Level", 42);
        body = rc0::setSectionField(body, "RHYTHM", "Level", 77);
        CHECK_EQ(rc0::sectionField(body, "MASTER", "Level"), 42);
        CHECK_EQ(rc0::sectionField(body, "RHYTHM", "Level"), 77);
    }

    // A tag absent from the requested section is an error even when present
    // elsewhere in the body — no cross-section fallback.
    CHECK_THROWS(rc0::sectionField(factory, "TRACK1", "Level"), "occurs 0 times");
    CHECK_THROWS(rc0::sectionField(factory, "RHYTHM", "Tempo"), "occurs 0 times");

    // Malformed bodies are refused, never repaired.
    CHECK_THROWS(rc0::sectionField(factory, "RHYTHM2", "Level"), "missing <RHYTHM2>");
    {
        const std::string duplicated = factory + "<RHYTHM>\n\t<Level>10</Level>\n</RHYTHM>\n";
        CHECK_THROWS(rc0::sectionField(duplicated, "RHYTHM", "Level"), "occurs more than once");
        const auto open = factory.find("</RHYTHM>");
        const std::string unterminated = factory.substr(0, open);
        CHECK_THROWS(rc0::sectionField(unterminated, "RHYTHM", "Level"), "unterminated <RHYTHM>");
    }

    // --- read: the factory body, values pinned from hardware ---

    {
        const memsettings::Values v = memsettings::read(factory);
        CHECK(!v.reverse);
        CHECK_EQ(v.playLevel, 100);
        CHECK_EQ(v.pan, 50);
        CHECK(!v.rhythmOn);
        CHECK_EQ(v.rhythmLevel, 100);
    }

    // --- apply: field isolation, both directions across the Level pair ---

    {
        const std::string body = memsettings::apply(factory, { .rhythmLevel = 55 });
        CHECK_EQ(rc0::sectionField(body, "RHYTHM", "Level"), 55);
        CHECK_EQ(rc0::sectionField(body, "MASTER", "Level"), 100); // untouched
        CHECK_EQ(memsettings::read(body).rhythmLevel, 55);
    }
    {
        // playLevel is TRACK1-only: neither Level field may move.
        const std::string body = memsettings::apply(factory, { .playLevel = 120 });
        CHECK_EQ(rc0::sectionField(body, "TRACK1", "PlyLvl"), 120);
        CHECK_EQ(rc0::sectionField(body, "MASTER", "Level"), 100);
        CHECK_EQ(rc0::sectionField(body, "RHYTHM", "Level"), 100);
    }
    {
        const std::string body = memsettings::apply(
            factory, { .reverse = true, .pan = 0, .rhythmOn = true });
        const memsettings::Values v = memsettings::read(body);
        CHECK(v.reverse);
        CHECK_EQ(v.pan, 0);
        CHECK(v.rhythmOn);
        CHECK_EQ(v.playLevel, 100);   // absent fields untouched
        CHECK_EQ(v.rhythmLevel, 100);
    }

    // Writing the values the slot already has is byte-identical output — the
    // surgical invariant, on the whole body including whitespace and name.
    {
        const std::string body = memsettings::apply(
            factory, { .reverse = false, .playLevel = 100, .pan = 50, .rhythmOn = false,
                       .rhythmLevel = 100 });
        CHECK(body == factory);
    }

    // --- apply: validation is typed and total ---

    CHECK_THROWS(memsettings::apply(factory, {}), "no memory settings to change");
    CHECK_THROWS(memsettings::apply(factory, { .playLevel = 201 }), "play level out of range");
    CHECK_THROWS(memsettings::apply(factory, { .playLevel = -1 }), "play level out of range");
    CHECK_THROWS(memsettings::apply(factory, { .pan = 101 }), "pan out of range");
    CHECK_THROWS(memsettings::apply(factory, { .pan = -1 }), "pan out of range");
    CHECK_THROWS(memsettings::apply(factory, { .rhythmLevel = 201 }), "rhythm level out of range");
    CHECK_THROWS(memsettings::apply(factory, { .rhythmLevel = -1 }), "rhythm level out of range");

    // Range edges are legal, not off-by-one refusals.
    {
        const memsettings::Values v = memsettings::read(memsettings::apply(
            factory, { .playLevel = 200, .pan = 100, .rhythmLevel = 0 }));
        CHECK_EQ(v.playLevel, 200);
        CHECK_EQ(v.pan, 100);
        CHECK_EQ(v.rhythmLevel, 0);
    }

    // A body with no RHYTHM section cannot take rhythm edits — and validation
    // must reject the bad range BEFORE structure is even consulted.
    {
        const std::string noRhythm = factory.substr(0, factory.find("<RHYTHM>"));
        CHECK_THROWS(memsettings::apply(noRhythm, { .rhythmOn = true }), "missing <RHYTHM>");
        CHECK_THROWS(memsettings::read(noRhythm), "missing <RHYTHM>");
        CHECK_THROWS(memsettings::apply(noRhythm, { .rhythmLevel = 999 }), "out of range");
    }

    // --- the command: end-to-end against a scratch volume ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const auto before = volumeBytes(volume);

        commands::setMemorySettings(volume, 7, { .reverse = true, .rhythmLevel = 63 },
                                    { .backupRoot = tmp.path / "backups", .stamp = "s1" });

        const std::string after = commands::readMemory(volume);
        const memsettings::Values v = catalog::readSlot(after, 7).settings;
        CHECK(v.reverse);
        CHECK_EQ(v.rhythmLevel, 63);

        // Every OTHER slot is reproduced byte-for-byte.
        for (const int slot : { 1, 6, 8, 99 })
            CHECK(rc0::slotBody(after, slot)
                  == rc0::slotBody(rc0::splitFile(before.at("ROLAND/DATA/MEMORY1.RC0")).document,
                                   slot));

        // Both banks advanced past the factory pair, one generation apart.
        CHECK_EQ(*rc0::tailMarker(commands::readFileBytes(volume::memoryPath(volume, 1))), 0x3a);
        CHECK_EQ(*rc0::tailMarker(commands::readFileBytes(volume::memoryPath(volume, 2))), 0x3b);

        // The pre-write backup landed.
        CHECK(commands::readFileBytes(tmp.path / "backups" / "s1" / "MEMORY1.RC0")
              == before.at("ROLAND/DATA/MEMORY1.RC0"));

        // An invalid edit leaves the volume byte-identical (validate first).
        const auto snapshotBefore = volumeBytes(volume);
        const memsettings::Edits badPan { .pan = 999 };
        const commands::WriteOptions opts2 { .backupRoot = tmp.path / "backups", .stamp = "s2" };
        CHECK_THROWS(commands::setMemorySettings(volume, 7, badPan, opts2), "pan out of range");
        CHECK(volumeBytes(volume) == snapshotBefore);
    }

    return testkit::summary("memory_settings");
}
