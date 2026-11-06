// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat - see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Push on the two-track model: into a memory with no take on any track, and
// nowhere else (decided 2026-10-29).
//
// The theory, from the pedal's own files (2026-09-22/23 readings): a take
// lands on track 1 as the memory's first recording, with the body the
// one-track model gets - TRACK1 WavStat, WavLen, MeasLen, Measure, RecTmp;
// MASTER Tempo and LpLen, the length of the first recording; track 2 and
// the MASTER fields the pedal keeps for itself (CurrentTrack, LpMod)
// untouched. What the pedal makes of a second take of another length beside
// a pushed one has not been asked of it, so a memory with a take on either
// track - configured or merely in its folder - is refused in a sentence that
// names the track, force or not, config written or not; and the profile gate
// stands on the drop-only path as on the configured one. The state before
// the push is a unit a restore takes back: both tracks empty again.

#include "support.hpp"
#include "archive_support.hpp"
#include "two_track_card.hpp"

#include <loopercat/Catalog.hpp>
#include <loopercat/Commands.hpp>
#include <loopercat/DeviceProfile.hpp>
#include <loopercat/Params.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/Volume.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path()
        / ("loopercat-rc500-push-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { fs::create_directories(path); }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec)
            testkit::fail("scratch folder left behind: " + ec.message(), __FILE__, __LINE__);
    }
};

commands::WriteOptions writeOpts(const fs::path& root, std::string opId)
{
    commands::Archive archive = testkit::fileArchive(root / "archive", opId);
    return { .opId = std::move(opId), .archive = std::move(archive), .journal = testkit::noOpJournal() };
}

// Every file of the volume with its bytes: the card as a whole, to say
// "nothing moved" after a refusal.
std::map<std::string, std::string> volumeBytes(const fs::path& volume)
{
    std::map<std::string, std::string> out;
    for (const auto& entry : fs::recursive_directory_iterator(volume))
        if (entry.is_regular_file())
            out[fs::relative(entry.path(), volume).generic_string()] =
                commands::readFileBytes(entry.path());
    return out;
}

// A source long enough for a real tempo: 30 s -> 8 measures, 96.0 BPM by the
// pedal's formula (params::computeSlotParams), the pedal's own format.
constexpr int kFrames = 1323000;

fs::path writeSource(const fs::path& dir, const std::string& name)
{
    const auto bytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = kFrames });
    const fs::path source = dir / name;
    commands::writeFileBytes(source, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                      bytes.size()));
    return source;
}

long long trackField(const std::string& body, int track, const char* tag)
{
    return rc0::sectionField(body, rc0::trackSectionName(track), tag);
}

} // namespace

int main()
{
    // The card: memory 1 recorded on both tracks, memory 11 on track 2
    // alone, memory 42 as the factory left it.
    TempDir tmp;
    const fs::path volume = testkit::makeTwoTrackCard(
        tmp.path, { { 1, 1, "001_1.WAV", 44100, 1 }, { 1, 2, "001_2.WAV", 22050, 2 },
                    { 11, 2, "011_2.WAV", 8820, 5 } });
    const fs::path source = writeSource(tmp.path, "take.wav");
    const auto trackFiles = [&volume](int slot, int track) {
        return volume::listTrackWavs(volume, profile::kRc500, slot, track);
    };
    {
        // The pedal's MASTER carries CurrentTrack beside LpMod (the 2026-09-22
        // card: 0 and 0 on an untouched memory); the synthetic text has the
        // latter and not the former, so memory 42 gets it here, as the card
        // has it, and the push can be seen leaving both alone.
        std::string text = commands::readMemory(volume);
        std::string body = rc0::slotBody(text, 42);
        const std::string_view after = "</LpLen>";
        const auto at = body.find(after);
        CHECK(at != std::string::npos);
        body.insert(at + after.size(), "\n\t<CurrentTrack>0</CurrentTrack>");
        commands::writeMemoryPair(volume, rc0::replaceSlotBody(text, 42, body),
                                  testkit::unrecordedWrite());
    }

    // --- what the catalog offers, before anything is pushed ---

    {
        const auto memories = catalog::listSlots(commands::readMemory(volume));
        CHECK(!catalog::uploadOffered(memories.at(0)));  // both tracks recorded
        CHECK(!catalog::uploadOffered(memories.at(10))); // track 2 alone is a used memory
        CHECK(catalog::uploadOffered(memories.at(41)));  // nothing on either track
        // The one-track model is offered a push whatever the slot holds: an
        // occupied slot takes a replace there.
        auto one = catalog::listSlots(testkit::syntheticMemoryText()).at(0);
        CHECK(catalog::uploadOffered(one));
        one.tracks.at(0).hasAudio = true;
        CHECK(catalog::uploadOffered(one));
    }

    // --- an empty memory takes the upload, on track 1, with the RC-5 body rules ---

    const commands::SlotState before { rc0::slotBody(commands::readMemory(volume), 42), {} };
    {
        const std::string bodyBefore = before.body;
        const auto result =
            commands::push(volume, source, 42, { .name = "Pushed", .write = writeOpts(tmp.path, "op-1") });
        CHECK(result.configured);
        CHECK(result.dest == volume::trackDir(volume, profile::kRc500, 42, 1) / "take.wav");
        CHECK_EQ(trackFiles(42, 1).size(), 1u);
        CHECK(trackFiles(42, 2).empty()); // track 2's folder is as it was: empty

        const std::string text = commands::readMemory(volume);
        const std::string body = rc0::slotBody(text, 42);
        const auto expected = params::computeSlotParams(kFrames);
        CHECK_EQ(trackField(body, 1, "WavStat"), 1);
        CHECK_EQ(trackField(body, 1, "WavLen"), kFrames);
        CHECK_EQ(trackField(body, 1, "MeasLen"), expected.measures);
        CHECK_EQ(trackField(body, 1, "Measure"), expected.measureField());
        CHECK_EQ(trackField(body, 1, "RecTmp"), expected.tempoTenths);
        CHECK_EQ(rc0::sectionField(body, rc0::kSectionMaster, "Tempo"), expected.tempoTenths);
        CHECK_EQ(rc0::sectionField(body, rc0::kSectionMaster, "LpLen"), expected.measures);
        CHECK_EQ(rc0::decodeName(body), "Pushed      ");
        // Track 2's section and the MASTER fields the pedal keeps for itself
        // are byte for byte what the factory left.
        for (const char* tag : { "WavStat", "WavLen", "MeasLen", "Measure", "RecTmp" })
            CHECK_EQ(trackField(body, 2, tag), trackField(bodyBefore, 2, tag));
        for (const char* tag : { "CurrentTrack", "LpMod" })
            CHECK_EQ(rc0::sectionField(body, rc0::kSectionMaster, tag),
                     rc0::sectionField(bodyBefore, rc0::kSectionMaster, tag));
        CHECK_EQ(rc0::sectionField(body, rc0::kSectionMaster, "CurrentTrack"), 0); // track 1, zero-based

        // Both banks carry it, and the catalog reads a memory recorded on
        // track 1 - which is a used memory now.
        CHECK(rc0::slotBody(commands::readMemory(volume, 1), 42)
              == rc0::slotBody(commands::readMemory(volume, 2), 42));
        const auto memory = catalog::listSlots(text).at(41);
        CHECK(memory.tracks.at(0).hasAudio && !memory.tracks.at(1).hasAudio);
        CHECK(catalog::holdsAudio(memory) && !catalog::uploadOffered(memory));
    }

    // --- a used memory is refused, naming the track in the way; nothing moves ---

    {
        const auto untouched = volumeBytes(volume);
        CHECK_THROWS(commands::push(volume, source, 1, { .write = writeOpts(tmp.path, "op-2") }),
                     "memory 1 holds a take on track 1");
        CHECK_THROWS(commands::push(volume, source, 1, { .write = writeOpts(tmp.path, "op-2") }),
                     "upload into a used \"RC-500\" memory is not offered yet");
        // Force is the one-track model's replace; here it changes nothing.
        CHECK_THROWS(commands::push(volume, source, 1,
                                    { .force = true, .write = writeOpts(tmp.path, "op-2") }),
                     "not offered yet");
        // Track 2 alone, the flat fields saying "empty": the sentence names
        // the second track.
        CHECK_THROWS(commands::push(volume, source, 11, { .write = writeOpts(tmp.path, "op-2") }),
                     "memory 11 holds a take on track 2");
        // The memory just pushed is used now, like one the pedal recorded.
        CHECK_THROWS(commands::push(volume, source, 42,
                                    { .force = true, .write = writeOpts(tmp.path, "op-2") }),
                     "memory 42 holds a take on track 1");
        CHECK(volumeBytes(volume) == untouched);
        CHECK(!fs::exists(tmp.path / "archive" / "op-2"));
    }

    // --- a take in the folder alone is a used memory too ---

    {
        // Memory 7's body says empty; a file sits in its track 2 folder (a
        // hand copy, or a take the pedal has not indexed). Not offered: what
        // the pedal would make of the pair is the same unknown.
        commands::writeFileBytes(volume::trackDir(volume, profile::kRc500, 7, 2) / "stray.wav",
                                 testkit::floatTake(4410, 9));
        const auto untouched = volumeBytes(volume);
        CHECK_THROWS(commands::push(volume, source, 7, { .write = writeOpts(tmp.path, "op-3") }),
                     "memory 7 holds a take on track 2");
        CHECK(volumeBytes(volume) == untouched);
    }

    // --- the drop-only path is ruled and gated like the configured one ---

    {
        // No config written, the same rule: a used memory takes no file.
        const auto untouched = volumeBytes(volume);
        CHECK_THROWS(commands::push(volume, source, 11,
                                    { .writeConfig = false, .write = writeOpts(tmp.path, "op-4") }),
                     "memory 11 holds a take on track 2");
        CHECK(volumeBytes(volume) == untouched);
    }
    {
        // A card the gate does not read - a model the table does not know -
        // refuses the drop-only push before the file lands: the memory is
        // read first on every path, so no model this app does not open for
        // push ever gets a file dropped into its folders.
        TempDir foreign;
        const fs::path card = testkit::makeTwoTrackCard(foreign.path, {});
        const std::string root = "<database name=\"RC-500\" revision=\"0\">";
        for (const int fileNo : { 1, 2 }) {
            std::string text = commands::readFileBytes(volume::memoryPath(card, fileNo));
            text.replace(text.find(root), root.size(), "<database name=\"RC-505\" revision=\"0\">");
            commands::writeFileBytes(volume::memoryPath(card, fileNo), text);
        }
        const fs::path foreignSource = writeSource(foreign.path, "take.wav");
        const auto untouched = volumeBytes(card);
        CHECK_THROWS(commands::push(card, foreignSource, 42,
                                    { .writeConfig = false, .write = writeOpts(foreign.path, "op-5") }),
                     "RC-505");
        CHECK(volumeBytes(card) == untouched);
        CHECK(volume::listTrackWavs(card, profile::kRc500, 42, 1).empty());
    }

    // --- the state before the push is a unit a restore takes back ---

    {
        const commands::WriteOptions restoring = writeOpts(tmp.path, "op-6");
        const auto result = commands::restore(volume, 42, before, restoring);
        CHECK_EQ(result.archived.size(), 1u); // the pushed take, kept first
        CHECK_EQ(result.frames.size(), 2u);
        if (result.frames.size() == 2)
            CHECK(result.frames.at(0) == 0 && result.frames.at(1) == 0);
        CHECK(trackFiles(42, 1).empty() && trackFiles(42, 2).empty());
        CHECK(rc0::slotBody(commands::readMemory(volume), 42) == before.body);
        const auto memory = catalog::listSlots(commands::readMemory(volume)).at(41);
        CHECK(!catalog::holdsAudio(memory) && catalog::uploadOffered(memory));
    }

    return testkit::summary("rc500_push_tests");
}
