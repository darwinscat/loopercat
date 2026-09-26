// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A card as a folder on disk (issue #77), from the theory of what a card is:
//
//   - a fresh folder is a factory-fresh card: ROLAND/DATA with the memory
//     pair, ROLAND/WAVE empty, no SYSTEM; the memory document is the real
//     card's document with every memory in its factory state, byte for byte,
//     and the pair carries the factory generations
//   - the app reads it as a card: the scan finds it, the guard names the
//     RC-5, the browser lists 99 empty memories, the doctor has nothing to say
//   - export is restore pointed at the folder: the states written are
//     exactly what the folder then lists, the memories not written stay
//     factory, both banks agree, the generations count on from the factory
//     pair, a slot restored twice keeps its first take in the archive
//   - import is the scan pointed at a folder: another model's folder is
//     refused by the guard in its old words, a folder without DATA or WAVE
//     is not a card, a folder with strangers in its root still reads
//   - making a folder never writes over one, and a model without a factory
//     body on record gets no folder at all

#include "support.hpp"

#include "../app/OperationId.h"

#include <loopercat/CardFolder.hpp>
#include <loopercat/Catalog.hpp>
#include <loopercat/Commands.hpp>
#include <loopercat/DeviceProfile.hpp>
#include <loopercat/Volume.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
        path = fs::temp_directory_path() / ("loopercat-folder-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

std::string readFixture(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error(std::string("cannot open fixture: ") + path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::set<std::string> namesIn(const fs::path& dir)
{
    std::set<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        out.insert(it->path().filename().string());
    return out;
}

std::map<std::string, std::string> folderBytes(const fs::path& root)
{
    std::map<std::string, std::string> map;
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it)
        if (!it->is_directory())
            map[fs::relative(it->path(), root).string()] = commands::readFileBytes(it->path());
    return map;
}

std::string bytesOf(const std::vector<unsigned char>& wav)
{
    return std::string(reinterpret_cast<const char*>(wav.data()), wav.size());
}

// A slot state as a card would have recorded it: a named memory holding a
// float32 take of `frames` frames, indexed.
commands::SlotState recordedState(int slot, const std::string& name, int frames,
                                  const std::string& fileName)
{
    std::string body = rc0::factorySlotBody(slot);
    body = rc0::setName(body, name);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "WavStat", rc0::kWavStatIndexed);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "WavLen", frames);
    return { body, commands::Take { fileName, bytesOf(testkit::syntheticWav(
                                                  { .tag = 3, .bits = 32, .frames = frames })) } };
}

commands::WriteOptions writeOpts(const fs::path& root, std::string opId = opid::make("op"))
{
    commands::Archive archive = commands::trashFolder(root / "trash", opId);
    return { .backupRoot = root / "backups",
             .opId = std::move(opId),
             .archive = std::move(archive) };
}

} // namespace

int main()
{
    const std::string card = readFixture(LOOPERCAT_RC5_CARD);

    // --- a fresh folder is a factory-fresh card ---

    {
        TempDir tmp;
        const fs::path root = tmp.path / "Card";
        cardfolder::create(root, profile::kRc5);

        // The layout: DATA with the pair, WAVE empty, nothing else — no SYSTEM.
        CHECK(volume::looksLikePedal(root));
        cardfolder::assertCardFolder(root);
        CHECK(volume::detectVolume({ tmp.path / "nothing", root }) == root);
        CHECK(namesIn(root) == std::set<std::string> { "ROLAND" });
        CHECK((namesIn(root / "ROLAND") == std::set<std::string> { "DATA", "WAVE" }));
        CHECK((namesIn(volume::dataDir(root))
               == std::set<std::string> { "MEMORY1.RC0", "MEMORY2.RC0" }));
        CHECK(namesIn(root / "ROLAND" / "WAVE").empty());

        // The memory document is the real card's with every memory reset to
        // the factory body — the header, the 99 <mem> wrappers, the closer,
        // every newline, byte for byte. (57 of the card's memories already are
        // that body; the other 42 are reset here to compare the whole.)
        std::string factoryCard = card;
        for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
            factoryCard = rc0::replaceSlotBody(factoryCard, slot, rc0::factorySlotBody(slot));
        const std::string bank1 = commands::readFileBytes(volume::memoryPath(root, 1));
        const std::string bank2 = commands::readFileBytes(volume::memoryPath(root, 2));
        CHECK(rc0::splitFile(bank1).document == rc0::splitFile(factoryCard).document);
        CHECK(rc0::splitFile(bank2).document == rc0::splitFile(bank1).document);
        CHECK_EQ(cardfolder::factoryMemoryDocument(profile::kRc5),
                 rc0::splitFile(factoryCard).document);

        // The trailer is the factory pair (golden.json tailMarkers), and it
        // is the only difference between the two files.
        CHECK_EQ(*rc0::tailMarker(bank1), rc0::tailMarkerFor(1));
        CHECK_EQ(*rc0::tailMarker(bank2), rc0::tailMarkerFor(2));
        CHECK_EQ(bank1.size(), bank2.size());
        CHECK_EQ(bank1.substr(0, bank1.size() - 4), bank2.substr(0, bank2.size() - 4));

        // The app reads it as a card: the guard names the RC-5, the browser
        // lists 99 empty memories under their factory names, the doctor has
        // nothing to say.
        // The read takes the bank whose counter is newest, and in the
        // factory pair that is MEMORY2 (0x39 over 0x38) — as on a fresh card.
        const std::string text = commands::readMemory(root);
        CHECK(text == bank2);
        CHECK(&rc0::assertMemoryFile(text) == &profile::kRc5);
        const auto slots = catalog::listSlots(text);
        CHECK_EQ(slots.size(), static_cast<std::size_t>(rc0::kSlotCount));
        for (const auto& slot : slots) {
            CHECK(!slot.hasAudio);
            CHECK_EQ(slot.frames, 0);
            CHECK_EQ(slot.name, rc0::defaultSlotName(slot.slot) + std::string(4, ' '));
            CHECK(volume::listSlotWavs(root, slot.slot).empty());
        }
        CHECK(commands::doctor(root).empty());
    }

    // --- making a folder never writes over one, and refuses what it cannot make ---

    {
        TempDir tmp;
        const fs::path root = tmp.path / "Card";
        cardfolder::create(root, profile::kRc5);
        const auto before = folderBytes(root);
        CHECK_THROWS(cardfolder::create(root, profile::kRc5), "already holds a card folder");
        CHECK(folderBytes(root) == before);
        // A root that is a file.
        commands::writeFileBytes(tmp.path / "file", "x");
        CHECK_THROWS(cardfolder::create(tmp.path / "file", profile::kRc5), "not a directory");
        // An existing empty directory is fine.
        fs::create_directories(tmp.path / "Empty");
        cardfolder::create(tmp.path / "Empty", profile::kRc5);
        CHECK(volume::looksLikePedal(tmp.path / "Empty"));
        // A model without a factory body on record: refused, and no folder
        // appears — not even the directory.
        CHECK_THROWS(cardfolder::create(tmp.path / "TwoTrack", profile::kRc500), "not on record");
        CHECK(!fs::exists(tmp.path / "TwoTrack"));
        CHECK_THROWS(cardfolder::factoryMemoryDocument(profile::kRc500), "not on record");
    }

    // --- export: restore pointed at the folder ---

    {
        TempDir tmp;
        const fs::path root = tmp.path / "Export";
        cardfolder::create(root, profile::kRc5);

        // Three states of a card's timeline go in: slots 5, 42 and 99, each a
        // named memory with an indexed take.
        const auto s5 = recordedState(5, "Verse", 4410, "005_1.WAV");
        const auto s42 = recordedState(42, "Chorus", 8820, "chorus.wav");
        const auto s99 = recordedState(99, "Outro", 2205, "099_1.WAV");
        commands::restore(root, 5, s5, writeOpts(tmp.path, "op-5"));
        commands::restore(root, 42, s42, writeOpts(tmp.path, "op-42"));
        commands::restore(root, 99, s99, writeOpts(tmp.path, "op-99"));

        // The folder lists exactly them.
        const std::string text = commands::readMemory(root);
        const auto slots = catalog::listSlots(text);
        int withAudio = 0;
        for (const auto& slot : slots)
            withAudio += slot.hasAudio ? 1 : 0;
        CHECK_EQ(withAudio, 3);
        CHECK_EQ(slots.at(4).name, "Verse       ");
        CHECK_EQ(slots.at(4).frames, 4410);
        CHECK_EQ(slots.at(41).name, "Chorus      ");
        CHECK_EQ(slots.at(41).frames, 8820);
        CHECK_EQ(slots.at(98).name, "Outro       ");
        CHECK_EQ(slots.at(98).frames, 2205);
        CHECK(volume::listSlotWavs(root, 5) == std::vector<std::string> { "005_1.WAV" });
        CHECK(volume::listSlotWavs(root, 42) == std::vector<std::string> { "chorus.wav" });
        CHECK(volume::listSlotWavs(root, 99) == std::vector<std::string> { "099_1.WAV" });
        CHECK(commands::readFileBytes(volume::wavDir(root, 42) / "chorus.wav") == s42.take->bytes);
        // The bodies are the states', byte for byte; the other 96 memories are
        // still the factory body, byte for byte.
        CHECK(rc0::slotBody(text, 5) == s5.body);
        CHECK(rc0::slotBody(text, 42) == s42.body);
        CHECK(rc0::slotBody(text, 99) == s99.body);
        for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
            if (slot != 5 && slot != 42 && slot != 99) {
                CHECK(rc0::slotBody(text, slot) == rc0::factorySlotBody(slot));
                CHECK(volume::listSlotWavs(root, slot).empty());
            }
        // Both banks agree, and the generations count on from the factory
        // pair: three pair writes past 0x38/0x39 land on 0x3e/0x3f.
        CHECK(rc0::splitFile(commands::readMemory(root, 1)).document
              == rc0::splitFile(commands::readMemory(root, 2)).document);
        CHECK_EQ(*rc0::tailMarker(commands::readMemory(root, 1)), rc0::tailMarkerFor(1) + 6);
        CHECK_EQ(*rc0::tailMarker(commands::readMemory(root, 2)), rc0::tailMarkerFor(2) + 6);
        // Each write backed the folder's pair up first, as on a card.
        CHECK(fs::exists(tmp.path / "backups" / "op-5" / "MEMORY1.RC0"));
        CHECK(fs::exists(tmp.path / "backups" / "op-99" / "MEMORY2.RC0"));
        // The doctor sees a healthy card.
        CHECK(commands::doctor(root).empty());

        // A slot exported twice: the first take goes to the archive before the
        // second lands, as on a card — nothing is ever deleted outright.
        const auto later = recordedState(42, "Chorus v2", 6615, "chorus.wav");
        const auto opts = writeOpts(tmp.path, "op-42b");
        const auto result = commands::restore(root, 42, later, opts);
        CHECK(result.archived == std::vector<std::string> { "chorus.wav" });
        CHECK(commands::readFileBytes(tmp.path / "trash" / "op-42b" / "042_1" / "chorus.wav")
              == s42.take->bytes);
        CHECK(commands::readFileBytes(volume::wavDir(root, 42) / "chorus.wav")
              == later.take->bytes);
        CHECK_EQ(catalog::readSlot(commands::readMemory(root), 42).name, "Chorus v2   ");
        // Without an archive it is refused, and the folder is as it was.
        const auto before = folderBytes(root);
        commands::WriteOptions noArchive = writeOpts(tmp.path, "op-42c");
        noArchive.archive = nullptr;
        CHECK_THROWS(commands::restore(root, 42, s42, noArchive), "needs an archive");
        CHECK(folderBytes(root) == before);

        // The folder is still a folder any card accepts: DATA holds the pair
        // and nothing else, WAVE holds the three slots' folders and no junk.
        CHECK((namesIn(volume::dataDir(root))
               == std::set<std::string> { "MEMORY1.RC0", "MEMORY2.RC0" }));
        CHECK((namesIn(root / "ROLAND" / "WAVE")
               == std::set<std::string> { "005_1", "042_1", "099_1" }));
        CHECK(volume::findJunk(root).empty());
    }

    // --- import: the scan pointed at a folder ---

    {
        TempDir tmp;
        // A folder taken off the two-track model: the shape of a card, and
        // the family guard's old words at the door, before anything is read.
        const fs::path twoTrack = tmp.path / "TwoTrack";
        fs::create_directories(volume::dataDir(twoTrack));
        fs::create_directories(twoTrack / "ROLAND" / "WAVE" / "001_1");
        fs::create_directories(twoTrack / "ROLAND" / "WAVE" / "001_2");
        const std::string foreign = testkit::syntheticTwoTrackMemoryText();
        for (const int fileNo : { 1, 2 })
            commands::writeFileBytes(volume::memoryPath(twoTrack, fileNo),
                                     rc0::setTailMarker(foreign, fileNo));
        CHECK(volume::looksLikePedal(twoTrack));
        cardfolder::assertCardFolder(twoTrack); // the shape is a card's
        // It reads as the two-track card it is, and nothing may be written
        // into it — import is a look, never a restore.
        {
            const auto memories = catalog::listSlots(commands::readMemory(twoTrack));
            CHECK_EQ(memories.size(), static_cast<std::size_t>(rc0::kSlotCount));
            CHECK_EQ(memories.at(0).tracks.size(), 2u);
        }
        CHECK_THROWS(commands::restore(twoTrack, 1, recordedState(1, "X", 4410, "001_1.WAV"),
                                       writeOpts(tmp.path)),
                     "restore refused on an \"RC-500\" card");

        // Not a card: DATA without WAVE, WAVE without DATA, a file, nothing.
        fs::create_directories(tmp.path / "DataOnly" / "ROLAND" / "DATA");
        fs::create_directories(tmp.path / "WaveOnly" / "ROLAND" / "WAVE");
        commands::writeFileBytes(tmp.path / "file", "x");
        for (const auto& name : { "DataOnly", "WaveOnly" }) {
            CHECK(!volume::looksLikePedal(tmp.path / name));
            CHECK_THROWS(cardfolder::assertCardFolder(tmp.path / name),
                         "no ROLAND/DATA and ROLAND/WAVE");
        }
        CHECK_THROWS(cardfolder::assertCardFolder(tmp.path / "file"), "is not a directory");
        CHECK_THROWS(cardfolder::assertCardFolder(tmp.path / "missing"), "does not exist");
        CHECK(!volume::detectVolume(
                   { tmp.path / "DataOnly", tmp.path / "WaveOnly", tmp.path / "file" })
                   .has_value());

        // A card folder with strangers in its root — a note, a marker, a
        // Finder sidecar — still reads as the card it is.
        const fs::path noisy = tmp.path / "Noisy";
        cardfolder::create(noisy, profile::kRc5);
        commands::writeFileBytes(noisy / "README.txt", "my loops");
        commands::writeFileBytes(noisy / "loopercat-card.json", "{}");
        commands::writeFileBytes(noisy / ".DS_Store", "");
        CHECK(volume::looksLikePedal(noisy));
        cardfolder::assertCardFolder(noisy);
        CHECK_EQ(catalog::listSlots(commands::readMemory(noisy)).size(),
                 static_cast<std::size_t>(rc0::kSlotCount));
        CHECK(commands::doctor(noisy).empty()); // strangers in the root are not junk on the card

        // The other half of the same question: the root sidecar is still OUR
        // litter, so a sweep takes it — the player's own files and the marker
        // stay, and the report says nothing either side. A sidecar under
        // ROLAND is the case that IS reported (commands_tests covers that).
        const auto sweptNoisy = volume::sweepJunk(noisy);
        CHECK_EQ(sweptNoisy.removed.size(), static_cast<std::size_t>(1));
        CHECK(!fs::exists(noisy / ".DS_Store"));
        CHECK(fs::exists(noisy / "README.txt"));
        CHECK(fs::exists(noisy / "loopercat-card.json"));
        CHECK(commands::doctor(noisy).empty());

        // A real card's memory file in a folder reads as that card.
        const fs::path real = tmp.path / "Real";
        fs::create_directories(volume::dataDir(real));
        fs::create_directories(real / "ROLAND" / "WAVE");
        for (const int fileNo : { 1, 2 })
            commands::writeFileBytes(volume::memoryPath(real, fileNo), card);
        int withAudio = 0;
        for (const auto& slot : catalog::listSlots(commands::readMemory(real)))
            withAudio += slot.hasAudio ? 1 : 0;
        CHECK_EQ(withAudio, 41);
    }

    return testkit::summary("card_folder");
}
