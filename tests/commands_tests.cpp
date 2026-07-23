// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The mutation commands against a real scratch volume, attacking the safety
// discipline from its theory:
//
//   - every write backs up first, writes BOTH memory files with their own
//     trailers, verifies by re-read, sweeps AppleDouble junk
//   - a failed push leaves the volume byte-identical (validate-then-write)
//   - clear never destroys audio without the trash copy landing first
//   - rename touches nothing but the name; the byte-invariant holds on disk
//   - pull renames technical names, disambiguates duplicates, refuses to
//     overwrite without force
//   - doctor reports junk, trailer damage, pair divergence, config/audio
//     disagreements

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
        path = fs::temp_directory_path() / ("loopercat-cmd-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

// A scratch pedal volume: synthetic memory pair + optional slot audio.
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

void putWav(const fs::path& volume, int slot, const std::string& name,
            const testkit::WavSpec& spec = { .frames = 4410 })
{
    const auto bytes = testkit::syntheticWav(spec);
    fs::create_directories(volume::wavDir(volume, slot));
    commands::writeFileBytes(volume::wavDir(volume, slot) / name,
                             std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                              bytes.size()));
}

// Byte-map of the whole volume, for exact before/after comparisons.
std::map<std::string, std::string> volumeBytes(const fs::path& volume)
{
    std::map<std::string, std::string> map;
    for (fs::recursive_directory_iterator it(volume), end; it != end; ++it)
        if (!it->is_directory())
            map[fs::relative(it->path(), volume).string()] = commands::readFileBytes(it->path());
    return map;
}

commands::WriteOptions writeOpts(const fs::path& root, const std::string& stamp = "stamp-1")
{
    return { .backupRoot = root / "backups", .stamp = stamp };
}

} // namespace

int main()
{
    // --- writeMemoryPair: the whole discipline in one call ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        commands::writeFileBytes(volume / "ROLAND" / "WAVE" / ".DS_Store", "junk");
        commands::writeFileBytes(volume::dataDir(volume) / "._MEMORY1.RC0", "sidecar");

        const std::string original = commands::readMemory(volume);
        const std::string renamed = rc0::replaceSlotBody(
            original, 7, rc0::setName(rc0::slotBody(original, 7), "New Name"));
        const auto result = commands::writeMemoryPair(volume, renamed, writeOpts(tmp.path));

        // Both files carry the SAME document with their OWN trailers.
        const std::string m1 = commands::readFileBytes(volume::memoryPath(volume, 1));
        const std::string m2 = commands::readFileBytes(volume::memoryPath(volume, 2));
        CHECK_EQ(static_cast<int>(rc0::tailMarker(m1).value()), 0x38);
        CHECK_EQ(static_cast<int>(rc0::tailMarker(m2).value()), 0x39);
        CHECK_EQ(rc0::splitFile(m1).document, rc0::splitFile(m2).document);

        // The backup holds the PRE-write bytes of both memory files.
        CHECK(result.backedUp.has_value());
        if (result.backedUp) {
            CHECK_EQ(result.backedUp->copied.size(), 2u);
            CHECK(commands::readFileBytes(result.backedUp->dest / "MEMORY1.RC0")
                  == rc0::setTailMarker(original, 1));
        }

        // Junk is gone — the sidecar next to the memory files included.
        CHECK_EQ(result.swept.size(), 2u);
        CHECK(volume::findJunk(volume).empty());

        // skipBackup without roots works; missing roots without it fail fast.
        commands::writeMemoryPair(volume, renamed, { .skipBackup = true });
        CHECK_THROWS(commands::writeMemoryPair(volume, renamed, {}), "backup requires");
    }

    // --- rename: the byte-invariant holds on disk ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const std::string before = commands::readMemory(volume);
        commands::rename(volume, 42, "Cold Gaze", writeOpts(tmp.path));
        const std::string after = commands::readMemory(volume);
        CHECK_EQ(rc0::decodeName(rc0::slotBody(after, 42)), "Cold Gaze   ");
        int changedOtherSlots = 0;
        for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
            if (slot != 42 && rc0::slotBody(after, slot) != rc0::slotBody(before, slot))
                ++changedOtherSlots;
        CHECK_EQ(changedOtherSlots, 0);

        CHECK_THROWS(commands::rename(volume, 42, "\xd0\x9a\xd0\x9e\xd0\xa2", writeOpts(tmp.path)),
                     "ASCII");
        CHECK_THROWS(commands::rename(volume, 0, "X", writeOpts(tmp.path)), "out of range");
    }

    // --- setOneShot across multiple slots ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        commands::setOneShot(volume, { 3, 5 }, true, writeOpts(tmp.path));
        const std::string text = commands::readMemory(volume);
        CHECK_EQ(rc0::field(rc0::slotBody(text, 3), "One"), 1);
        CHECK_EQ(rc0::field(rc0::slotBody(text, 5), "One"), 1);
        CHECK_EQ(rc0::field(rc0::slotBody(text, 4), "One"), 0);
    }

    // --- push: validate-then-write, canonical bytes, full config ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);

        // A wav long enough for a real tempo: 6860867 frames is golden
        // (64 measures, 98.7 BPM) — but huge; use 1323000 frames = 30 s
        // -> 8 measures, 96.0 BPM by the formula. Compute, don't copy.
        const int frames = 1323000;
        const auto wavBytes = testkit::syntheticWav({ .frames = frames, .extraChunk = true });
        const fs::path source = tmp.path / "My Song.wav";
        commands::writeFileBytes(source, std::string_view(reinterpret_cast<const char*>(wavBytes.data()),
                                                          wavBytes.size()));

        const auto result = commands::push(volume, source, 9,
                                           { .name = "My Song", .oneShot = true,
                                             .write = writeOpts(tmp.path) });
        CHECK(result.configured);
        CHECK_EQ(result.dest.filename().string(), "My Song.wav");

        // On-volume bytes are CANONICAL (metadata stripped), not the source copy.
        const std::string pushed = commands::readFileBytes(result.dest);
        CHECK_EQ(pushed.size(), 44u + static_cast<std::size_t>(frames) * 4);

        const std::string text = commands::readMemory(volume);
        const std::string body = rc0::slotBody(text, 9);
        const auto expected = params::computeSlotParams(frames);
        CHECK_EQ(rc0::field(body, "WavStat"), 1);
        CHECK_EQ(rc0::field(body, "WavLen"), frames);
        CHECK_EQ(rc0::field(body, "MeasLen"), expected.measures);
        CHECK_EQ(rc0::field(body, "Measure"), expected.measureField());
        CHECK_EQ(rc0::field(body, "RecTmp"), expected.tempoTenths);
        CHECK_EQ(rc0::field(body, "Tempo"), expected.tempoTenths);
        CHECK_EQ(rc0::field(body, "LpLen"), expected.measures);
        CHECK_EQ(rc0::field(body, "One"), 1);
        CHECK_EQ(rc0::decodeName(body), "My Song     ");

        // Occupied slot: refused without force, replaced with it.
        CHECK_THROWS(commands::push(volume, source, 9, { .write = writeOpts(tmp.path, "stamp-2") }),
                     "already has audio");
        const auto forced = commands::push(volume, source, 9,
                                           { .force = true, .write = writeOpts(tmp.path, "stamp-3") });
        CHECK_EQ(volume::listSlotWavs(volume, 9).size(), 1u);
        CHECK(forced.configured);
    }

    // --- push failure leaves the volume byte-identical ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const auto before = volumeBytes(volume);

        // Mono is not uploadable — must be rejected BEFORE any write.
        const auto mono = testkit::syntheticWav({ .channels = 1, .frames = 1323000 });
        const fs::path source = tmp.path / "mono.wav";
        commands::writeFileBytes(source, std::string_view(reinterpret_cast<const char*>(mono.data()),
                                                          mono.size()));
        CHECK_THROWS(commands::push(volume, source, 5, { .write = writeOpts(tmp.path) }), "stereo");

        // Too short for the tempo range: also rejected pre-write.
        const auto tiny = testkit::syntheticWav({ .frames = 1000 });
        const fs::path tinySource = tmp.path / "tiny.wav";
        commands::writeFileBytes(tinySource, std::string_view(reinterpret_cast<const char*>(tiny.data()),
                                                              tiny.size()));
        CHECK_THROWS(commands::push(volume, tinySource, 5, { .write = writeOpts(tmp.path) }),
                     "too short");

        // A bad slot name: rejected before the audio lands too.
        const auto ok = testkit::syntheticWav({ .frames = 1323000 });
        const fs::path okSource = tmp.path / "ok.wav";
        commands::writeFileBytes(okSource, std::string_view(reinterpret_cast<const char*>(ok.data()),
                                                            ok.size()));
        CHECK_THROWS(commands::push(volume, okSource, 5,
                                    { .name = "ThirteenChars", .write = writeOpts(tmp.path) }),
                     "longer than 12");

        CHECK(volumeBytes(volume) == before);
    }

    // --- pull: smart naming, duplicates, overwrite protection ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 3, "TRACK~1.WAV");    // technical -> renamed from the slot
        putWav(volume, 7, "nice-take.wav");  // human -> kept
        putWav(volume, 8, "nice-take.wav");  // duplicate of slot 7's name

        {
            const std::string text = commands::readMemory(volume);
            const std::string renamed = rc0::replaceSlotBody(
                text, 3, rc0::setName(rc0::slotBody(text, 3), "Deep Space 1"));
            commands::writeMemoryPair(volume, renamed, writeOpts(tmp.path));
        }

        const fs::path dest = tmp.path / "out";
        const auto jobs = commands::pull(volume, { 3, 7, 8 }, { .dest = dest });
        CHECK_EQ(jobs.size(), 3u);
        CHECK_EQ(jobs.at(0).base, "03 - Deep Space 1.wav");
        CHECK_EQ(jobs.at(1).base, "07 - nice-take.wav"); // duplicate -> slot-prefixed
        CHECK_EQ(jobs.at(2).base, "08 - nice-take.wav");
        CHECK(fs::exists(dest / "03 - Deep Space 1.wav"));
        CHECK(commands::readFileBytes(jobs.at(1).destFile)
              == commands::readFileBytes(jobs.at(1).src));

        CHECK_THROWS(commands::pull(volume, { 3 }, { .dest = dest }), "already exists");
        commands::pull(volume, { 3 }, { .dest = dest, .force = true }); // no throw
        CHECK_THROWS(commands::pull(volume, { 4 }, { .dest = dest }), "no audio to pull");
        CHECK_THROWS(commands::pull(volume, { 3 }, {}), "destination");
    }

    // --- clear: factory state, trash safety net ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 5, "gone.wav");
        const std::string wavBytesBefore =
            commands::readFileBytes(volume::wavDir(volume, 5) / "gone.wav");

        commands::ClearOptions options { .trashRoot = tmp.path / "trash",
                                         .write = writeOpts(tmp.path) };
        const auto result = commands::clear(volume, { 5 }, options);

        // The audio is out of the slot but safe in the trash, byte-identical.
        CHECK(volume::listSlotWavs(volume, 5).empty());
        CHECK_EQ(result.trashed.size(), 1u);
        CHECK(commands::readFileBytes(result.trashed.front()) == wavBytesBefore);

        // The slot body is EXACTLY the factory one (byte-level, not field-level).
        const std::string text = commands::readMemory(volume);
        CHECK(rc0::slotBody(text, 5) == rc0::factorySlotBody(5));

        // keepName: factory values, surviving name.
        commands::rename(volume, 6, "Keep Me", writeOpts(tmp.path, "stamp-2"));
        commands::ClearOptions keep { .keepName = true, .trashRoot = tmp.path / "trash",
                                      .write = writeOpts(tmp.path, "stamp-3") };
        commands::clear(volume, { 6 }, keep);
        const std::string after = commands::readMemory(volume);
        CHECK_EQ(rc0::decodeName(rc0::slotBody(after, 6)), "Keep Me     ");
        CHECK_EQ(rc0::field(rc0::slotBody(after, 6), "Measure"), 1); // factory value

        // No trash root -> fail fast before touching anything.
        putWav(volume, 8, "safe.wav");
        commands::ClearOptions bad { .write = writeOpts(tmp.path, "stamp-4") };
        CHECK_THROWS(commands::clear(volume, { 8 }, bad), "trash root");
        CHECK_EQ(volume::listSlotWavs(volume, 8).size(), 1u);
    }

    // --- trim: canonical slice in place, original in trash, config recomputed ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);

        // A 2-minute silent wav with marker bytes at known frames: frame F's
        // first sample byte = 0xAB proves the slice offset end-to-end.
        const int frames = 5292000;
        auto bytes = testkit::syntheticWav({ .frames = frames });
        const auto frameByte = [&](int frame) { return 44 + static_cast<std::size_t>(frame) * 4; };
        bytes[frameByte(1000000)] = 0xab;     // inside the kept range -> lands at new frame 0
        bytes[frameByte(3999999)] = 0xcd;     // the last kept frame
        bytes[frameByte(4000001)] = 0xef;     // outside -> must vanish
        fs::create_directories(volume::wavDir(volume, 4));
        commands::writeFileBytes(volume::wavDir(volume, 4) / "take.wav",
                                 std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                  bytes.size()));

        const std::string originalBytes =
            commands::readFileBytes(volume::wavDir(volume, 4) / "take.wav");

        commands::TrimOptions options { .trashRoot = tmp.path / "trash",
                                        .write = writeOpts(tmp.path, "trim-1") };
        const auto result = commands::trim(volume, 4, 1000000, 4000000, options);

        // Same filename, canonical header, exactly the requested 3M frames.
        const std::string after = commands::readFileBytes(volume::wavDir(volume, 4) / "take.wav");
        CHECK_EQ(after.size(), 44u + 3000000u * 4);
        CHECK_EQ(static_cast<unsigned char>(after[44]), 0xab);                          // old frame 1000000
        CHECK_EQ(static_cast<unsigned char>(after[44 + 2999999u * 4]), 0xcd);           // old frame 3999999
        CHECK_EQ(result.frames, 3000000);

        // The original is in the trash, byte-identical — the undo.
        CHECK(commands::readFileBytes(result.trashedOriginal) == originalBytes);

        // The slot's boot-index config matches the formula for the new length.
        const auto expected = params::computeSlotParams(3000000);
        const std::string body = rc0::slotBody(commands::readMemory(volume), 4);
        CHECK_EQ(rc0::field(body, "WavLen"), 3000000);
        CHECK_EQ(rc0::field(body, "MeasLen"), expected.measures);
        CHECK_EQ(rc0::field(body, "Measure"), expected.measureField());
        CHECK_EQ(rc0::field(body, "Tempo"), expected.tempoTenths);
        CHECK(result.slotParams == expected);
    }

    // --- trim failures leave the volume byte-identical ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 4, "take.wav", { .frames = 1323000 });
        const auto before = volumeBytes(volume);
        const commands::TrimOptions options { .trashRoot = tmp.path / "trash",
                                              .write = writeOpts(tmp.path, "trim-2") };

        CHECK_THROWS(commands::trim(volume, 4, 500, 100, options), "bad frame range");
        CHECK_THROWS(commands::trim(volume, 4, 0, 1000, options), "too short");     // > 160 BPM
        CHECK_THROWS(commands::trim(volume, 5, 0, 1000, options), "no audio to trim");
        commands::TrimOptions noTrash { .write = writeOpts(tmp.path, "trim-3") };
        CHECK_THROWS(commands::trim(volume, 4, 0, 1323000, noTrash), "trash root");

        CHECK(volumeBytes(volume) == before);
        CHECK(!fs::exists(tmp.path / "trash"));
    }

    // --- doctor ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        CHECK(commands::doctor(volume).empty()); // a healthy pedal reports nothing

        // Junk + a slot configured with audio but an empty folder + an
        // unindexed wav + diverged memory pair.
        commands::writeFileBytes(volume / "ROLAND" / "WAVE" / ".DS_Store", "junk");
        putWav(volume, 7, "unindexed.wav");
        {
            std::string text = commands::readMemory(volume);
            std::string body = rc0::slotBody(text, 3);
            body = rc0::setField(body, "WavStat", 1);
            body = rc0::setField(body, "WavLen", 4410);
            commands::writeFileBytes(volume::memoryPath(volume, 1),
                                     rc0::setTailMarker(rc0::replaceSlotBody(text, 3, body), 1));
        }

        const auto findings = commands::doctor(volume);
        std::map<commands::Level, int> byLevel;
        for (const auto& finding : findings)
            ++byLevel[finding.level];
        CHECK_EQ(byLevel[commands::Level::error], 1); // the junk
        CHECK_EQ(byLevel[commands::Level::warn], 2);  // pair divergence + configured-but-empty
        CHECK_EQ(byLevel[commands::Level::info], 1);  // the unindexed wav

        // A wrong trailer marker is an error finding.
        {
            const std::string m2 = commands::readFileBytes(volume::memoryPath(volume, 2));
            commands::writeFileBytes(volume::memoryPath(volume, 2),
                                     m2.substr(0, m2.size() - 4) + std::string("\x37\0\0\0", 4));
        }
        bool sawTrailer = false;
        for (const auto& finding : commands::doctor(volume))
            sawTrailer = sawTrailer || finding.message.find("trailer marker") != std::string::npos;
        CHECK(sawTrailer);
    }

    return testkit::summary("commands");
}
