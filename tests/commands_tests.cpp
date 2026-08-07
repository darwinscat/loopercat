// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The mutation commands against a real scratch volume, attacking the safety
// discipline from its theory:
//
//   - every write backs up first, writes BOTH memory files with their own
//     trailers, verifies by re-read, sweeps AppleDouble junk
//   - a failed push leaves the volume byte-identical (validate-then-write,
//     the full config document included — a field-broken slot pushes nothing)
//   - neither clear nor a forced push destroys audio without the trash copy
//     landing first; trim preserves the slot's tempo (QA-4)
//   - a write-phase failure never costs audio: the trash copy survives and
//     the memory pair stays untouched (fault injection)
//   - a sweep survivor is a warning on a SUCCESSFUL write, never a "failure"
//     that would roll a completed swap back
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

        // Both files carry the SAME document, stamped one generation apart
        // and PAST the factory pair 8/9 the volume started at — the write
        // continues the pedal's generation count, never rewinds it.
        const std::string m1 = commands::readFileBytes(volume::memoryPath(volume, 1));
        const std::string m2 = commands::readFileBytes(volume::memoryPath(volume, 2));
        CHECK_EQ(static_cast<int>(rc0::tailMarker(m1).value()), 0x3a);
        CHECK_EQ(static_cast<int>(rc0::tailMarker(m2).value()), 0x3b);
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
        CHECK(result.sweepFailed.empty());
        CHECK(volume::findJunk(volume).empty());

        // skipBackup without roots works; missing roots without it fail fast.
        commands::writeMemoryPair(volume, renamed, { .skipBackup = true });
        CHECK_THROWS(commands::writeMemoryPair(volume, renamed, {}), "backup requires");
    }

    // --- writeMemoryPair continues the pedal's generation count ---

    {
        // The exact state a pedal-side recording leaves behind (observed on
        // hardware 2026-07-24): the freshly saved bank one generation ahead.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const std::string text = commands::readMemory(volume);
        commands::writeFileBytes(volume::memoryPath(volume, 1),
                                 rc0::setTailGeneration(text, 0x3a));
        commands::writeFileBytes(volume::memoryPath(volume, 2),
                                 rc0::setTailGeneration(text, 0x39));

        commands::writeMemoryPair(volume, text, { .skipBackup = true });
        const auto m1 = rc0::tailMarker(commands::readFileBytes(volume::memoryPath(volume, 1)));
        const auto m2 = rc0::tailMarker(commands::readFileBytes(volume::memoryPath(volume, 2)));
        CHECK_EQ(static_cast<int>(m1.value()), 0x3b); // past the pedal's 0x3a...
        CHECK_EQ(static_cast<int>(m2.value()), 0x3c); // ...never rewound to 8/9

        // One unreadable bank cannot rewind the count either.
        fs::remove(volume::memoryPath(volume, 2));
        commands::writeMemoryPair(volume, text, { .skipBackup = true });
        const auto healed1 = rc0::tailMarker(commands::readFileBytes(volume::memoryPath(volume, 1)));
        const auto healed2 = rc0::tailMarker(commands::readFileBytes(volume::memoryPath(volume, 2)));
        CHECK_EQ(static_cast<int>(healed1.value()), 0x3c);
        CHECK_EQ(static_cast<int>(healed2.value()), 0x3d);
    }

    // --- setTempo: the user's BPM lands in every tempo-shaped field ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);

        // Give slot 5 indexed audio: exactly 60 s at 44.1 kHz.
        {
            std::string text = commands::readMemory(volume);
            std::string body = rc0::slotBody(text, 5);
            body = rc0::setField(body, "WavStat", 1);
            body = rc0::setField(body, "WavLen", 44100LL * 60);
            commands::writeMemoryPair(volume, rc0::replaceSlotBody(text, 5, body),
                                      { .skipBackup = true });
        }

        // 112.0 BPM over 60 s = 112 beats = 28 bars of 4/4; Measure carries
        // the hardware-verified +7 offset.
        commands::setTempo(volume, 5, 1120, { .skipBackup = true });
        const std::string after = commands::readMemory(volume);
        const std::string body = rc0::slotBody(after, 5);
        CHECK_EQ(rc0::field(body, "Tempo"), 1120);
        CHECK_EQ(rc0::field(body, "RecTmp"), 1120);
        CHECK_EQ(rc0::field(body, "MeasLen"), 28);
        CHECK_EQ(rc0::field(body, "Measure"), 35);
        CHECK_EQ(catalog::readSlot(after, 5).measures, 28);

        // A slot without indexed audio gets the tempo but keeps its measure
        // fields untouched — there is no duration to derive bars from.
        commands::setTempo(volume, 6, 905, { .skipBackup = true });
        const std::string empty = rc0::slotBody(commands::readMemory(volume), 6);
        CHECK_EQ(rc0::field(empty, "Tempo"), 905);
        CHECK_EQ(rc0::field(empty, "RecTmp"), 905);
        CHECK_EQ(rc0::field(empty, "MeasLen"), 0);
        CHECK_EQ(rc0::field(empty, "Measure"), 0);

        // Other slots stay byte-identical.
        const std::string untouched = rc0::slotBody(commands::readMemory(volume), 7);
        CHECK_EQ(untouched, rc0::slotBody(commands::readMemory(volume, 2), 7));

        // The pedal's range is a hard wall, not a clamp.
        CHECK_THROWS(commands::setTempo(volume, 5, 399, { .skipBackup = true }), "40.0-300.0");
        CHECK_THROWS(commands::setTempo(volume, 5, 3001, { .skipBackup = true }), "40.0-300.0");
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

        // Occupied slot: refused without force; force without a trash root is
        // refused too — the replaced take must have somewhere safe to go.
        CHECK_THROWS(commands::push(volume, source, 9, { .write = writeOpts(tmp.path, "stamp-2") }),
                     "already has audio");
        CHECK_THROWS(commands::push(volume, source, 9,
                                    { .force = true, .write = writeOpts(tmp.path, "stamp-2") }),
                     "trash root");
        CHECK_EQ(volume::listSlotWavs(volume, 9).size(), 1u);

        // Forced replace: the old take lands in the trash byte-identical —
        // push never deletes audio outright.
        const auto forced = commands::push(volume, source, 9,
                                           { .force = true, .trashRoot = tmp.path / "trash",
                                             .write = writeOpts(tmp.path, "stamp-3") });
        CHECK_EQ(volume::listSlotWavs(volume, 9).size(), 1u);
        CHECK(forced.configured);
        CHECK_EQ(forced.trashed.size(), 1u);
        CHECK(commands::readFileBytes(forced.trashed.front()) == pushed);
        CHECK(forced.trashed.front().string().find("stamp-3") != std::string::npos);
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

    // --- a field-broken slot pushes NOTHING (crew review #4) ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        // Break slot 5's <Tempo> opening tag in both banks — a structurally
        // valid document whose slot cannot be edited. The config document is
        // built and validated BEFORE the audio phase, so this must fail with
        // the volume untouched (it used to throw only after the wav landed).
        for (const int fileNo : { 1, 2 }) {
            const std::string text = commands::readFileBytes(volume::memoryPath(volume, fileNo));
            std::string body = rc0::slotBody(text, 5);
            body.replace(body.find("<Tempo>"), std::string("<Tempo>").size(), "<Tmpo->");
            commands::writeFileBytes(volume::memoryPath(volume, fileNo),
                                     rc0::replaceSlotBody(text, 5, body));
        }
        const auto before = volumeBytes(volume);

        const auto ok = testkit::syntheticWav({ .frames = 1323000 });
        const fs::path source = tmp.path / "ok.wav";
        commands::writeFileBytes(source, std::string_view(reinterpret_cast<const char*>(ok.data()),
                                                          ok.size()));
        CHECK_THROWS(commands::push(volume, source, 5, { .write = writeOpts(tmp.path) }),
                     "occurs 0 times");
        CHECK(volumeBytes(volume) == before);
        CHECK(!fs::exists(volume::wavDir(volume, 5)));
        CHECK(!fs::exists(tmp.path / "backups"));
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

        // Trim preserves the slot's tempo (QA-4): Tempo/RecTmp keep their
        // 120.0 BPM, and only the length fields follow the new duration —
        // 3000000 frames = 68.03 s = 136.05 beats at 120 BPM = 34 bars of
        // 4/4 (rounded), Measure carrying the +7 offset. The pedal's
        // power-of-two import formula would have written a different tempo —
        // asserted different, to pin the OLD bug.
        const std::string body = rc0::slotBody(commands::readMemory(volume), 4);
        CHECK_EQ(rc0::field(body, "WavLen"), 3000000);
        CHECK_EQ(rc0::field(body, "Tempo"), 1200);
        CHECK_EQ(rc0::field(body, "RecTmp"), 1200);
        CHECK_EQ(rc0::field(body, "MeasLen"), 34);
        CHECK_EQ(rc0::field(body, "Measure"), 41);
        CHECK_EQ(rc0::field(body, "LpLen"), 34);
        CHECK((result.slotParams == params::SlotParams { 34, 1200 }));
        CHECK(params::computeSlotParams(3000000).tempoTenths != 1200);
    }

    // --- trim after Set tempo: the user's BPM survives the cut (QA-4) ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 5, "take.wav", { .frames = 2646000 }); // 60 s
        {
            std::string text = commands::readMemory(volume);
            std::string body = rc0::slotBody(text, 5);
            body = rc0::setField(body, "WavStat", 1);
            body = rc0::setField(body, "WavLen", 2646000);
            commands::writeMemoryPair(volume, rc0::replaceSlotBody(text, 5, body),
                                      { .skipBackup = true });
        }
        commands::setTempo(volume, 5, 1120, { .skipBackup = true }); // the TRUE 112.0 BPM

        commands::trim(volume, 5, 0, 1323000,
                       { .trashRoot = tmp.path / "trash",
                         .write = { .stamp = "qa4", .skipBackup = true } });

        // 30 s at the KEPT 112.0 BPM = 56 beats = 14 bars. The hardware QA
        // run caught trim re-running the import formula here (16 bars at
        // 128.0 BPM for this length) — overwriting the tempo the user had
        // just corrected.
        const std::string body = rc0::slotBody(commands::readMemory(volume), 5);
        CHECK_EQ(rc0::field(body, "Tempo"), 1120);
        CHECK_EQ(rc0::field(body, "RecTmp"), 1120);
        CHECK_EQ(rc0::field(body, "WavLen"), 1323000);
        CHECK_EQ(rc0::field(body, "MeasLen"), 14);
        CHECK_EQ(rc0::field(body, "Measure"), 21);
        CHECK_EQ(rc0::field(body, "LpLen"), 14);
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
        CHECK_THROWS(commands::trim(volume, 5, 0, 1000, options), "no audio to trim");
        commands::TrimOptions noTrash { .write = writeOpts(tmp.path, "trim-3") };
        CHECK_THROWS(commands::trim(volume, 4, 0, 1323000, noTrash), "trash root");

        CHECK(volumeBytes(volume) == before);
        CHECK(!fs::exists(tmp.path / "trash"));

        // A slot whose config carries a nonsense tempo is refused before any
        // write — deriving a bar count from garbage would cement it.
        {
            const std::string text = commands::readMemory(volume);
            const std::string broken = rc0::setField(rc0::slotBody(text, 4), "Tempo", 9999);
            commands::writeMemoryPair(volume, rc0::replaceSlotBody(text, 4, broken),
                                      { .skipBackup = true });
        }
        const auto corrupted = volumeBytes(volume);
        CHECK_THROWS(commands::trim(volume, 4, 0, 1323000, options), "broken config");
        CHECK(volumeBytes(volume) == corrupted);
        CHECK(!fs::exists(tmp.path / "trash"));
    }

    // --- swap: two memories trade places wholesale ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 3, "003_1.WAV");                          // pedal-recorded shape
        putWav(volume, 7, "nice-take.wav", { .frames = 8820 }); // app-pushed shape
        {
            // Distinct configs so every travelling field is observable.
            std::string text = commands::readMemory(volume);
            std::string three = rc0::slotBody(text, 3);
            three = rc0::setField(three, "WavStat", 1);
            three = rc0::setField(three, "WavLen", 4410);
            three = rc0::setField(three, "One", 1);
            three = rc0::setName(three, "Part A");
            text = rc0::replaceSlotBody(text, 3, three);
            std::string seven = rc0::slotBody(text, 7);
            seven = rc0::setField(seven, "WavStat", 1);
            seven = rc0::setField(seven, "WavLen", 8820);
            seven = rc0::setName(seven, "Part B");
            text = rc0::replaceSlotBody(text, 7, seven);
            commands::writeMemoryPair(volume, text, { .skipBackup = true });
        }
        const std::string before = commands::readMemory(volume);
        const std::string wav3 = commands::readFileBytes(volume::wavDir(volume, 3) / "003_1.WAV");
        const std::string wav7 =
            commands::readFileBytes(volume::wavDir(volume, 7) / "nice-take.wav");

        commands::swap(volume, 3, 7, writeOpts(tmp.path));

        // The whole bodies traded places; every other slot is byte-identical.
        const std::string after = commands::readMemory(volume);
        CHECK(rc0::slotBody(after, 3) == rc0::slotBody(before, 7));
        CHECK(rc0::slotBody(after, 7) == rc0::slotBody(before, 3));
        CHECK_EQ(rc0::decodeName(rc0::slotBody(after, 3)), "Part B      ");
        CHECK_EQ(rc0::field(rc0::slotBody(after, 7), "One"), 1);
        int changedOtherSlots = 0;
        for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
            if (slot != 3 && slot != 7 && rc0::slotBody(after, slot) != rc0::slotBody(before, slot))
                ++changedOtherSlots;
        CHECK_EQ(changedOtherSlots, 0);

        // The audio traded addresses byte-identically. The pedal-recorded
        // technical name follows its new folder; the human name travels as is.
        CHECK(commands::readFileBytes(volume::wavDir(volume, 3) / "nice-take.wav") == wav7);
        CHECK(commands::readFileBytes(volume::wavDir(volume, 7) / "007_1.WAV") == wav3);
        CHECK(!fs::exists(volume / "ROLAND" / "WAVE" / commands::kSwapParkName));
        CHECK(commands::doctor(volume).empty()); // config and audio agree everywhere

        // Swapping back restores the document and the audio exactly (only the
        // write generations keep counting).
        commands::swap(volume, 3, 7, writeOpts(tmp.path, "stamp-2"));
        CHECK(rc0::splitFile(commands::readMemory(volume)).document
              == rc0::splitFile(before).document);
        CHECK(commands::readFileBytes(volume::wavDir(volume, 3) / "003_1.WAV") == wav3);
        CHECK(commands::readFileBytes(volume::wavDir(volume, 7) / "nice-take.wav") == wav7);
    }

    // --- swap with an empty slot degenerates into a move ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 12, "012_1.WAV");
        {
            std::string text = commands::readMemory(volume);
            std::string body = rc0::slotBody(text, 12);
            body = rc0::setField(body, "WavStat", 1);
            body = rc0::setField(body, "WavLen", 4410);
            commands::writeMemoryPair(volume, rc0::replaceSlotBody(text, 12, body),
                                      { .skipBackup = true });
        }
        const std::string before = commands::readMemory(volume);

        commands::swap(volume, 12, 15, writeOpts(tmp.path));
        CHECK(!fs::exists(volume::wavDir(volume, 12)));
        CHECK(volume::listSlotWavs(volume, 15) == std::vector<std::string> { "015_1.WAV" });
        const std::string after = commands::readMemory(volume);
        CHECK_EQ(rc0::field(rc0::slotBody(after, 15), "WavStat"), 1);
        CHECK_EQ(rc0::field(rc0::slotBody(after, 12), "WavStat"), 0);
        CHECK(rc0::slotBody(after, 12) == rc0::slotBody(before, 15));
        CHECK(commands::doctor(volume).empty());

        // And back: the other direction of the move.
        commands::swap(volume, 15, 12, writeOpts(tmp.path, "stamp-2"));
        CHECK(volume::listSlotWavs(volume, 12) == std::vector<std::string> { "012_1.WAV" });
        CHECK(!fs::exists(volume::wavDir(volume, 15)));
        CHECK(rc0::splitFile(commands::readMemory(volume)).document
              == rc0::splitFile(before).document);
    }

    // --- swap refusals: typed errors before anything moves ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const auto pristine = volumeBytes(volume);
        CHECK_THROWS(commands::swap(volume, 5, 5, writeOpts(tmp.path)), "different");
        CHECK_THROWS(commands::swap(volume, 0, 5, writeOpts(tmp.path)), "out of range");
        CHECK_THROWS(commands::swap(volume, 5, 100, writeOpts(tmp.path)), "out of range");
        CHECK(volumeBytes(volume) == pristine);

        // A leftover park folder from an interrupted swap blocks the next one
        // (occupied<->occupied needs the temp address) — and doctor points at it.
        putWav(volume, 2, "a.wav");
        putWav(volume, 4, "b.wav");
        fs::create_directories(volume / "ROLAND" / "WAVE" / commands::kSwapParkName);
        CHECK_THROWS(commands::swap(volume, 2, 4, writeOpts(tmp.path, "stamp-2")),
                     "interrupted swap");
        CHECK_EQ(volume::listSlotWavs(volume, 2).front(), "a.wav");
        CHECK_EQ(volume::listSlotWavs(volume, 4).front(), "b.wav");
        bool sawParked = false;
        for (const auto& finding : commands::doctor(volume))
            sawParked = sawParked
                || (finding.level == commands::Level::error
                    && finding.message.find(commands::kSwapParkName) != std::string::npos);
        CHECK(sawParked);
    }

    // --- a failed config write moves the audio back ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 3, "003_1.WAV");
        putWav(volume, 7, "take.wav", { .frames = 8820 });
        const auto before = volumeBytes(volume);

        // First bank unwritable: the write fails before any config byte lands;
        // the rollback must leave the volume byte-identical.
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_read,
                        fs::perm_options::replace);
        CHECK_THROWS(commands::swap(volume, 3, 7, { .skipBackup = true }), "cannot write");
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_all,
                        fs::perm_options::replace);
        CHECK(volumeBytes(volume) == before);

        // Second bank unwritable: MEMORY1 already carries the swapped config —
        // the audio still comes home, and the half-written pair is not silent:
        // doctor reports the divergence.
        fs::permissions(volume::memoryPath(volume, 2), fs::perms::owner_read,
                        fs::perm_options::replace);
        CHECK_THROWS(commands::swap(volume, 3, 7, { .skipBackup = true }), "cannot write");
        fs::permissions(volume::memoryPath(volume, 2), fs::perms::owner_all,
                        fs::perm_options::replace);
        CHECK_EQ(volume::listSlotWavs(volume, 3).front(), "003_1.WAV");
        CHECK_EQ(volume::listSlotWavs(volume, 7).front(), "take.wav");
        bool sawDiverged = false;
        for (const auto& finding : commands::doctor(volume))
            sawDiverged = sawDiverged || finding.message.find("differ") != std::string::npos;
        CHECK(sawDiverged);
    }

    // --- a locked sidecar cannot un-swap a successful swap (crew review #5) ---

    // POSIX-only: the injection removes write permission from the sidecar's
    // parent directory, which denies the delete. A Windows read-only
    // directory attribute does not deny child deletion, so there is no
    // equivalent lever there — the behavior under test is platform-free,
    // the injection mechanism is not.
#ifndef _WIN32
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 3, "003_1.WAV");
        putWav(volume, 7, "take.wav", { .frames = 8820 });
        const fs::path lockedDir = volume / "ROLAND" / "WAVE" / "LOCKED";
        fs::create_directories(lockedDir);
        commands::writeFileBytes(lockedDir / ".DS_Store", "junk");
        fs::permissions(lockedDir, fs::perms::owner_read | fs::perms::owner_exec,
                        fs::perm_options::replace);

        const std::string before = commands::readMemory(volume);
        const auto result = commands::swap(volume, 3, 7, { .skipBackup = true });
        fs::permissions(lockedDir, fs::perms::owner_all, fs::perm_options::replace);

        // The write SUCCEEDED and stays: config and audio both swapped. The
        // sidecar that would not delete is a warning in the result — treating
        // it as a write failure used to move the audio back over an already
        // swapped memory pair, silently diverging config from audio.
        const std::string after = commands::readMemory(volume);
        CHECK(rc0::slotBody(after, 3) == rc0::slotBody(before, 7));
        CHECK(rc0::slotBody(after, 7) == rc0::slotBody(before, 3));
        CHECK_EQ(volume::listSlotWavs(volume, 3).front(), "take.wav");
        CHECK_EQ(volume::listSlotWavs(volume, 7).front(), "007_1.WAV");
        CHECK_EQ(result.sweepFailed.size(), 1u);
        CHECK(fs::exists(lockedDir / ".DS_Store"));
    }
#endif // !_WIN32

    // --- write-phase fault injection: a failed write never costs audio ---
    // (crew review #14 — the trap the unreproduced QA-5 waits behind)

    // push --force with an unwritable MEMORY1: the audio phase completed,
    // the config write failed — the old take sits in the trash, the new one
    // in the slot, and the memory pair is byte-untouched. Nothing lost.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 9, "old.wav");
        const std::string oldBytes =
            commands::readFileBytes(volume::wavDir(volume, 9) / "old.wav");
        const std::string m1 = commands::readFileBytes(volume::memoryPath(volume, 1));
        const std::string m2 = commands::readFileBytes(volume::memoryPath(volume, 2));

        const auto wavBytes = testkit::syntheticWav({ .frames = 1323000 });
        const fs::path source = tmp.path / "new.wav";
        commands::writeFileBytes(source,
                                 std::string_view(reinterpret_cast<const char*>(wavBytes.data()),
                                                  wavBytes.size()));

        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_read,
                        fs::perm_options::replace);
        CHECK_THROWS(commands::push(volume, source, 9,
                                    { .force = true, .trashRoot = tmp.path / "trash",
                                      .write = { .stamp = "fi-push", .skipBackup = true } }),
                     "cannot write");
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_all,
                        fs::perm_options::replace);

        CHECK(commands::readFileBytes(tmp.path / "trash" / "fi-push" / "009_1" / "old.wav")
              == oldBytes);
        CHECK_EQ(volume::listSlotWavs(volume, 9).front(), "new.wav");
        CHECK(commands::readFileBytes(volume::memoryPath(volume, 1)) == m1);
        CHECK(commands::readFileBytes(volume::memoryPath(volume, 2)) == m2);
    }

    // trim with an unwritable MEMORY1: the original is already safe in the
    // trash and the slot holds the slice — recoverable, honestly reported.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 4, "take.wav", { .frames = 1323000 });
        const std::string original =
            commands::readFileBytes(volume::wavDir(volume, 4) / "take.wav");
        const std::string m1 = commands::readFileBytes(volume::memoryPath(volume, 1));

        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_read,
                        fs::perm_options::replace);
        CHECK_THROWS(commands::trim(volume, 4, 0, 661500,
                                    { .trashRoot = tmp.path / "trash",
                                      .write = { .stamp = "fi-trim", .skipBackup = true } }),
                     "cannot write");
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_all,
                        fs::perm_options::replace);

        CHECK(commands::readFileBytes(tmp.path / "trash" / "fi-trim" / "004_1" / "take.wav")
              == original);
        CHECK(commands::readFileBytes(volume::memoryPath(volume, 1)) == m1);
    }

    // clear with an unwritable MEMORY1: the audio left the slot but its
    // trash copy landed first — the take survives the failed command.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 6, "gone.wav");
        const std::string original =
            commands::readFileBytes(volume::wavDir(volume, 6) / "gone.wav");

        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_read,
                        fs::perm_options::replace);
        CHECK_THROWS(commands::clear(volume, { 6 },
                                     { .trashRoot = tmp.path / "trash",
                                       .write = { .stamp = "fi-clear", .skipBackup = true } }),
                     "cannot write");
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_all,
                        fs::perm_options::replace);

        CHECK(commands::readFileBytes(tmp.path / "trash" / "fi-clear" / "006_1" / "gone.wav")
              == original);
        CHECK(volume::listSlotWavs(volume, 6).empty());
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
        CHECK_EQ(byLevel[commands::Level::warn], 1);  // configured-but-empty
        CHECK_EQ(byLevel[commands::Level::info], 2);  // unindexed wav + pair divergence
        // Divergence is a fact of life after a pedal-side save, not damage.
        bool sawDivergence = false;
        for (const auto& finding : findings)
            sawDivergence = sawDivergence
                || (finding.level == commands::Level::info
                    && finding.message.find("normal right after a save") != std::string::npos);
        CHECK(sawDivergence);

        // A pedal-recorded generation pair (e.g. 0x3a/0x39, hardware-observed
        // 2026-07-24) is healthy — trailer values are counters, not constants.
        {
            const std::string text = commands::readMemory(volume, 1);
            commands::writeFileBytes(volume::memoryPath(volume, 1),
                                     rc0::setTailGeneration(text, 0x3a));
            commands::writeFileBytes(volume::memoryPath(volume, 2),
                                     rc0::setTailGeneration(text, 0x39));
        }
        for (const auto& finding : commands::doctor(volume))
            CHECK(finding.message.find("trailer") == std::string::npos
                  && finding.message.find("generations") == std::string::npos);

        // Generations more than one step apart are flagged as unexpected.
        {
            const std::string text = commands::readMemory(volume, 1);
            commands::writeFileBytes(volume::memoryPath(volume, 2),
                                     rc0::setTailGeneration(text, 0x35));
        }
        bool sawGap = false;
        for (const auto& finding : commands::doctor(volume))
            sawGap = sawGap
                || (finding.level == commands::Level::warn
                    && finding.message.find("more than one step apart") != std::string::npos);
        CHECK(sawGap);

        // A structurally broken trailer is the boot-fatal condition.
        {
            const std::string m2 = commands::readFileBytes(volume::memoryPath(volume, 2));
            commands::writeFileBytes(volume::memoryPath(volume, 2),
                                     m2.substr(0, m2.size() - 4) + std::string("\x39\x01\0\0", 4));
        }
        bool sawMalformed = false;
        for (const auto& finding : commands::doctor(volume))
            sawMalformed = sawMalformed
                || (finding.level == commands::Level::error
                    && finding.message.find("malformed") != std::string::npos);
        CHECK(sawMalformed);
    }

    // --- family guard at the door (issue #35): a foreign card refuses to open ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        // Re-flag the pair as an RC-500 card: identical structure, foreign
        // root name — the header shape from the boss-rc500-editor template
        // quoted in issue #35. This is exactly what a healthy RC-500 card
        // looks like to a byte parser.
        const std::string rc5Header = "<database name=\"RC-5\" revision=\"0\">";
        for (const int fileNo : { 1, 2 }) {
            std::string text = commands::readFileBytes(volume::memoryPath(volume, fileNo));
            text.replace(text.find(rc5Header), rc5Header.size(),
                         "<database name=\"RC-500\" revision=\"0\">");
            commands::writeFileBytes(volume::memoryPath(volume, fileNo), text);
        }
        const auto before = volumeBytes(volume);

        // Reading refuses by name — the honest message, not "broken card".
        CHECK_THROWS(commands::readMemory(volume), "RC-500");

        // A mutation refuses BEFORE anything is written: every byte on the
        // volume identical, and not even a backup directory appeared.
        CHECK_THROWS(commands::rename(volume, 1, "Hijack", writeOpts(tmp.path)), "RC-500");
        CHECK_THROWS(commands::setTempo(volume, 1, 1200, writeOpts(tmp.path)), "RC-500");
        CHECK_THROWS(commands::swap(volume, 1, 2, writeOpts(tmp.path)), "RC-500");
        CHECK(volumeBytes(volume) == before);
        CHECK(!fs::exists(tmp.path / "backups"));

        // The doctor names the family too, instead of diagnosing damage.
        bool named = false;
        for (const auto& finding : commands::doctor(volume))
            named = named
                || (finding.level == commands::Level::error
                    && finding.message.find("RC-500") != std::string::npos);
        CHECK(named);
    }

    return testkit::summary("commands");
}
