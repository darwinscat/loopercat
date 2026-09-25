// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The bank pair, for both kinds of file the card carries.
//
// A card holds MEMORY1/MEMORY2 and SYSTEM1/SYSTEM2, and the pair mechanism is
// the card's, not one file kind's: the pedal saves into one bank, stamps its
// write counter one past the other, and reconciles the pair at its next boot.
// So reading means "the bank the counters name as newest" and writing means
// "both banks, stamped past the highest generation, each verified".
//
// The tests are written from what that mechanism promises, and from the two
// ways it can betray a musician: reading a stale bank (a just-saved loop
// looks absent, and a mutation started from it clobbers the save), and
// writing a settings file the pedal will consider older than its own.
//
// Where the two kinds must differ, they are checked against each other: the
// memory pair may restart at the factory generation, because we know what it
// is; the settings pair may not, because we do not.

#include "support.hpp"

#include <loopercat/Commands.hpp>
#include <loopercat/SystemFile.hpp>
#include <loopercat/Volume.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-bank-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

std::string systemFixture()
{
    std::ifstream in(LOOPERCAT_RC5_SYSTEM, std::ios::binary);
    if (!in)
        throw Error("cannot open fixture: " LOOPERCAT_RC5_SYSTEM);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// A scratch card: the synthetic memory pair the other suites use, plus the
// real settings file in both settings banks.
fs::path makeCard(const fs::path& root)
{
    const fs::path volume = root / "PEDAL";
    fs::create_directories(volume / "ROLAND" / "WAVE");
    fs::create_directories(volume::dataDir(volume));
    const std::string memory = testkit::syntheticMemoryText();
    for (const int fileNo : { 1, 2 })
        commands::writeFileBytes(volume::memoryPath(volume, fileNo),
                                 rc0::setTailMarker(memory, fileNo));
    const std::string system = systemFixture();
    for (const int fileNo : { 1, 2 })
        commands::writeFileBytes(volume::systemPath(volume, fileNo),
                                 rc0::setTailGeneration(system, 100 + static_cast<unsigned>(fileNo)));
    return volume;
}

commands::WriteOptions options(const fs::path& backupRoot, const std::string& opId)
{
    commands::WriteOptions o;
    o.backupRoot = backupRoot;
    o.opId = opId;
    return o;
}

// A settings document with one control changed, so two banks can be told
// apart by their content rather than by their counter.
std::string withCtl2(const std::string& system, long long value)
{
    return sysfile::setField(system, sysfile::kSectionCtl, "Ctl2", value);
}

} // namespace

int main()
{
    const std::string system = systemFixture();

    // --- reading: the counter decides, for both kinds ---

    {
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path);

        // SYSTEM2 carries the higher generation in makeCard, so it wins; the
        // two banks are told apart by a control's value, not by the counter.
        commands::writeFileBytes(volume::systemPath(volume, 1),
                                 rc0::setTailGeneration(withCtl2(system, 11), 101));
        commands::writeFileBytes(volume::systemPath(volume, 2),
                                 rc0::setTailGeneration(withCtl2(system, 22), 102));
        CHECK_EQ(sysfile::field(commands::readSystem(volume), sysfile::kSectionCtl, "Ctl2"), 22);

        // Flip the counters and the other bank wins — nothing else changed.
        commands::writeFileBytes(volume::systemPath(volume, 1),
                                 rc0::setTailGeneration(withCtl2(system, 11), 104));
        CHECK_EQ(sysfile::field(commands::readSystem(volume), sysfile::kSectionCtl, "Ctl2"), 11);

        // Serial arithmetic, not plain comparison: 0 is one past 0xFFFFFFFF.
        commands::writeFileBytes(volume::systemPath(volume, 1),
                                 rc0::setTailGeneration(withCtl2(system, 11), 0xFFFFFFFFu));
        commands::writeFileBytes(volume::systemPath(volume, 2),
                                 rc0::setTailGeneration(withCtl2(system, 22), 0u));
        CHECK_EQ(sysfile::field(commands::readSystem(volume), sysfile::kSectionCtl, "Ctl2"), 22);

        // A bank without a trailer cannot vote, whatever it contains.
        commands::writeFileBytes(volume::systemPath(volume, 2), withCtl2(system, 22) + "\n\x01");
        commands::writeFileBytes(volume::systemPath(volume, 1),
                                 rc0::setTailGeneration(withCtl2(system, 11), 7u));
        CHECK_EQ(sysfile::field(commands::readSystem(volume), sysfile::kSectionCtl, "Ctl2"), 11);
    }

    // --- the two kinds are never confused for one another ---

    {
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path);
        const std::string memory = testkit::syntheticMemoryText();

        // A memory document sitting in the settings banks is refused, and the
        // other way round: the file's contents decide, never its name.
        for (const int fileNo : { 1, 2 })
            commands::writeFileBytes(volume::systemPath(volume, fileNo), memory);
        CHECK_THROWS(commands::readSystem(volume), "memory file");

        const fs::path second = tmp.path / "OTHER";
        fs::create_directories(volume::dataDir(second));
        for (const int fileNo : { 1, 2 })
            commands::writeFileBytes(volume::memoryPath(second, fileNo),
                                     rc0::setTailGeneration(system, 100));
        CHECK_THROWS(commands::readMemory(second), "<mem");
    }

    // --- writing the settings pair ---

    {
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path);
        const std::string before = commands::readSystem(volume);
        const long long memoryNumber = sysfile::currentMemory(before);
        const std::string edited = withCtl2(before, 25);

        const commands::WriteResult result =
            commands::writeSystemPair(volume, edited, options(tmp.path / "backups", "op-1"));

        // Both banks carry the edit, one generation apart, past the highest
        // counter the card had (101 and 102 from makeCard).
        for (const int fileNo : { 1, 2 }) {
            const std::string written =
                commands::readFileBytes(volume::systemPath(volume, fileNo));
            sysfile::assertSystemFile(written);
            CHECK_EQ(sysfile::field(written, sysfile::kSectionCtl, "Ctl2"), 25);
            CHECK_EQ(*rc0::tailMarker(written), 102u + static_cast<unsigned>(fileNo));
            // Only the trailer tells the two banks apart from the document
            // we handed in — and the pedal's own business survived.
            CHECK_EQ(written.substr(0, written.size() - 4), edited.substr(0, edited.size() - 4));
            CHECK_EQ(sysfile::currentMemory(written), memoryNumber);
        }

        // The card was backed up before the write, memories included.
        CHECK(result.backedUp.has_value());
        CHECK(fs::exists(result.backedUp->dest / "SYSTEM1.RC0"));
        CHECK(fs::exists(result.backedUp->dest / "MEMORY1.RC0"));
        CHECK_EQ(sysfile::field(commands::readFileBytes(result.backedUp->dest / "SYSTEM1.RC0"),
                                sysfile::kSectionCtl, "Ctl2"),
                 sysfile::field(before, sysfile::kSectionCtl, "Ctl2"));

        // The memories are untouched by a settings write.
        CHECK(commands::readMemory(volume) == rc0::setTailMarker(testkit::syntheticMemoryText(), 2)
              || commands::readMemory(volume)
                     == rc0::setTailMarker(testkit::syntheticMemoryText(), 1));
    }

    // A settings write refuses what it cannot be sure of, and changes nothing.
    {
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path);

        // A memory document is not a settings file.
        CHECK_THROWS(commands::writeSystemPair(volume, testkit::syntheticMemoryText(),
                                              options(tmp.path / "backups", "op-2")),
                     "memory file");

        // Neither bank readable: there is no generation to continue from, and
        // unlike the memory pair there is no factory pair to restart at.
        const std::string keep = commands::readFileBytes(volume::systemPath(volume, 1));
        for (const int fileNo : { 1, 2 })
            fs::remove(volume::systemPath(volume, fileNo));
        CHECK_THROWS(commands::writeSystemPair(volume, withCtl2(system, 25),
                                              options(tmp.path / "backups", "op-3")),
                     "no write generation to continue from");
        for (const int fileNo : { 1, 2 })
            CHECK(!fs::exists(volume::systemPath(volume, fileNo))); // nothing was created
        commands::writeFileBytes(volume::systemPath(volume, 1), keep);

        // Banks that ARE readable settings files but carry no recognisable
        // trailer are refused too: there is still no generation to continue
        // from, and a settings file must never claim one we invented.
        const std::string trailerless = rc0::splitFile(system).document + "\n\x01\x02";
        for (const int fileNo : { 1, 2 })
            commands::writeFileBytes(volume::systemPath(volume, fileNo), trailerless);
        sysfile::assertSystemFile(trailerless); // it is a settings file; only its tail is odd
        CHECK_THROWS(commands::writeSystemPair(volume, withCtl2(system, 25),
                                              options(tmp.path / "backups", "op-4")),
                     "no write generation to continue from");
    }

    // The sidecars macOS leaves on a FAT card are swept after a settings
    // write, exactly as after a memory write — the pedal chokes on them.
    {
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path);
        commands::writeFileBytes(volume / "ROLAND" / "DATA" / "._SYSTEM1.RC0", "sidecar");
        const commands::WriteResult result = commands::writeSystemPair(
            volume, withCtl2(commands::readSystem(volume), 30),
            options(tmp.path / "backups", "op-5"));
        CHECK_EQ(result.swept.size(), static_cast<std::size_t>(1));
        CHECK(!fs::exists(volume / "ROLAND" / "DATA" / "._SYSTEM1.RC0"));
    }

    // --- the memory pair keeps its own rule: it may restart at the factory ---

    {
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path);
        const std::string memory = testkit::syntheticMemoryText();
        // Both banks readable as memory files but carrying no recognisable
        // trailer: no generation to continue from, so the count restarts at
        // the factory pair the whole family starts from.
        const std::string trailerless = rc0::splitFile(memory).document + "\n\x01\x02";
        for (const int fileNo : { 1, 2 })
            commands::writeFileBytes(volume::memoryPath(volume, fileNo), trailerless);
        commands::writeMemoryPair(volume, memory, options(tmp.path / "backups", "op-6"));
        for (const int fileNo : { 1, 2 })
            CHECK_EQ(*rc0::tailMarker(commands::readFileBytes(volume::memoryPath(volume, fileNo))),
                     rc0::tailMarkerFor(fileNo));
    }

    // --- the paths themselves ---

    {
        const fs::path volume = "/Volumes/BOSS RC-5";
        CHECK_EQ(volume::bankPath(volume, volume::Bank::memory, 2).filename().string(),
                 std::string("MEMORY2.RC0"));
        CHECK_EQ(volume::bankPath(volume, volume::Bank::system, 1).filename().string(),
                 std::string("SYSTEM1.RC0"));
        CHECK(volume::memoryPath(volume, 1) == volume::bankPath(volume, volume::Bank::memory, 1));
        CHECK(volume::systemPath(volume, 2) == volume::bankPath(volume, volume::Bank::system, 2));
        CHECK_THROWS(volume::bankPath(volume, volume::Bank::system, 0), "fileNo must be 1 or 2");
        CHECK_THROWS(volume::bankPath(volume, volume::Bank::memory, 3), "fileNo must be 1 or 2");
    }

    return testkit::summary("bank_pair");
}
