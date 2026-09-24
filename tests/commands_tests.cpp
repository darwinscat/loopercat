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
//   - restore puts a recorded slot state back as ONE unit: nothing recomputed,
//     the other 98 slots untouched, a state that disagrees with itself refused
//   - pull renames technical names, disambiguates duplicates, refuses to
//     overwrite without force
//   - doctor reports junk, trailer damage, pair divergence, config/audio
//     disagreements
//   - on a real card (fixtures/rc5-card.RC0) a mutation changes exactly the
//     fields it exists to write, each in the section that owns it, and no
//     other byte; a same-named tag in a section the mutations do not own is
//     neither read nor written

#include "support.hpp"

#include "../app/OperationId.h"

#include <loopercat/Commands.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <utility>
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

// A take at a KNOWN loudness (issue #53): 997 Hz — the tone EBU Tech 3341
// calibrates on — at `dbfs` peak in both channels, so a -23 dBFS take reads
// -23 LUFS. `spike` plants one hot frame-0 sample on channel 0, the
// quiet-but-peaky shape whose boost must stop at the ceiling.
void putSineFloatWav(const fs::path& volume, int slot, const std::string& name, int frames,
                     double dbfs, float spike = 0.0f)
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
    const double amp = std::pow(10.0, dbfs / 20.0);
    const double w = 2.0 * std::numbers::pi * 997.0 / wav::kSampleRate;
    for (int frame = 0; frame < frames; ++frame) {
        const auto v = static_cast<float>(amp * std::sin(w * frame));
        sample(frame == 0 && spike > 0.0f ? spike : v);
        sample(v);
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

// Every call mints a fresh id, exactly as the app does — through the app's own
// generator, so the suite exercises the thing that ships. Sharing one id
// between two operations is a bug, and a test has to ask for it by name.
// The archive is the transitional trash folder under the same root, so a
// test finds a kept take exactly where the app would have put it.
commands::WriteOptions writeOpts(const fs::path& root, std::string opId = opid::make("op"))
{
    commands::Archive archive = commands::trashFolder(root / "trash", opId);
    return { .backupRoot = root / "backups", .opId = std::move(opId), .archive = std::move(archive) };
}

// The same options with the archive taken away — what every refusal is about.
commands::WriteOptions withoutArchive(commands::WriteOptions write)
{
    write.archive = nullptr;
    return write;
}

// Where writeOpts' trash folder keeps a take the operation archived.
fs::path keptTake(const fs::path& root, const commands::WriteOptions& write, int slot,
                  const std::string& fileName)
{
    return root / "trash" / write.opId / volume::slotDirName(slot) / fileName;
}

// The bytes of a synthetic wav as the string a Take carries.
std::string bytesOf(const std::vector<unsigned char>& wav)
{
    return std::string(reinterpret_cast<const char*>(wav.data()), wav.size());
}

// A slot as the pedal leaves it after a recording: the file in its folder,
// WavStat=1 and WavLen counting its frames in both banks. Returns the state
// the slot is then in — the unit a restore takes back.
commands::SlotState recordOnPedal(const fs::path& volume, int slot, const std::string& name,
                                  int frames)
{
    putWav(volume, slot, name, { .tag = 3, .bits = 32, .frames = frames });
    const std::string text = commands::readMemory(volume);
    std::string body = rc0::slotBody(text, slot);
    body = rc0::setField(body, "WavStat", 1);
    body = rc0::setField(body, "WavLen", frames);
    commands::writeMemoryPair(volume, rc0::replaceSlotBody(text, slot, body),
                              { .skipBackup = true });
    return { rc0::slotBody(commands::readMemory(volume), slot),
             commands::Take { name, commands::readFileBytes(volume::wavDir(volume, slot) / name) } };
}

// --- a real card, and the field-level view of a change ---

// The real card, as the pedal wrote it (fixtures/rc5-card.RC0): 99 memories,
// 41 of them a loop, every byte off an RC-5.
std::string cardText()
{
    return commands::readFileBytes(LOOPERCAT_RC5_CARD);
}

// A scratch volume holding the real card in both banks: a mutation then
// starts from exactly the bytes a pedal wrote.
fs::path makePedalFromCard(const fs::path& root)
{
    const fs::path volume = root / "PEDAL";
    fs::create_directories(volume / "ROLAND" / "WAVE");
    fs::create_directories(volume::dataDir(volume));
    const std::string card = cardText();
    for (const int fileNo : { 1, 2 })
        commands::writeFileBytes(volume::memoryPath(volume, fileNo), card);
    return volume;
}

// One line of a memory body is one field — a tab, then <Tag>value</Tag> — or
// one section's opener or closer on a line of its own: the shape every RC-5
// writes (the card fixture; golden.json's factory slot). A field line taken
// apart, or nothing for any other line.
struct FieldLine {
    std::string tag;
    long long value;
};

std::optional<FieldLine> parseFieldLine(const std::string& line)
{
    if (line.size() < 2 || line[0] != '\t' || line[1] != '<')
        return std::nullopt;
    const auto tagEnd = line.find('>', 2);
    if (tagEnd == std::string::npos)
        return std::nullopt;
    const std::string tag = line.substr(2, tagEnd - 2);
    const std::string close = "</" + tag + ">";
    const std::size_t valueBegin = tagEnd + 1;
    if (line.size() < valueBegin + close.size()
        || line.compare(line.size() - close.size(), close.size(), close) != 0)
        return std::nullopt;
    const std::string digits = line.substr(valueBegin, line.size() - close.size() - valueBegin);
    std::size_t i = (!digits.empty() && digits[0] == '-') ? 1 : 0;
    if (i == digits.size())
        return std::nullopt;
    for (; i < digits.size(); ++i)
        if (digits[i] < '0' || digits[i] > '9')
            return std::nullopt;
    return FieldLine { tag, std::stoll(digits) };
}

// What changed between two bodies of one memory, as "SECTION.Tag before->after"
// in the body's own order — or "shape" when the bodies differ in anything but
// field values: a line added or lost, a tag renamed, a line outside any
// section touched. A field's value is the only unit a mutation may change;
// everything else about the body is the pedal's, byte for byte.
std::string fieldChanges(const std::string& before, const std::string& after)
{
    const auto lines = [](const std::string& text) {
        std::vector<std::string> out;
        std::size_t from = 0;
        while (true) {
            const auto nl = text.find('\n', from);
            if (nl == std::string::npos) {
                out.push_back(text.substr(from));
                return out;
            }
            out.push_back(text.substr(from, nl - from));
            from = nl + 1;
        }
    };
    const std::vector<std::string> a = lines(before);
    const std::vector<std::string> b = lines(after);
    if (a.size() != b.size())
        return "shape";
    std::string section;
    std::string out;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) {
            // <X> on a line of its own opens section X; </X> closes it.
            if (a[i].size() > 2 && a[i].front() == '<' && a[i].back() == '>'
                && a[i].find('<', 1) == std::string::npos) {
                if (a[i][1] == '/')
                    section.clear();
                else
                    section = a[i].substr(1, a[i].size() - 2);
            }
            continue;
        }
        const auto fa = parseFieldLine(a[i]);
        const auto fb = parseFieldLine(b[i]);
        if (!fa || !fb || fa->tag != fb->tag || section.empty())
            return "shape";
        if (!out.empty())
            out += ", ";
        out += section + "." + fa->tag + " " + std::to_string(fa->value) + "->"
             + std::to_string(fb->value);
    }
    return out;
}

// What one mutation of `slot` did to the memory document: that memory's field
// changes, with every byte outside the memory asserted identical (the trailer
// aside — every write stamps it).
std::string slotChangesOnCard(const std::string& before, const std::string& after, int slot)
{
    const std::string afterBody = rc0::slotBody(after, slot);
    CHECK(rc0::splitFile(rc0::replaceSlotBody(before, slot, afterBody)).document
          == rc0::splitFile(after).document);
    return fieldChanges(rc0::slotBody(before, slot), afterBody);
}

} // namespace

int main()
{
    // --- operation identity: the archive is never shared, never overwritten ---
    //
    // The identity used to be the wall clock at one-second resolution, and a
    // bulk apply enqueues all its jobs inside one second: slots 32, 33 and 34
    // normalized together left ONE backup directory and two pre-states were
    // gone (issue #72). Theory: an operation owns its archive, and a reused id
    // is loud rather than destructive.

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);

        // Both ids are minted in the same second, from the same label.
        const std::string label = "2026-09-01T21-35-46";
        const commands::WriteOptions first = writeOpts(tmp.path, opid::make(label));
        const commands::WriteOptions second = writeOpts(tmp.path, opid::make(label));
        CHECK(first.opId != second.opId);

        const std::string before = commands::readFileBytes(volume::memoryPath(volume, 1));
        commands::rename(volume, 3, "First", first);
        const std::string between = commands::readFileBytes(volume::memoryPath(volume, 1));
        commands::rename(volume, 4, "Second", second);

        CHECK(before != between); // the two operations really did differ
        CHECK(commands::readFileBytes(tmp.path / "backups" / first.opId / "MEMORY1.RC0")
              == before);
        CHECK(commands::readFileBytes(tmp.path / "backups" / second.opId / "MEMORY1.RC0")
              == between);
    }

    {
        // A reused id refuses, and refuses BEFORE it has cost anything.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const commands::WriteOptions shared = writeOpts(tmp.path, "one-id-for-two");

        const std::string before = commands::readFileBytes(volume::memoryPath(volume, 1));
        commands::rename(volume, 3, "First", shared);
        const std::string kept = commands::readMemory(volume);

        CHECK_THROWS(commands::rename(volume, 4, "Second", shared), "under one id");
        CHECK(commands::readFileBytes(tmp.path / "backups" / shared.opId / "MEMORY1.RC0")
              == before);
        CHECK(rc0::slotBody(commands::readMemory(volume), 4) == rc0::slotBody(kept, 4));
    }

    {
        // The case that loses audio outright: one id touching one slot twice.
        // The first take is in the archive, and nothing may write over it.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 5, "005_1.WAV", { .tag = 3, .bits = 32, .frames = 4410 });
        const std::string firstTake =
            commands::readFileBytes(volume::wavDir(volume, 5) / "005_1.WAV");

        commands::ClearOptions options { .write = writeOpts(tmp.path, "one-id-twice") };
        const auto cleared = commands::clear(volume, { 5 }, options);
        const fs::path kept = keptTake(tmp.path, options.write, 5, cleared.archived.front());
        CHECK(commands::readFileBytes(kept) == firstTake);

        // A second take lands in the same slot, and the same id clears it.
        putWav(volume, 5, "005_1.WAV", { .tag = 3, .bits = 32, .frames = 8820 });
        CHECK_THROWS(commands::clear(volume, { 5 }, options), "under one id");

        // The first take is still the first take, and the refusal kept the
        // second one on the card rather than deleting it into nothing.
        CHECK(commands::readFileBytes(kept) == firstTake);
        CHECK(volume::listSlotWavs(volume, 5).size() == 1);
    }

    // --- the archive comes BEFORE the card changes ---
    //
    // Theory: a take is handed to the archive while it is still in its slot,
    // whole and byte-identical, and the card changes only after the archive
    // returns. An archive that throws therefore costs nothing: the command
    // stops with the volume exactly as it was. The recording archive below
    // looks at the card at the moment it is called — that is the ordering
    // proof, not just "a copy exists somewhere afterwards".

    {
        struct Kept {
            int slot;
            std::string name;
            std::string bytes;      // what the archive was handed
            std::string onCardThen; // what the slot's file held at that moment
        };
        const auto recording = [](const fs::path& volume, std::vector<Kept>& kept) {
            return commands::WriteOptions {
                .opId = opid::make("rec"),
                .skipBackup = true,
                .archive = [&volume, &kept](int slot, const std::string& name,
                                            std::string_view bytes) {
                    const fs::path onCard = volume::wavDir(volume, slot) / name;
                    kept.push_back({ slot, name, std::string(bytes),
                                     fs::exists(onCard) ? commands::readFileBytes(onCard)
                                                        : std::string("<gone>") });
                },
            };
        };
        const auto takeOf = [](const fs::path& volume, int slot, const std::string& name) {
            return commands::readFileBytes(volume::wavDir(volume, slot) / name);
        };
        const auto keptBeforeTheCardChanged = [](const std::vector<Kept>& kept, int slot,
                                                 const std::string& name,
                                                 const std::string& original) {
            CHECK_EQ(kept.size(), 1u);
            if (kept.size() != 1u)
                return;
            CHECK_EQ(kept.front().slot, slot);
            CHECK_EQ(kept.front().name, name);
            CHECK(kept.front().bytes == original);      // the whole take, byte for byte
            CHECK(kept.front().onCardThen == original); // and the card still held it
        };

        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        // 3 s: long enough that the pedal's 160 BPM ceiling allows it a measure
        const auto sourceBytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 132300 });
        const fs::path source = tmp.path / "incoming.wav";
        commands::writeFileBytes(source,
                                 std::string_view(reinterpret_cast<const char*>(sourceBytes.data()),
                                                  sourceBytes.size()));

        {   // push over an occupied slot
            putWav(volume, 11, "old.wav");
            const std::string original = takeOf(volume, 11, "old.wav");
            std::vector<Kept> kept;
            commands::push(volume, source, 11, { .force = true, .write = recording(volume, kept) });
            keptBeforeTheCardChanged(kept, 11, "old.wav", original);
        }
        {   // trim
            putWav(volume, 12, "take.wav");
            const std::string original = takeOf(volume, 12, "take.wav");
            std::vector<Kept> kept;
            commands::trim(volume, 12, 0, 2205, { .write = recording(volume, kept) });
            keptBeforeTheCardChanged(kept, 12, "take.wav", original);
        }
        {   // downmix
            putStereoFloatWav(volume, 13, "stereo.wav", 256);
            const std::string original = takeOf(volume, 13, "stereo.wav");
            std::vector<Kept> kept;
            commands::downmixToMono(volume, 13, { .write = recording(volume, kept) });
            keptBeforeTheCardChanged(kept, 13, "stereo.wav", original);
        }
        {   // normalize
            putSineFloatWav(volume, 14, "quiet.wav", 44100, -23.0);
            const std::string original = takeOf(volume, 14, "quiet.wav");
            std::vector<Kept> kept;
            const auto result = commands::normalize(
                volume, 14, { .targetLufs = -18.0, .write = recording(volume, kept) });
            CHECK(result.applied);
            keptBeforeTheCardChanged(kept, 14, "quiet.wav", original);
        }
        {   // clear
            putWav(volume, 15, "gone.wav");
            const std::string original = takeOf(volume, 15, "gone.wav");
            std::vector<Kept> kept;
            commands::clear(volume, { 15 }, { .write = recording(volume, kept) });
            keptBeforeTheCardChanged(kept, 15, "gone.wav", original);
        }
        {   // restore: the take the slot holds goes to the archive before the
            // restored one lands — under the SAME name, so an archive fed
            // after the write would be handed the wrong bytes
            const commands::SlotState older = recordOnPedal(volume, 17, "017_1.WAV", 8820);
            commands::clear(volume, { 17 }, { .write = writeOpts(tmp.path) });
            recordOnPedal(volume, 17, "017_1.WAV", 4410);
            const std::string original = takeOf(volume, 17, "017_1.WAV");
            std::vector<Kept> kept;
            commands::restore(volume, 17, older, recording(volume, kept));
            keptBeforeTheCardChanged(kept, 17, "017_1.WAV", original);
        }
        {   // clear with trash=false is the player's "Delete permanently":
            // nothing is handed over, and the file is reported as deleted
            putWav(volume, 16, "unkept.wav");
            std::vector<Kept> kept;
            const auto result = commands::clear(
                volume, { 16 }, { .trash = false, .write = recording(volume, kept) });
            CHECK(kept.empty());
            CHECK_EQ(result.deleted.size(), 1u);
        }
    }

    {
        // An archive that fails stops the command before the card changes —
        // for every command that would have destroyed a take.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 11, "old.wav");
        putWav(volume, 12, "take.wav");
        putStereoFloatWav(volume, 13, "stereo.wav", 256);
        putSineFloatWav(volume, 14, "quiet.wav", 44100, -23.0);
        putWav(volume, 15, "gone.wav");
        putWav(volume, 16, "kept.wav");
        // 3 s: long enough that the pedal's 160 BPM ceiling allows it a measure
        const auto sourceBytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 132300 });
        const fs::path source = tmp.path / "incoming.wav";
        commands::writeFileBytes(source,
                                 std::string_view(reinterpret_cast<const char*>(sourceBytes.data()),
                                                  sourceBytes.size()));
        const auto before = volumeBytes(volume);

        const commands::WriteOptions failing {
            .opId = "archive-fails",
            .skipBackup = true,
            .archive = [](int, const std::string&, std::string_view) {
                throw Error("the archive is full");
            },
        };
        CHECK_THROWS(commands::push(volume, source, 11, { .force = true, .write = failing }),
                     "the archive is full");
        CHECK_THROWS(commands::trim(volume, 12, 0, 2205, { .write = failing }),
                     "the archive is full");
        CHECK_THROWS(commands::downmixToMono(volume, 13, { .write = failing }),
                     "the archive is full");
        CHECK_THROWS(commands::normalize(volume, 14, { .targetLufs = -18.0, .write = failing }),
                     "the archive is full");
        CHECK_THROWS(commands::clear(volume, { 15 }, { .write = failing }), "the archive is full");
        CHECK_THROWS(commands::restore(volume, 16, { rc0::factorySlotBody(16), std::nullopt }, failing),
                     "the archive is full");

        CHECK(volumeBytes(volume) == before);
    }

    {
        // The transitional folder keeps the on-disk shape the guide describes:
        // <root>/<operation>/<NNN_1>/<file>, byte-identical, and never over
        // itself.
        TempDir tmp;
        const commands::Archive folder = commands::trashFolder(tmp.path / "trash", "op-x");
        folder(7, "take.wav", "first");
        CHECK(commands::readFileBytes(tmp.path / "trash" / "op-x" / "007_1" / "take.wav") == "first");
        CHECK_THROWS(folder(7, "take.wav", "second"), "under one id");
        CHECK(commands::readFileBytes(tmp.path / "trash" / "op-x" / "007_1" / "take.wav") == "first");
        CHECK_THROWS(commands::trashFolder({}, "op-x"), "needs a root and an operation id");
        CHECK_THROWS(commands::trashFolder(tmp.path / "trash", ""), "needs a root and an operation id");
    }

    // --- the journal: what a write changes, told before it changes it ---
    //
    // Theory: the history records a write as the slot bodies it changed, each
    // reported while the card still holds the old one, plus the audio that
    // landed. A write that changes the document anywhere else could not be
    // described, so it must not happen at all.

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const std::string original = commands::readMemory(volume);

        // rename: exactly one slot, before and after byte-exact, and the card
        // still held the old body when the journal heard about it.
        std::vector<commands::SlotChange> heard;
        std::string cardWhenHeard;
        commands::WriteOptions options = writeOpts(tmp.path);
        options.journal.bodiesChanging = [&](const std::vector<commands::SlotChange>& changes) {
            heard = changes;
            cardWhenHeard = commands::readMemory(volume);
        };
        commands::rename(volume, 7, "Heard", options);
        CHECK_EQ(heard.size(), 1u);
        if (heard.size() == 1u) {
            CHECK_EQ(heard.front().slot, 7);
            CHECK(heard.front().before == rc0::slotBody(original, 7));
            CHECK(heard.front().after == rc0::slotBody(commands::readMemory(volume), 7));
            CHECK(heard.front().before != heard.front().after);
        }
        CHECK(cardWhenHeard == original);
    }

    {
        // Several slots in one write: every changed slot, in slot order, and
        // not one slot more — slot 9 is in the list but already off.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        commands::setOneShot(volume, { 9 }, false, writeOpts(tmp.path));
        std::vector<commands::SlotChange> heard;
        commands::WriteOptions options = writeOpts(tmp.path);
        options.journal.bodiesChanging = [&](const std::vector<commands::SlotChange>& c) { heard = c; };
        commands::setOneShot(volume, { 40, 9, 3 }, true, options);
        std::vector<int> slots;
        for (const auto& change : heard)
            slots.push_back(change.slot);
        CHECK((slots == std::vector<int> { 3, 9, 40 }));
    }

    {
        // swap: both slots, each after-body being the other's before-body.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        commands::rename(volume, 3, "Three", writeOpts(tmp.path));
        commands::rename(volume, 7, "Seven", writeOpts(tmp.path));
        std::vector<commands::SlotChange> heard;
        commands::WriteOptions options = writeOpts(tmp.path);
        options.journal.bodiesChanging = [&](const std::vector<commands::SlotChange>& c) { heard = c; };
        commands::swap(volume, 3, 7, options);
        CHECK_EQ(heard.size(), 2u);
        if (heard.size() == 2u) {
            CHECK_EQ(heard[0].slot, 3);
            CHECK_EQ(heard[1].slot, 7);
            CHECK(heard[0].after == heard[1].before);
            CHECK(heard[1].after == heard[0].before);
        }
    }

    {
        // A journal that cannot keep up stops the write: the volume is as it was.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const auto before = volumeBytes(volume);
        commands::WriteOptions options = writeOpts(tmp.path);
        options.skipBackup = true;
        options.journal.bodiesChanging = [](const std::vector<commands::SlotChange>&) {
            throw Error("the history is unavailable");
        };
        CHECK_THROWS(commands::rename(volume, 7, "Never", options), "the history is unavailable");
        CHECK(volumeBytes(volume) == before);
        // swap moved its audio before the pair write; the refusal moves it back
        putWav(volume, 3, "003_1.WAV");
        const auto withAudio = volumeBytes(volume);
        CHECK_THROWS(commands::swap(volume, 3, 7, options), "the history is unavailable");
        CHECK(volumeBytes(volume) == withAudio);
    }

    {
        // A change outside every slot is one no row could describe: refused,
        // before anything is written.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const auto before = volumeBytes(volume);
        std::string text = commands::readMemory(volume);
        const auto closing = text.find("</database>");
        CHECK(closing != std::string::npos);
        text.insert(closing, "<stray/>");
        CHECK_THROWS(commands::writeMemoryPair(volume, text, writeOpts(tmp.path)),
                     "outside its slots");
        CHECK(volumeBytes(volume) == before);
        CHECK(!fs::exists(tmp.path / "backups")); // refused before the backup, too
    }

    {
        // Audio-only rewrites restamp the pair without touching a body — the
        // journal hears an empty list, and hears the audio instead.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putStereoFloatWav(volume, 13, "stereo.wav", 256);
        putSineFloatWav(volume, 14, "quiet.wav", 44100, -23.0);

        std::optional<std::vector<commands::SlotChange>> bodies;
        std::vector<std::pair<int, std::string>> landedName;
        std::vector<std::string> landedBytes;
        const auto listening = [&](commands::WriteOptions options) {
            options.journal.bodiesChanging = [&](const std::vector<commands::SlotChange>& c) {
                bodies = c;
            };
            options.journal.audioWritten = [&](int slot, const std::string& name,
                                               std::string_view bytes) {
                landedName.emplace_back(slot, name);
                landedBytes.emplace_back(bytes);
            };
            return options;
        };

        commands::downmixToMono(volume, 13, { .write = listening(writeOpts(tmp.path)) });
        CHECK(bodies.has_value() && bodies->empty());
        CHECK_EQ(landedName.size(), 1u);
        CHECK((landedName.back() == std::pair<int, std::string> { 13, "stereo.wav" }));
        CHECK(landedBytes.back() == commands::readFileBytes(volume::wavDir(volume, 13) / "stereo.wav"));

        bodies.reset();
        commands::normalize(volume, 14, { .targetLufs = -18.0, .write = listening(writeOpts(tmp.path)) });
        CHECK(bodies.has_value() && bodies->empty());
        CHECK_EQ(landedName.size(), 2u);
        CHECK((landedName.back() == std::pair<int, std::string> { 14, "quiet.wav" }));
        CHECK(landedBytes.back() == commands::readFileBytes(volume::wavDir(volume, 14) / "quiet.wav"));

        // push and trim: the take that landed is the take on the card
        const auto sourceBytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 132300 });
        const fs::path source = tmp.path / "incoming.wav";
        commands::writeFileBytes(source,
                                 std::string_view(reinterpret_cast<const char*>(sourceBytes.data()),
                                                  sourceBytes.size()));
        commands::push(volume, source, 20, { .write = listening(writeOpts(tmp.path)) });
        CHECK((landedName.back() == std::pair<int, std::string> { 20, "incoming.wav" }));
        CHECK(landedBytes.back() == commands::readFileBytes(volume::wavDir(volume, 20) / "incoming.wav"));

        commands::trim(volume, 20, 0, 88200, { .write = listening(writeOpts(tmp.path)) });
        CHECK((landedName.back() == std::pair<int, std::string> { 20, "incoming.wav" }));
        CHECK(landedBytes.back() == commands::readFileBytes(volume::wavDir(volume, 20) / "incoming.wav"));
        CHECK_EQ(landedName.size(), 4u); // one per take written, never more
    }

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

        // Occupied slot: refused without force; force without an archive is
        // refused too — the replaced take must have somewhere safe to go.
        CHECK_THROWS(commands::push(volume, source, 9, { .write = writeOpts(tmp.path, "op-2") }),
                     "already has audio");
        CHECK_THROWS(commands::push(volume, source, 9,
                                    { .force = true,
                                      .write = withoutArchive(writeOpts(tmp.path, "op-2")) }),
                     "needs an archive");
        CHECK_EQ(volume::listSlotWavs(volume, 9).size(), 1u);

        // Forced replace: the old take lands in the archive byte-identical —
        // push never deletes audio outright.
        const commands::WriteOptions replacing = writeOpts(tmp.path, "op-3");
        const auto forced = commands::push(volume, source, 9, { .force = true, .write = replacing });
        CHECK_EQ(volume::listSlotWavs(volume, 9).size(), 1u);
        CHECK(forced.configured);
        CHECK_EQ(forced.archived.size(), 1u);
        CHECK(commands::readFileBytes(keptTake(tmp.path, replacing, 9, forced.archived.front()))
              == pushed);
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

        commands::ClearOptions options { .write = writeOpts(tmp.path) };
        const auto result = commands::clear(volume, { 5 }, options);

        // The audio is out of the slot but safe in the archive, byte-identical.
        CHECK(volume::listSlotWavs(volume, 5).empty());
        CHECK_EQ(result.archived.size(), 1u);
        CHECK(commands::readFileBytes(keptTake(tmp.path, options.write, 5, result.archived.front()))
              == wavBytesBefore);

        // The slot body is EXACTLY the factory one (byte-level, not field-level).
        const std::string text = commands::readMemory(volume);
        CHECK(rc0::slotBody(text, 5) == rc0::factorySlotBody(5));

        // keepName: factory values, surviving name.
        commands::rename(volume, 6, "Keep Me", writeOpts(tmp.path, "op-2"));
        commands::ClearOptions keep { .keepName = true, .write = writeOpts(tmp.path, "op-3") };
        commands::clear(volume, { 6 }, keep);
        const std::string after = commands::readMemory(volume);
        CHECK_EQ(rc0::decodeName(rc0::slotBody(after, 6)), "Keep Me     ");
        CHECK_EQ(rc0::field(rc0::slotBody(after, 6), "Measure"), 1); // factory value

        // No archive -> fail fast before touching anything.
        putWav(volume, 8, "safe.wav");
        commands::ClearOptions bad { .write = withoutArchive(writeOpts(tmp.path, "op-4")) };
        CHECK_THROWS(commands::clear(volume, { 8 }, bad), "needs an archive");
        CHECK_EQ(volume::listSlotWavs(volume, 8).size(), 1u);
    }

    // --- restore: a recorded slot state goes back as one unit (#50) ---
    //
    // Theory: the body is spliced into the live document byte for byte and
    // the take goes back under its own name — nothing recomputed, so the
    // tempo the player set survives where push would have replaced it with
    // the import guess. The other 98 slots belong to the present. The body
    // and the take are one unit: a WavLen that does not count the take's
    // frames is refused with the card untouched, so is a body whose WavStat
    // claims a take it does not bring (or denies the one it does), and so is
    // a take the pedal would not play.

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);

        // Slot 5 as the player left it: a 60 s recording, named, with its
        // TRUE tempo set — 112.0 BPM, which is not what the import formula
        // derives for this length. That difference is the whole point.
        recordOnPedal(volume, 5, "005_1.WAV", 2646000);
        commands::rename(volume, 5, "Good Take", { .skipBackup = true });
        commands::setTempo(volume, 5, 1120, { .skipBackup = true });
        const commands::SlotState good {
            rc0::slotBody(commands::readMemory(volume), 5),
            commands::Take { "005_1.WAV",
                             commands::readFileBytes(volume::wavDir(volume, 5) / "005_1.WAV") }
        };
        CHECK(params::computeSlotParams(2646000).tempoTenths != 1120);
        CHECK_EQ(rc0::field(good.body, "Tempo"), 1120);

        // Then the slot is cleared and re-recorded on the pedal, and the
        // present moves on elsewhere too.
        commands::clear(volume, { 5 }, { .write = writeOpts(tmp.path, "clear-5") });
        recordOnPedal(volume, 5, "005_1.WAV", 4410);
        commands::rename(volume, 6, "Meanwhile", { .skipBackup = true });
        const std::string present = commands::readMemory(volume);
        const std::string newerTake =
            commands::readFileBytes(volume::wavDir(volume, 5) / "005_1.WAV");
        CHECK(newerTake != good.take->bytes);

        const commands::WriteOptions options = writeOpts(tmp.path, "restore-5");
        const auto result = commands::restore(volume, 5, good, options);

        // The slot is the recorded state byte for byte: body AND take, the
        // tempo the player set included — push would have recomputed it.
        const std::string after = commands::readMemory(volume);
        CHECK(rc0::slotBody(after, 5) == good.body);
        CHECK_EQ(rc0::field(rc0::slotBody(after, 5), "Tempo"), 1120);
        CHECK_EQ(rc0::decodeName(rc0::slotBody(after, 5)), "Good Take   ");
        CHECK((volume::listSlotWavs(volume, 5) == std::vector<std::string> { "005_1.WAV" }));
        CHECK(commands::readFileBytes(volume::wavDir(volume, 5) / "005_1.WAV") == good.take->bytes);
        CHECK_EQ(result.frames, 2646000);

        // The other 98 slots are the PRESENT's, byte for byte — slot 6's new
        // name included: an old document is never written wholesale.
        int changedOtherSlots = 0;
        for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
            if (slot != 5 && rc0::slotBody(after, slot) != rc0::slotBody(present, slot))
                ++changedOtherSlots;
        CHECK_EQ(changedOtherSlots, 0);
        CHECK_EQ(rc0::decodeName(rc0::slotBody(after, 6)), "Meanwhile   ");

        // The take the slot held went to the archive, byte-identical; the
        // shared discipline ran; the card agrees with itself afterwards.
        CHECK((result.archived == std::vector<std::string> { "005_1.WAV" }));
        CHECK(commands::readFileBytes(keptTake(tmp.path, options, 5, "005_1.WAV")) == newerTake);
        CHECK(result.written.backedUp.has_value());
        CHECK(commands::doctor(volume).empty());
    }

    {
        // A state without a take — a cleared slot — is a unit as well: the
        // takes in the slot go to the archive before they leave, all of them
        // (nothing stops a FAT folder from holding two), and the folder ends
        // up empty with the body in place. Onto an empty slot it replaces no
        // take, so it needs no archive.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 9, "a.wav");
        putWav(volume, 9, "b.wav", { .tag = 3, .bits = 32, .frames = 8820 });
        const std::string a = commands::readFileBytes(volume::wavDir(volume, 9) / "a.wav");
        const std::string b = commands::readFileBytes(volume::wavDir(volume, 9) / "b.wav");
        const commands::SlotState cleared { rc0::factorySlotBody(9), std::nullopt };

        const commands::WriteOptions options = writeOpts(tmp.path);
        const auto result = commands::restore(volume, 9, cleared, options);
        CHECK((result.archived == std::vector<std::string> { "a.wav", "b.wav" }));
        CHECK(commands::readFileBytes(keptTake(tmp.path, options, 9, "a.wav")) == a);
        CHECK(commands::readFileBytes(keptTake(tmp.path, options, 9, "b.wav")) == b);
        CHECK(volume::listSlotWavs(volume, 9).empty());
        CHECK(rc0::slotBody(commands::readMemory(volume), 9) == rc0::factorySlotBody(9));
        CHECK_EQ(result.frames, 0);
        CHECK(commands::doctor(volume).empty()); // the body and the empty folder agree

        const auto empty = commands::restore(volume, 10, { rc0::factorySlotBody(10), std::nullopt },
                                             withoutArchive(writeOpts(tmp.path)));
        CHECK(empty.archived.empty());
        CHECK(rc0::slotBody(commands::readMemory(volume), 10) == rc0::factorySlotBody(10));
    }

    {
        // Refusals, each before any write: the state must agree with itself,
        // and every crooked input is named. The card stays byte-identical and
        // nothing is spent on the archive or the backup.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const commands::SlotState whole = recordOnPedal(volume, 5, "005_1.WAV", 4410);
        const std::string& body = whole.body;
        const std::string& bytes = whole.take->bytes;
        const auto before = volumeBytes(volume);
        const commands::WriteOptions options = writeOpts(tmp.path);

        // The body counts 8820 frames while the take holds 4410 — and the
        // other way round, a take of another length under the same name.
        CHECK_THROWS(commands::restore(volume, 5, { rc0::setField(body, "WavLen", 8820), whole.take },
                                       options),
                     "WavLen=8820 but its take \"005_1.WAV\" holds 4410 frames");
        const std::string longer =
            bytesOf(testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 8820 }));
        CHECK_THROWS(commands::restore(volume, 5, { body, commands::Take { "005_1.WAV", longer } },
                                       options),
                     "WavLen=4410 but its take \"005_1.WAV\" holds 8820 frames");
        // A body that claims audio with no take to go with it: "restore the
        // settings" alone is exactly the half that is refused.
        CHECK_THROWS(commands::restore(volume, 5, { body, std::nullopt }, options),
                     "WavLen=4410 but it carries no take");
        // A take beside a body that says the slot is empty.
        CHECK_THROWS(commands::restore(volume, 5, { rc0::factorySlotBody(5), whole.take }, options),
                     "WavStat=0, WavLen=0 but its take");
        // WavStat is what the pedal — and doctor — read as "this slot holds a
        // take": a body that claims one beside no take (the slot doctor
        // reports as "configured with audio but its folder is empty"), or
        // denies the one it brings, is not a unit either, whatever WavLen says.
        CHECK_THROWS(commands::restore(volume, 5,
                                       { rc0::setField(rc0::factorySlotBody(5), "WavStat", 1),
                                         std::nullopt },
                                       options),
                     "WavStat=1, WavLen=0 but it carries no take");
        CHECK_THROWS(commands::restore(volume, 5, { rc0::setField(body, "WavStat", 0), whole.take },
                                       options),
                     "WavStat=0, WavLen=4410 but its take");
        CHECK_THROWS(commands::restore(volume, 5, { rc0::setField(body, "WavStat", 2), whole.take },
                                       options),
                     "WavStat=2, WavLen=4410 but its take");
        // A take the pedal does not play — 16-bit, or mono — is refused at the
        // door exactly as push refuses it, frames matching or not: no state
        // recorded from a card looks like this, and the pedal would discard
        // it at its next boot rather than index it (issue #44).
        CHECK_THROWS(commands::restore(volume, 5,
                                       { body, commands::Take { "005_1.WAV",
                                                                bytesOf(testkit::syntheticWav(
                                                                    { .tag = 1, .bits = 16, .frames = 4410 })) } },
                                       options),
                     "32-bit float");
        CHECK_THROWS(commands::restore(volume, 5,
                                       { body, commands::Take { "005_1.WAV",
                                                                bytesOf(testkit::syntheticWav(
                                                                    { .tag = 3, .channels = 1, .bits = 32, .frames = 4410 })) } },
                                       options),
                     "stereo");

        // Crooked inputs.
        CHECK_THROWS(commands::restore(volume, 0, whole, options), "out of range");
        CHECK_THROWS(commands::restore(volume, 100, whole, options), "out of range");
        CHECK_THROWS(commands::restore(volume, 5, {}, options), "needs a slot body");
        for (const char* name : { "", ".", "..", "../005_1.WAV", "WAVE/005_1.WAV", "._005_1.WAV",
                                  ".DS_Store" })
            CHECK_THROWS(commands::restore(volume, 5, { body, commands::Take { name, bytes } },
                                           options),
                         "not a file name a take can carry");
        CHECK_THROWS(commands::restore(volume, 5, { body, commands::Take { "005_1.WAV", "" } },
                                       options),
                     "RIFF");
        CHECK_THROWS(commands::restore(volume, 5,
                                       { body, commands::Take { "005_1.WAV", "not audio at all" } },
                                       options),
                     "RIFF");
        CHECK_THROWS(commands::restore(volume, 5, { body + "</mem>", whole.take }, options),
                     "<mem> block");
        CHECK_THROWS(commands::restore(volume, 5, { body + "<mem id=\"98\">", whole.take }, options),
                     "<mem> block");
        CHECK_THROWS(commands::restore(volume, 5, whole, withoutArchive(options)), "needs an archive");

        CHECK(volumeBytes(volume) == before);
        CHECK(!fs::exists(tmp.path / "trash"));
        CHECK(!fs::exists(tmp.path / "backups"));
    }

    {
        // A restore is an operation like any other: the journal hears the
        // body it replaces and the body it puts back — while the card still
        // holds the old one — and the take that landed, whole.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const commands::SlotState recorded = recordOnPedal(volume, 7, "take.wav", 4410);
        // The present moves on: another take, another name.
        commands::clear(volume, { 7 }, { .write = writeOpts(tmp.path) });
        recordOnPedal(volume, 7, "take.wav", 8820);
        commands::rename(volume, 7, "Later", { .skipBackup = true });
        const std::string present = commands::readMemory(volume);

        std::vector<commands::SlotChange> heard;
        std::string cardWhenHeard;
        std::vector<std::pair<int, std::string>> landed;
        std::string landedBytes;
        commands::WriteOptions options = writeOpts(tmp.path);
        options.journal.bodiesChanging = [&](const std::vector<commands::SlotChange>& changes) {
            heard = changes;
            cardWhenHeard = commands::readMemory(volume);
        };
        options.journal.audioWritten = [&](int slot, const std::string& name,
                                           std::string_view landedView) {
            landed.emplace_back(slot, name);
            landedBytes = std::string(landedView);
        };
        commands::restore(volume, 7, recorded, options);

        CHECK_EQ(heard.size(), 1u);
        if (heard.size() == 1u) {
            CHECK_EQ(heard.front().slot, 7);
            CHECK(heard.front().before == rc0::slotBody(present, 7));
            CHECK(heard.front().after == recorded.body);
        }
        CHECK(cardWhenHeard == present);
        CHECK_EQ(landed.size(), 1u);
        if (landed.size() == 1u) {
            CHECK((landed.front() == std::pair<int, std::string> { 7, "take.wav" }));
            CHECK(landedBytes == recorded.take->bytes);
        }
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

        commands::TrimOptions options { .write = writeOpts(tmp.path, "trim-1") };
        const auto result = commands::trim(volume, 4, 1000000, 4000000, options);

        // Same filename, canonical header, exactly the requested 3M frames.
        const std::string after = commands::readFileBytes(volume::wavDir(volume, 4) / "take.wav");
        CHECK_EQ(after.size(), kCanonicalHeader + 3000000u * kFrameBytes);
        CHECK_EQ(static_cast<unsigned char>(after[kCanonicalHeader]), 0xab);                          // old frame 1000000
        CHECK_EQ(static_cast<unsigned char>(after[kCanonicalHeader + 2999999u * kFrameBytes]), 0xcd);           // old frame 3999999
        CHECK_EQ(result.frames, 3000000);

        // The original is in the archive, byte-identical — the undo.
        CHECK(commands::readFileBytes(keptTake(tmp.path, options.write, 4, result.archivedOriginal))
              == originalBytes);

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
                       { .write = { .opId = "qa4",
                                    .skipBackup = true,
                                    .archive = commands::trashFolder(tmp.path / "trash", "qa4") } });

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
        const commands::TrimOptions options { .write = writeOpts(tmp.path, "trim-2") };

        CHECK_THROWS(commands::trim(volume, 4, 500, 100, options), "bad frame range");
        CHECK_THROWS(commands::trim(volume, 5, 0, 1000, options), "no audio to trim");
        commands::TrimOptions noArchive { .write = withoutArchive(writeOpts(tmp.path, "trim-3")) };
        CHECK_THROWS(commands::trim(volume, 4, 0, 1323000, noArchive), "needs an archive");

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
        commands::swap(volume, 3, 7, writeOpts(tmp.path, "op-2"));
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
        commands::swap(volume, 15, 12, writeOpts(tmp.path, "op-2"));
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
        CHECK_THROWS(commands::swap(volume, 2, 4, writeOpts(tmp.path, "op-2")),
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
                                    { .force = true,
                                      .write = { .opId = "fi-push",
                                                 .skipBackup = true,
                                                 .archive = commands::trashFolder(
                                                     tmp.path / "trash", "fi-push") } }),
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
    // archive and the slot holds the slice — recoverable, honestly reported.
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
                                    { .write = { .opId = "fi-trim",
                                                 .skipBackup = true,
                                                 .archive = commands::trashFolder(
                                                     tmp.path / "trash", "fi-trim") } }),
                     "cannot write");
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_all,
                        fs::perm_options::replace);

        CHECK(commands::readFileBytes(tmp.path / "trash" / "fi-trim" / "004_1" / "take.wav")
              == original);
        CHECK(commands::readFileBytes(volume::memoryPath(volume, 1)) == m1);
    }

    // clear with an unwritable MEMORY1: the audio left the slot but its
    // archive copy landed first — the take survives the failed command.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 6, "gone.wav");
        const std::string original =
            commands::readFileBytes(volume::wavDir(volume, 6) / "gone.wav");

        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_read,
                        fs::perm_options::replace);
        CHECK_THROWS(commands::clear(volume, { 6 },
                                     { .write = { .opId = "fi-clear",
                                                  .skipBackup = true,
                                                  .archive = commands::trashFolder(
                                                      tmp.path / "trash", "fi-clear") } }),
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
            { .write = writeOpts(tmp.path, "fold-1") });

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

        // The stereo original is in the archive, byte-identical — the undo.
        CHECK(commands::readFileBytes(
                  tmp.path / "trash" / "fold-1" / volume::slotDirName(6) / result.archivedOriginal)
              == originalBytes);

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
        const commands::DownmixOptions options { .write = writeOpts(tmp.path, "fold-2") };

        CHECK_THROWS(commands::downmixToMono(volume, 9, options), "no audio to fold");
        CHECK_THROWS(commands::downmixToMono(volume, 3, options), "32-bit float");
        CHECK_THROWS(commands::downmixToMono(volume, 4, options), "already folded to both outputs");
        // The refusal is per placement: silence is already "on OUTPUT A" too,
        // but a foldable take is not, and must not be refused.
        CHECK_THROWS(commands::downmixToMono(
                         volume, 4,
                         { .placement = wav::Placement::OutputAOnly,
                           .write = writeOpts(tmp.path, "fold-2b") }),
                     "already folded to OUTPUT A only");
        // A fold with nowhere to put the original must not touch the audio.
        CHECK_THROWS(commands::downmixToMono(volume, 2,
                                             { .write = withoutArchive(writeOpts(tmp.path)) }),
                     "needs an archive");
        CHECK_THROWS(commands::downmixToMono(volume, 2, { .write = {} }), "needs an archive");

        CHECK(volumeBytes(volume) == before);
    }

    // --- downmix: the chosen jack is what reaches the card ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const int frames = 256;
        putUncancellingStereoFloatWav(volume, 7, "take.wav", frames);

        commands::downmixToMono(volume, 7,
                                { .placement = wav::Placement::OutputBOnly,
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
            { .write = writeOpts(tmp.path, "fold-3") });

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

    // --- normalize: the gain lands, the take stays recoverable (issue #53) ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const int frames = 44100;
        putSineFloatWav(volume, 6, "take.wav", frames, -28.0);
        const std::string originalBytes =
            commands::readFileBytes(volume::wavDir(volume, 6) / "take.wav");
        const std::string bodyBefore = rc0::slotBody(commands::readMemory(volume), 6);

        std::vector<double> ticks; // the overlay's current-file bar (issue #61)
        const auto result = commands::normalize(
            volume, 6,
            { .targetLufs = -18.0,
              .write = writeOpts(tmp.path, "norm-1"),
              .progress = [&ticks](double v) { ticks.push_back(v); } });

        // Progress covers the whole command — the read and the measure lead
        // (the source must be in hand before anything), the write phases
        // follow, and it ends at exactly done — monotonically.
        CHECK(!ticks.empty());
        CHECK(ticks.front() <= 0.5);
        bool monotonic = true;
        for (std::size_t i = 1; i < ticks.size(); ++i)
            monotonic = monotonic && ticks[i] >= ticks[i - 1];
        CHECK(monotonic);
        CHECK(std::abs(ticks.back() - 1.0) <= 1.0e-12);

        CHECK(result.applied);
        CHECK(!result.cappedByPeak);
        CHECK(std::abs(result.measuredLufs - (-28.0)) <= 0.1);
        CHECK(std::abs(result.gainDb - 10.0) <= 0.15);

        // The OUTPUT is the proof: the rewritten take reads -18 on the same
        // meter, same frame count, canonical float32, same filename.
        const std::string after = commands::readFileBytes(volume::wavDir(volume, 6) / "take.wav");
        const auto afterView =
            wav::BytesView(reinterpret_cast<const unsigned char*>(after.data()), after.size());
        const wav::Info info = wav::readWavInfo(afterView);
        CHECK_EQ(info.frames, frames);
        CHECK_EQ(info.format(), std::string("float32"));
        const auto reading = wav::measureLoudness(afterView);
        CHECK(reading.integratedLufs.has_value());
        CHECK(std::abs(*reading.integratedLufs - (-18.0)) <= 0.1);

        // The original is in the archive, byte-identical — the undo.
        CHECK(commands::readFileBytes(
                  tmp.path / "trash" / "norm-1" / volume::slotDirName(6) / result.archivedOriginal)
              == originalBytes);

        // A gain moves no frame: the config's whole length-and-tempo story is
        // untouched, like the fold's.
        const std::string bodyAfter = rc0::slotBody(commands::readMemory(volume), 6);
        CHECK_EQ(bodyAfter, bodyBefore);
    }

    // --- normalize: the no-write answers, and refusals that change nothing ---

    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putSineFloatWav(volume, 6, "attarget.wav", 44100, -18.0);
        putSineFloatWav(volume, 5, "peaky.wav", 44100, -25.0, 0.95f);
        putSineFloatWav(volume, 4, "faint.wav", 44100, -100.0); // under the -70 gate
        putSineFloatWav(volume, 2, "short.wav", 4410, -20.0);   // 100 ms < one block
        putWav(volume, 3, "pcm.wav", { .frames = 128 });        // pcm16 — not the pedal's own
        putSineFloatWav(volume, 7, "damaged.wav", 44100, -23.0, 1.0e20f); // one impossible sample
        const auto before = volumeBytes(volume);
        const commands::NormalizeOptions options { .targetLufs = -18.0,
                                                   .write = writeOpts(tmp.path, "norm-2") };

        // Already at target: an answer, not an error — and not a write.
        const auto atTarget = commands::normalize(volume, 6, options);
        CHECK(!atTarget.applied);
        CHECK(!atTarget.cappedByPeak);
        CHECK(std::abs(atTarget.measuredLufs - (-18.0)) <= 0.1);
        CHECK(std::abs(atTarget.gainDb) < 1.0e-12); // not a tiny write, an exact non-write

        // A boost fully swallowed by the -1 dB ceiling: the peak is already
        // there, nothing to give — an answer too, and still not a write.
        const auto swallowed = commands::normalize(volume, 5, options);
        CHECK(!swallowed.applied);
        CHECK(swallowed.cappedByPeak);
        CHECK(std::abs(swallowed.gainDb) < 1.0e-12);

        // A no-write answer stops at the end of the measure phase (0.45) —
        // the overlay's file bar must not claim work that never ran.
        std::vector<double> ticks;
        (void) commands::normalize(volume, 6,
                                   { .targetLufs = -18.0,
                                     .write = writeOpts(tmp.path, "norm-3"),
                                     .progress = [&ticks](double v) { ticks.push_back(v); } });
        CHECK(!ticks.empty());
        double top = 0.0;
        for (const double v : ticks)
            top = std::max(top, v);
        CHECK(top <= 0.45 + 1.0e-9);

        // The player asked to normalize THIS slot; no gain does what they
        // asked — these are errors, each naming its reason.
        CHECK_THROWS(commands::normalize(volume, 9, options), "no audio to normalize");
        CHECK_THROWS(commands::normalize(volume, 4, options), "silent or shorter");
        CHECK_THROWS(commands::normalize(volume, 2, options), "silent or shorter");
        CHECK_THROWS(commands::normalize(volume, 3, options), "32-bit float");
        // Garbage is refused BEFORE any gain is computed from it: the "loudness"
        // of a 1e20 sample would bake in hundreds of dB and silence the take.
        CHECK_THROWS(commands::normalize(volume, 7, options), "impossible sample");
        CHECK_THROWS(commands::normalize(
                         volume, 6, { .targetLufs = -18.0,
                                      .write = withoutArchive(writeOpts(tmp.path)) }),
                     "needs an archive");
        // A default-constructed target (0.0) is a bug wearing a number.
        CHECK_THROWS(commands::normalize(volume, 6, { .write = writeOpts(tmp.path) }),
                     "between -70 and -1");

        // Every answer and every refusal above left the volume byte-identical.
        CHECK(volumeBytes(volume) == before);
        CHECK(!fs::exists(tmp.path / "trash")); // and no archive copy was spent
    }

    // --- a real card: a mutation changes what it exists to write, and no other byte ---
    //
    // fixtures/rc5-card.RC0 is a MEMORY1.RC0 off an RC-5 (fw 1.10): 41 loops in
    // 99 memories, every byte as the pedal wrote it. Each mutation below runs
    // on a volume holding that card, and what it did is read back as the field
    // changes of the one memory it was given: the set must be exactly the
    // fields the command exists to write, each in the section that owns it —
    // the loop's facts in TRACK1, the tempo and loop length in MASTER — with
    // every other line of the memory, and every other memory, reproduced byte
    // for byte. The numbers are the card's own: memory 11 holds a 10888139-
    // frame loop at 87.0 BPM, 90 bars, one-shot off; memory 42 is factory-
    // empty. The expected values follow from the format, not from the code:
    // bars = round(beats / 4) at the tempo, Measure = MeasLen + 7, and on an
    // upload the pedal's boot indexing (golden.json) — the largest power-of-two
    // bar count whose tempo stays at or below 160 BPM.
    {
        TempDir tmp;
        const fs::path volume = makePedalFromCard(tmp.path);
        const std::string card = commands::readMemory(volume);
        const auto memory = [&volume] { return commands::readMemory(volume); };

        // setOneShot: TRACK1's <One>, on and off again — and the card's own
        // document is back, to the byte.
        commands::setOneShot(volume, { 11 }, true, writeOpts(tmp.path));
        CHECK_EQ(slotChangesOnCard(card, memory(), 11), "TRACK1.One 0->1");
        const std::string oneShotOn = memory();
        commands::setOneShot(volume, { 11 }, false, writeOpts(tmp.path));
        CHECK_EQ(slotChangesOnCard(oneShotOn, memory(), 11), "TRACK1.One 1->0");
        CHECK(rc0::splitFile(memory()).document == rc0::splitFile(card).document);

        // setTempo on the loop: 120.0 BPM over 10888139 frames is 246.90 s,
        // 493.8 beats, 123 bars — Tempo in MASTER, RecTmp and the bars in
        // TRACK1.
        const std::string beforeTempo = memory();
        commands::setTempo(volume, 11, 1200, writeOpts(tmp.path));
        CHECK_EQ(slotChangesOnCard(beforeTempo, memory(), 11),
                 "TRACK1.Measure 97->130, TRACK1.MeasLen 90->123, TRACK1.RecTmp 870->1200, "
                 "MASTER.Tempo 870->1200");

        // setTempo on the factory-empty memory: the two tempo fields and no
        // bars — there is no loop to derive them from.
        const std::string beforeEmptyTempo = memory();
        commands::setTempo(volume, 42, 905, writeOpts(tmp.path));
        CHECK_EQ(slotChangesOnCard(beforeEmptyTempo, memory(), 42),
                 "TRACK1.RecTmp 1200->905, MASTER.Tempo 1200->905");

        // push into the empty memory: a 30 s take, one-shot. Boot indexing
        // gives 16 bars — 64 beats over 30 s is 128.0 BPM, 32 bars would be
        // 256.0 — so TRACK1 takes the loop's facts and MASTER the tempo and
        // the loop length.
        {
            const fs::path source = tmp.path / "take.wav";
            const auto take = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 1323000 });
            commands::writeFileBytes(source, bytesOf(take));
            const std::string beforePush = memory();
            commands::push(volume, source, 42, { .oneShot = true, .write = writeOpts(tmp.path) });
            CHECK_EQ(slotChangesOnCard(beforePush, memory(), 42),
                     "TRACK1.One 0->1, TRACK1.Measure 1->23, TRACK1.MeasLen 0->16, "
                     "TRACK1.RecTmp 905->1280, TRACK1.WavStat 0->1, TRACK1.WavLen 0->1323000, "
                     "MASTER.Tempo 905->1280, MASTER.LpLen 0->16");
        }

        // trim the loop in memory 11 to 10 s. The card came without its audio,
        // so a 20 s take stands in: the config is what trim reads (the kept
        // 120.0 BPM), the file is what it cuts. 10 s at 120.0 BPM is 20 beats,
        // 5 bars: the length fields follow in TRACK1, the loop length in
        // MASTER, and the tempo fields stay.
        putWav(volume, 11, "011_1.WAV", { .tag = 3, .bits = 32, .frames = 882000 });
        const std::string beforeTrim = memory();
        commands::trim(volume, 11, 0, 441000, { .write = writeOpts(tmp.path) });
        const std::string afterTrim = memory();
        CHECK_EQ(slotChangesOnCard(beforeTrim, afterTrim, 11),
                 "TRACK1.Measure 130->12, TRACK1.MeasLen 123->5, TRACK1.WavLen 10888139->441000, "
                 "MASTER.LpLen 128->5");

        // restore: the trimmed state recorded, a tempo change on top (90.5 BPM
        // over 10 s is 15.1 beats, 4 bars), and the state put back — exactly
        // the reverse of the tempo change, the take included.
        const commands::SlotState trimmed {
            rc0::slotBody(afterTrim, 11),
            commands::Take { "011_1.WAV",
                             commands::readFileBytes(volume::wavDir(volume, 11) / "011_1.WAV") }
        };
        commands::setTempo(volume, 11, 905, writeOpts(tmp.path));
        const std::string beforeRestore = memory();
        CHECK_EQ(slotChangesOnCard(afterTrim, beforeRestore, 11),
                 "TRACK1.Measure 12->11, TRACK1.MeasLen 5->4, TRACK1.RecTmp 1200->905, "
                 "MASTER.Tempo 1200->905");
        commands::restore(volume, 11, trimmed, writeOpts(tmp.path));
        CHECK_EQ(slotChangesOnCard(beforeRestore, memory(), 11),
                 "TRACK1.Measure 11->12, TRACK1.MeasLen 4->5, TRACK1.RecTmp 905->1200, "
                 "MASTER.Tempo 905->1200");
        CHECK(commands::readFileBytes(volume::wavDir(volume, 11) / "011_1.WAV")
              == trimmed.take->bytes);
    }

    // --- the section is the address: a same-named tag elsewhere is not the field ---
    //
    // Tag names are not unique across a memory's sections — <Level> is
    // MASTER's and RHYTHM's — and the two-track family carries every TRACK
    // field twice. So a mutation addresses the section that owns its field,
    // never the tag alone. A section the mutations do not own, carrying every
    // tag they write, is neither read nor written by any of them: the loop's
    // facts are TRACK1's, the tempo and loop length MASTER's, and the foreign
    // section comes out byte for byte as it went in. Looked up by tag alone,
    // each of these fields is found twice and the whole slot is refused.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const std::string other = "<OTHER>\n\t<One>1</One>\n\t<Tempo>1</Tempo>\n"
                                  "\t<RecTmp>1</RecTmp>\n\t<WavStat>1</WavStat>\n"
                                  "\t<WavLen>1</WavLen>\n\t<MeasLen>1</MeasLen>\n"
                                  "\t<Measure>1</Measure>\n\t<LpLen>1</LpLen>\n</OTHER>\n";
        for (const int fileNo : { 1, 2 }) {
            const std::string text = commands::readFileBytes(volume::memoryPath(volume, fileNo));
            std::string body = rc0::slotBody(text, 5);
            body.insert(body.find("<RHYTHM>"), other);
            commands::writeFileBytes(volume::memoryPath(volume, fileNo),
                                     rc0::replaceSlotBody(text, 5, body));
        }
        const auto memory = [&volume] { return commands::readMemory(volume); };
        // The field-change view sees OTHER as a section of its own: a write
        // into it would read as OTHER.<tag>.

        std::string before = memory();
        commands::setOneShot(volume, { 5 }, true, writeOpts(tmp.path));
        CHECK_EQ(slotChangesOnCard(before, memory(), 5), "TRACK1.One 0->1");

        before = memory();
        commands::setTempo(volume, 5, 905, writeOpts(tmp.path));
        CHECK_EQ(slotChangesOnCard(before, memory(), 5),
                 "TRACK1.RecTmp 1200->905, MASTER.Tempo 1200->905");

        // push: a 30 s take, 16 bars at 128.0 BPM (see the card block above).
        const fs::path source = tmp.path / "take.wav";
        commands::writeFileBytes(
            source, bytesOf(testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 1323000 })));
        before = memory();
        commands::push(volume, source, 5, { .write = writeOpts(tmp.path) });
        const std::string pushed = memory();
        CHECK_EQ(slotChangesOnCard(before, pushed, 5),
                 "TRACK1.Measure 0->23, TRACK1.MeasLen 0->16, TRACK1.RecTmp 905->1280, "
                 "TRACK1.WavStat 0->1, TRACK1.WavLen 0->1323000, MASTER.Tempo 905->1280, "
                 "MASTER.LpLen 0->16");
        const std::string takeName = volume::listSlotWavs(volume, 5).front();
        const commands::SlotState pushedState {
            rc0::slotBody(pushed, 5),
            commands::Take { takeName,
                             commands::readFileBytes(volume::wavDir(volume, 5) / takeName) }
        };

        // trim to 10 s at the kept 128.0 BPM: 21.3 beats, 5 bars.
        commands::trim(volume, 5, 0, 441000, { .write = writeOpts(tmp.path) });
        CHECK_EQ(slotChangesOnCard(pushed, memory(), 5),
                 "TRACK1.Measure 23->12, TRACK1.MeasLen 16->5, TRACK1.WavLen 1323000->441000, "
                 "MASTER.LpLen 16->5");

        // restore reads the state's WavStat and WavLen from TRACK1 as well —
        // the foreign section's pair would not match the take it brings.
        before = memory();
        commands::restore(volume, 5, pushedState, writeOpts(tmp.path));
        CHECK_EQ(slotChangesOnCard(before, memory(), 5),
                 "TRACK1.Measure 12->23, TRACK1.MeasLen 5->16, TRACK1.WavLen 441000->1323000, "
                 "MASTER.LpLen 5->16");

        // And through it all, the foreign section is the bytes it was.
        const std::string body = rc0::slotBody(memory(), 5);
        CHECK_EQ(body.substr(body.find("<OTHER>"), other.size()), other);
    }

    return testkit::summary("commands");
}
