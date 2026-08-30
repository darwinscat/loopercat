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

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>

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

// float32 stereo geometry, used by the size arithmetic below: 8 bytes per
// frame; the synthetic source keeps a plain 16-byte fmt (44-byte header),
// while anything the commands write is canonical, and float32 canonical
// carries a 28-byte fmt body (56-byte header).
constexpr std::size_t kFrameBytes = 8;
constexpr std::size_t kSourceHeader = 44;
constexpr std::size_t kCanonicalHeader = 56;

// Slot audio as the PEDAL writes it: float32. Since issue #44 that is also
// the only shape push accepts without conversion.
void putWav(const fs::path& volume, int slot, const std::string& name,
            const testkit::WavSpec& spec = { .tag = 3, .bits = 32, .frames = 4410 })
{
    const auto bytes = testkit::syntheticWav(spec);
    fs::create_directories(volume::wavDir(volume, slot));
    commands::writeFileBytes(volume::wavDir(volume, slot) / name,
                             std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                              bytes.size()));
}

// A float32 stereo slot take whose channels DIFFER — the only thing a fold
// has anything to do with. Left ramps up, right ramps down, so a fold that
// dropped a channel or read the wrong one cannot land on the right answer by
// accident. Values are dyadic so the mean is exact.
void putStereoFloatWav(const fs::path& volume, int slot, const std::string& name, int frames)
{
    std::vector<unsigned char> b;
    const auto ascii = [&b](std::string_view t) {
        for (const char c : t)
            b.push_back(static_cast<unsigned char>(c));
    };
    const auto p16 = [&b](int v) {
        b.push_back(static_cast<unsigned char>(v & 0xff));
        b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    };
    const auto p32 = [&p16](int v) { p16(v & 0xffff); p16((v >> 16) & 0xffff); };
    const auto sample = [&b](float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (int shift = 0; shift < 32; shift += 8)
            b.push_back(static_cast<unsigned char>((bits >> shift) & 0xffu));
    };
    const int dataSize = frames * 8;
    ascii("RIFF"); p32(12 + 24 + 8 + dataSize - 8); ascii("WAVE");
    ascii("fmt "); p32(16);
    p16(3); p16(2); p32(wav::kSampleRate); p32(wav::kSampleRate * 8); p16(8); p16(32);
    ascii("data"); p32(dataSize);
    for (int frame = 0; frame < frames; ++frame) {
        const float step = static_cast<float>(frame % 8) / 8.0f; // 0, .125 .. .875
        sample(step);
        sample(-step);
    }
    fs::create_directories(volume::wavDir(volume, slot));
    commands::writeFileBytes(volume::wavDir(volume, slot) / name,
                             std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
}

// The same shape, but the two channels do NOT cancel: the placement tests need
// a fold that is audible, or "channel 2 carries the loop" would be true of
// silence and prove nothing.
void putUncancellingStereoFloatWav(const fs::path& volume, int slot, const std::string& name,
                                   int frames)
{
    std::vector<unsigned char> b;
    const auto ascii = [&b](std::string_view t) {
        for (const char c : t)
            b.push_back(static_cast<unsigned char>(c));
    };
    const auto p16 = [&b](int v) {
        b.push_back(static_cast<unsigned char>(v & 0xff));
        b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    };
    const auto p32 = [&p16](int v) { p16(v & 0xffff); p16((v >> 16) & 0xffff); };
    const auto sample = [&b](float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (int shift = 0; shift < 32; shift += 8)
            b.push_back(static_cast<unsigned char>((bits >> shift) & 0xffu));
    };
    const int dataSize = frames * 8;
    ascii("RIFF"); p32(12 + 24 + 8 + dataSize - 8); ascii("WAVE");
    ascii("fmt "); p32(16);
    p16(3); p16(2); p32(wav::kSampleRate); p32(wav::kSampleRate * 8); p16(8); p16(32);
    ascii("data"); p32(dataSize);
    for (int frame = 0; frame < frames; ++frame) {
        const float step = static_cast<float>(frame % 8) / 8.0f; // 0, .125 .. .875
        sample(step);
        sample(step * 0.5f); // mean = 0.75 * step — dyadic, and not silence
    }
    fs::create_directories(volume::wavDir(volume, slot));
    commands::writeFileBytes(volume::wavDir(volume, slot) / name,
                             std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
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

    // --- writeMemoryPair carries the count past a byte, never rewinds it ---

    {
        // A pedal that has saved its way to the top of the first byte. A
        // byte-wide continuation would wrap the fresh stamps back to
        // 0x00/0x01 — rewinding the count and desyncing the pair from the
        // pedal's own next save. The uint32 counter carries instead.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const std::string text = commands::readMemory(volume);
        commands::writeFileBytes(volume::memoryPath(volume, 1),
                                 rc0::setTailGeneration(text, 0xffu));
        commands::writeFileBytes(volume::memoryPath(volume, 2),
                                 rc0::setTailGeneration(text, 0xfeu));

        commands::writeMemoryPair(volume, text, { .skipBackup = true });
        const auto m1 = rc0::tailMarker(commands::readFileBytes(volume::memoryPath(volume, 1)));
        const auto m2 = rc0::tailMarker(commands::readFileBytes(volume::memoryPath(volume, 2)));
        CHECK_EQ(m1.value(), 0x100u);
        CHECK_EQ(m2.value(), 0x101u);
    }

    // --- readMemory picks the bank the write counters name as newest ---

    {
        // The pedal-side save state observed live 2026-08-10: the fresh WRITE
        // sits in ONE bank (generation 237) while the other is stale (236).
        // Reading always-MEMORY1 called the just-saved loop absent.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const std::string stale = commands::readMemory(volume, 1);
        std::string fresh = rc0::replaceSlotBody(
            stale, 22, rc0::setName(rc0::slotBody(stale, 22), "Fresh Save"));

        commands::writeFileBytes(volume::memoryPath(volume, 1),
                                 rc0::setTailGeneration(stale, 236));
        commands::writeFileBytes(volume::memoryPath(volume, 2),
                                 rc0::setTailGeneration(fresh, 237));
        CHECK_EQ(rc0::decodeName(rc0::slotBody(commands::readMemory(volume), 22)),
                 "Fresh Save  ");

        // The other order too — the banks ping-pong save by save.
        commands::writeFileBytes(volume::memoryPath(volume, 1),
                                 rc0::setTailGeneration(fresh, 238));
        commands::writeFileBytes(volume::memoryPath(volume, 2),
                                 rc0::setTailGeneration(stale, 237));
        CHECK_EQ(rc0::decodeName(rc0::slotBody(commands::readMemory(volume), 22)),
                 "Fresh Save  ");

        // Serial arithmetic across the counter wrap: 0x00000000 is one past
        // 0xffffffff, not four billion behind it.
        commands::writeFileBytes(volume::memoryPath(volume, 1),
                                 rc0::setTailGeneration(stale, 0xffffffffu));
        commands::writeFileBytes(volume::memoryPath(volume, 2),
                                 rc0::setTailGeneration(fresh, 0x00000000u));
        CHECK_EQ(rc0::decodeName(rc0::slotBody(commands::readMemory(volume), 22)),
                 "Fresh Save  ");

        // A bank with a broken trailer loses the vote to a counted one — the
        // stale-but-counted MEMORY1 wins over the fresher trailer-less M2.
        const std::string m2 = commands::readFileBytes(volume::memoryPath(volume, 2));
        commands::writeFileBytes(volume::memoryPath(volume, 2), m2.substr(0, m2.size() - 2));
        CHECK_EQ(rc0::decodeName(rc0::slotBody(commands::readMemory(volume), 22)),
                 "Memory 22   ");

        // One bank gone entirely: the survivor answers.
        fs::remove(volume::memoryPath(volume, 1));
        commands::writeFileBytes(volume::memoryPath(volume, 2),
                                 rc0::setTailGeneration(fresh, 240));
        CHECK_EQ(rc0::decodeName(rc0::slotBody(commands::readMemory(volume), 22)),
                 "Fresh Save  ");

        // The pinned form still pins.
        CHECK_THROWS(commands::readMemory(volume, 1), "");
    }

    // --- rename works on a card whose counters sit far past one byte ---

    {
        // The user-facing path of the same theory: a fw 1.10 field pedal
        // (MEMORY pair 0x3e65736e/0x3e65736f) must rename, not refuse.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const std::string text = commands::readMemory(volume);
        commands::writeFileBytes(volume::memoryPath(volume, 1),
                                 rc0::setTailGeneration(text, 0x3e65736fu));
        commands::writeFileBytes(volume::memoryPath(volume, 2),
                                 rc0::setTailGeneration(text, 0x3e65736eu));

        commands::rename(volume, 2, "Field Test", { .skipBackup = true });
        const std::string m1 = commands::readFileBytes(volume::memoryPath(volume, 1));
        CHECK_EQ(rc0::decodeName(rc0::slotBody(m1, 2)), "Field Test  ");
        CHECK_EQ(rc0::tailMarker(m1).value(), 0x3e657370u); // count continued, not rewound
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

    // --- setCountIn: the toggle owns exactly the RHYTHM triple (#34) ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);

        // Preseed slot 8 with a custom rhythm the pedal could have written:
        // a drum kit and a real pattern. The toggle must never touch the kit.
        {
            std::string text = commands::readMemory(volume);
            std::string body = rc0::slotBody(text, 8);
            body = rc0::setField(body, "Kit", 3);
            body = rc0::setField(body, "Pattern", 11);
            text = rc0::replaceSlotBody(text, 8, body);
            for (const int fileNo : { 1, 2 })
                commands::writeFileBytes(volume::memoryPath(volume, fileNo),
                                         rc0::setTailMarker(text, fileNo));
        }
        const std::string before = commands::readMemory(volume);

        commands::setCountIn(volume, { 3, 8 }, true, writeOpts(tmp.path));
        const std::string afterOn = commands::readMemory(volume);
        for (const int slot : { 3, 8 }) {
            const std::string body = rc0::slotBody(afterOn, slot);
            CHECK_EQ(rc0::field(body, "State"), rc0::kRhythmStateOn);
            CHECK_EQ(rc0::field(body, "PlayCount"), rc0::kRhythmPlayCount1Meas);
            CHECK_EQ(rc0::field(body, "Pattern"), rc0::kRhythmPatternBlank);
            CHECK(catalog::readSlot(afterOn, slot).countIn);
        }
        // The custom kit survives; the drum pattern is deliberately replaced.
        CHECK_EQ(rc0::field(rc0::slotBody(afterOn, 8), "Kit"), 3);
        // No slot beyond the targeted two changed a byte.
        int changedOtherSlots = 0;
        for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
            if (slot != 3 && slot != 8 && rc0::slotBody(afterOn, slot) != rc0::slotBody(before, slot))
                ++changedOtherSlots;
        CHECK_EQ(changedOtherSlots, 0);

        // Off restores the factory zeros — NOT the pre-toggle pattern: the
        // toggle keeps no hidden state, and that is the documented contract.
        commands::setCountIn(volume, { 3, 8 }, false, writeOpts(tmp.path));
        const std::string afterOff = commands::readMemory(volume);
        for (const int slot : { 3, 8 }) {
            const std::string body = rc0::slotBody(afterOff, slot);
            CHECK_EQ(rc0::field(body, "State"), 0);
            CHECK_EQ(rc0::field(body, "PlayCount"), 0);
            CHECK_EQ(rc0::field(body, "Pattern"), 0);
            CHECK(!catalog::readSlot(afterOff, slot).countIn);
        }
        CHECK_EQ(rc0::field(rc0::slotBody(afterOff, 8), "Kit"), 3);
        // A factory slot round-trips byte-identically through on/off.
        CHECK(rc0::slotBody(afterOff, 3) == rc0::slotBody(before, 3));

        CHECK_THROWS(commands::setCountIn(volume, { 0 }, true, writeOpts(tmp.path)),
                     "out of range");
        CHECK_THROWS(commands::setCountIn(volume, { 100 }, true, writeOpts(tmp.path)),
                     "out of range");
    }

    // --- push: validate-then-write, canonical bytes, full config ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);

        // A wav long enough for a real tempo: 6860867 frames is golden
        // (64 measures, 98.7 BPM) — but huge; use 1323000 frames = 30 s
        // -> 8 measures, 96.0 BPM by the formula. Compute, don't copy.
        const int frames = 1323000;
        const auto wavBytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = frames, .extraChunk = true });
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
        CHECK_EQ(pushed.size(), kCanonicalHeader + static_cast<std::size_t>(frames) * kFrameBytes);

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
        const auto mono = testkit::syntheticWav({ .tag = 3, .channels = 1, .bits = 32, .frames = 1323000 });
        const fs::path source = tmp.path / "mono.wav";
        commands::writeFileBytes(source, std::string_view(reinterpret_cast<const char*>(mono.data()),
                                                          mono.size()));
        CHECK_THROWS(commands::push(volume, source, 5, { .write = writeOpts(tmp.path) }), "stereo");

        // Too short for the tempo range: also rejected pre-write.
        const auto tiny = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 1000 });
        const fs::path tinySource = tmp.path / "tiny.wav";
        commands::writeFileBytes(tinySource, std::string_view(reinterpret_cast<const char*>(tiny.data()),
                                                              tiny.size()));
        CHECK_THROWS(commands::push(volume, tinySource, 5, { .write = writeOpts(tmp.path) }),
                     "too short");

        // A bad slot name: rejected before the audio lands too.
        const auto ok = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 1323000 });
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

        const auto ok = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 1323000 });
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
        auto bytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = frames });
        const auto frameByte = [&](int frame) {
            return kSourceHeader + static_cast<std::size_t>(frame) * kFrameBytes;
        };
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
        CHECK_EQ(after.size(), kCanonicalHeader + 3000000u * kFrameBytes);
        CHECK_EQ(static_cast<unsigned char>(after[kCanonicalHeader]), 0xab);                          // old frame 1000000
        CHECK_EQ(static_cast<unsigned char>(after[kCanonicalHeader + 2999999u * kFrameBytes]), 0xcd);           // old frame 3999999
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

        const auto wavBytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 1323000 });
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

        // A counter past one byte is a pedal that has simply saved a lot —
        // field files at fw 1.10 carried MEMORY 0x3e65736e/0x3e65736f
        // (2026-08-09). The pair is healthy: no trailer or generation noise.
        {
            const std::string text = commands::readMemory(volume, 1);
            commands::writeFileBytes(volume::memoryPath(volume, 1),
                                     rc0::setTailGeneration(text, 0x3e65736fu));
            commands::writeFileBytes(volume::memoryPath(volume, 2),
                                     rc0::setTailGeneration(text, 0x3e65736eu));
        }
        for (const auto& finding : commands::doctor(volume))
            CHECK(finding.message.find("trailer") == std::string::npos
                  && finding.message.find("generations") == std::string::npos);

        // A structurally broken trailer is the boot-fatal condition: the
        // shape is gone (tail cut short), not merely the value unfamiliar.
        {
            const std::string m2 = commands::readFileBytes(volume::memoryPath(volume, 2));
            commands::writeFileBytes(volume::memoryPath(volume, 2), m2.substr(0, m2.size() - 2));
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

    // --- downmix: the fold lands, the stereo take stays recoverable ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const int frames = 4410;
        putStereoFloatWav(volume, 6, "take.wav", frames);
        const std::string originalBytes =
            commands::readFileBytes(volume::wavDir(volume, 6) / "take.wav");
        const std::string bodyBefore = rc0::slotBody(commands::readMemory(volume), 6);

        const auto result = commands::downmixToMono(
            volume, 6,
            { .trashRoot = tmp.path / "trash", .write = writeOpts(tmp.path, "fold-1") });

        // Same filename, canonical float32, same number of frames.
        const std::string after = commands::readFileBytes(volume::wavDir(volume, 6) / "take.wav");
        const auto afterView =
            wav::BytesView(reinterpret_cast<const unsigned char*>(after.data()), after.size());
        const wav::Info info = wav::readWavInfo(afterView);
        CHECK_EQ(info.frames, frames);
        CHECK_EQ(result.frames, frames);
        CHECK_EQ(info.format(), std::string("float32"));

        // The point of the whole feature: both channels now carry one signal.
        CHECK(wav::isDualMono(afterView));
        // Left ramps up and right ramps down, so their mean is silence — a
        // fold that kept one channel would leave a ramp here instead.
        for (std::size_t i = wav::kCanonicalFloatDataStart; i < after.size(); ++i)
            if (static_cast<unsigned char>(after[i]) != 0) {
                CHECK(false); // a non-zero byte means the channels did not cancel
                break;
            }

        // The stereo original is in the trash, byte-identical — the undo.
        CHECK(commands::readFileBytes(result.trashedOriginal) == originalBytes);

        // Folding moves no frame, so the length story in the config is
        // untouched — that is what lets this command skip the recompute trim
        // has to do.
        const std::string bodyAfter = rc0::slotBody(commands::readMemory(volume), 6);
        CHECK_EQ(rc0::field(bodyAfter, "WavLen"), rc0::field(bodyBefore, "WavLen"));
        CHECK_EQ(rc0::field(bodyAfter, "MeasLen"), rc0::field(bodyBefore, "MeasLen"));
        CHECK_EQ(rc0::field(bodyAfter, "Measure"), rc0::field(bodyBefore, "Measure"));
        CHECK_EQ(rc0::field(bodyAfter, "Tempo"), rc0::field(bodyBefore, "Tempo"));
        CHECK_EQ(bodyAfter, bodyBefore);
    }

    // --- downmix: every refusal leaves the volume byte-identical ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putStereoFloatWav(volume, 2, "take.wav", 128);   // foldable
        putWav(volume, 3, "pcm.wav", { .frames = 128 }); // pcm16 — not the pedal's own format
        putWav(volume, 4, "silent.wav",
               { .tag = 3, .channels = 2, .bits = 32, .frames = 128 }); // silence: already mono

        const auto before = volumeBytes(volume);
        const commands::DownmixOptions options { .trashRoot = tmp.path / "trash",
                                                 .write = writeOpts(tmp.path, "fold-2") };

        CHECK_THROWS(commands::downmixToMono(volume, 9, options), "no audio to fold");
        CHECK_THROWS(commands::downmixToMono(volume, 3, options), "32-bit float");
        CHECK_THROWS(commands::downmixToMono(volume, 4, options), "already folded to both outputs");
        // The refusal is per placement: silence is already "on OUTPUT A" too,
        // but a foldable take is not, and must not be refused.
        CHECK_THROWS(commands::downmixToMono(
                         volume, 4,
                         { .trashRoot = tmp.path / "trash",
                           .placement = wav::Placement::OutputAOnly,
                           .write = writeOpts(tmp.path, "fold-2b") }),
                     "already folded to OUTPUT A only");
        // A fold with nowhere to put the original must not touch the audio.
        CHECK_THROWS(commands::downmixToMono(volume, 2,
                                             { .trashRoot = {}, .write = writeOpts(tmp.path) }),
                     "requires a trash root");
        CHECK_THROWS(commands::downmixToMono(
                         volume, 2, { .trashRoot = tmp.path / "trash", .write = {} }),
                     "requires a trash root");

        CHECK(volumeBytes(volume) == before);
    }

    // --- downmix: the chosen jack is what reaches the card ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const int frames = 256;
        putUncancellingStereoFloatWav(volume, 7, "take.wav", frames);

        commands::downmixToMono(volume, 7,
                                { .trashRoot = tmp.path / "trash",
                                  .placement = wav::Placement::OutputBOnly,
                                  .write = writeOpts(tmp.path, "fold-b") });

        const std::string after = commands::readFileBytes(volume::wavDir(volume, 7) / "take.wav");
        const auto view =
            wav::BytesView(reinterpret_cast<const unsigned char*>(after.data()), after.size());
        const wav::Info info = wav::readWavInfo(view);
        CHECK_EQ(info.frames, frames);
        CHECK_EQ(info.channels, 2); // the pedal only takes stereo, placement or not

        // Channel 1 (OUTPUT A) must be exactly silent for every frame — that
        // is the whole promise of the placement, and the reason issue #43 can
        // be answered without the pedal doing anything.
        for (std::int64_t frame = 0; frame < info.frames; ++frame) {
            const std::size_t o = wav::kCanonicalFloatDataStart
                                + static_cast<std::size_t>(frame) * 8;
            for (std::size_t b = 0; b < 4; ++b)
                CHECK_EQ(static_cast<unsigned char>(after[o + b]), 0u);
        }
        // ...and channel 2 must not be: a placement that silenced everything
        // would pass the check above and be worthless.
        CHECK(!wav::foldWouldChangeNothing(view, wav::Placement::BothOutputs));
    }

    // --- downmix: the shared mutation tail still runs ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putStereoFloatWav(volume, 5, "take.wav", 64);
        commands::writeFileBytes(volume / "ROLAND" / "WAVE" / "._take.wav", "sidecar");

        const auto result = commands::downmixToMono(
            volume, 5,
            { .trashRoot = tmp.path / "trash", .write = writeOpts(tmp.path, "fold-3") });

        // Backed up, and the sidecar macOS left behind is gone: a fold is a
        // mutation like any other, not a side door around the discipline.
        CHECK(result.written.backedUp.has_value());
        CHECK(!fs::exists(volume / "ROLAND" / "WAVE" / "._take.wav"));
        // Both banks still readable and identical in content.
        CHECK_EQ(rc0::slotBody(commands::readMemory(volume, 1), 5),
                 rc0::slotBody(commands::readMemory(volume, 2), 5));
    }

    // --- doctor: the "reboot to index" hint tells the truth about format ---

    {
        // A reboot indexes a float32 take and DISCARDS a non-float one (issue
        // #44/#45). So the hint must never send a 16-bit take to a reboot: the
        // doctor reads the file and only promises a reboot for float32.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);

        // Both slots hold audio the config has not indexed (factory WavStat=0),
        // exactly the state a file copied straight onto the card leaves.
        putWav(volume, 5, "float-take.wav"); // default spec is float32
        putWav(volume, 6, "sixteen-bit.wav", { .tag = 1, .channels = 2, .bits = 16, .frames = 4410 });

        std::string floatMsg, pcmMsg;
        for (const auto& finding : commands::doctor(volume)) {
            if (finding.message.find("float-take.wav") != std::string::npos) {
                floatMsg = finding.message;
                CHECK(finding.level == commands::Level::info);
            }
            if (finding.message.find("sixteen-bit.wav") != std::string::npos) {
                pcmMsg = finding.message;
                CHECK(finding.level == commands::Level::warn);
            }
        }

        // The float32 slot keeps the reboot promise...
        CHECK(floatMsg.find("reboot the pedal to index it") != std::string::npos);
        // ...and the 16-bit slot never makes it: it says the pedal cannot
        // index the file and that a reboot would DISCARD it, and it points at
        // the fix (re-push to convert).
        CHECK(pcmMsg.find("cannot index") != std::string::npos);
        CHECK(pcmMsg.find("discard") != std::string::npos);
        CHECK(pcmMsg.find("Re-push") != std::string::npos);
        CHECK(pcmMsg.find("reboot the pedal to index it") == std::string::npos);
    }

    return testkit::summary("commands");
}
