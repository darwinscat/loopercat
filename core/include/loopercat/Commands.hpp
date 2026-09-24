// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Command implementations. Orchestration only — the invariants live in
// rc0/params/wav/volume. Every mutation follows the same discipline:
// back up, edit MEMORY1's content, write it to BOTH memory files with their
// own trailer markers, verify by re-reading, sweep AppleDouble junk.
//
// The core stays clock- and home-directory-free: callers supply the backup
// root, the operation id and the archive a replaced take goes to (the app
// derives them from its settings location and its history store, and mints
// one id per operation; tests pin all three).
//
// Behavior source: rc5cat lib/commands.js, byte-for-byte where it matters.

#pragma once

#include "Catalog.hpp"
#include "Downmix.hpp"
#include "Error.hpp"
#include "Loudness.hpp"
#include "Normalize.hpp"
#include "Params.hpp"
#include "Rc0.hpp"
#include "SystemFile.hpp"
#include "Volume.hpp"
#include "Wav.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace loopercat::commands {

namespace fs = std::filesystem;

// --- raw byte file I/O (a memory file is bytes, never text) ---

// `progress` (optional, both functions) hears 0..1 as chunks move: on a USB
// card the file I/O is where a big take's wall time actually goes, and a
// progress bar that skips it stands still through the longest part of the
// job (issue #61, seen on hardware with a 500 MB take).
inline std::string readFileBytes(const fs::path& path,
                                 const std::function<void(double)>& progress = {})
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error("cannot read " + path.string());
    if (!progress) {
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.good() && !in.eof())
            throw Error("cannot read " + path.string());
        return bytes;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 0 || !in)
        throw Error("cannot read " + path.string());
    std::string bytes(static_cast<std::size_t>(size), '\0');
    constexpr std::streamoff kChunk = 4 << 20;
    std::streamoff done = 0;
    while (done < size) {
        const std::streamoff n = std::min(kChunk, size - done);
        in.read(bytes.data() + done, n);
        if (!in)
            throw Error("cannot read " + path.string());
        done += n;
        progress(static_cast<double>(done) / static_cast<double>(size));
    }
    return bytes;
}

inline void writeFileBytes(const fs::path& path, std::string_view bytes,
                           const std::function<void(double)>& progress = {})
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw Error("cannot write " + path.string());
    if (!progress) {
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    } else {
        // Chunked so the observer hears the bytes go; a chunk the OS still
        // holds in cache reports as written — the flush below is the tail.
        constexpr std::size_t kChunk = std::size_t { 4 } << 20;
        std::size_t done = 0;
        while (done < bytes.size()) {
            const std::size_t n = std::min(kChunk, bytes.size() - done);
            out.write(bytes.data() + done, static_cast<std::streamsize>(n));
            if (!out.good())
                throw Error("cannot write " + path.string());
            done += n;
            progress(static_cast<double>(done) / static_cast<double>(bytes.size()));
        }
    }
    out.flush();
    if (!out.good())
        throw Error("cannot write " + path.string());
}

// Copy file content only — never metadata, so no AppleDouble sidecar payload.
inline void copyContent(const fs::path& src, const fs::path& dst)
{
    writeFileBytes(dst, readFileBytes(src));
}

// The archive never overwrites itself. Operation ids name its directories, and
// two operations that share one would otherwise replace each other's
// pre-state in silence: the identity used to be a wall clock at one-second
// resolution, and a bulk normalize of slots 32, 33 and 34 left ONE backup
// directory for the three operations. Minting a unique id is the caller's job;
// making a reused one loud is this one's.
inline void requireFreshArchivePath(const fs::path& dest)
{
    std::error_code ec;
    if (fs::exists(dest, ec))
        throw Error("the archive already holds " + dest.string()
                    + " — two operations are running under one id");
}

// --- reading ---

// Validating a bank is the one thing that differs between the two kinds: a
// memory file must carry its 99 <mem> entries, a settings file its single
// <sys>, and each is refused where the other is expected (SystemFile.hpp).
inline void assertBank(volume::Bank bank, std::string_view text)
{
    switch (bank) {
    case volume::Bank::memory: rc0::assertMemoryFile(text); return;
    case volume::Bank::system: sysfile::assertSystemFile(text); return;
    }
    throw Error("unknown bank");
}

// A specific bank, pinned — the doctor and the tests look at each in turn.
inline std::string readBank(const fs::path& volume, volume::Bank bank, int fileNo)
{
    const std::string text = readFileBytes(volume::bankPath(volume, bank, fileNo));
    assertBank(bank, text);
    return text;
}

inline std::string readMemory(const fs::path& volume, int fileNo)
{
    return readBank(volume, volume::Bank::memory, fileNo);
}

// THE database: the bank the write counters name as newest. The pedal writes
// a save into one bank and reconciles the pair only at its next boot
// (hardware-observed 2026-08-10: a fresh WRITE landed in MEMORY2 at
// generation 237 while MEMORY1 sat stale at 236) — always reading MEMORY1
// showed a just-saved loop as absent, and a mutation started from the stale
// document would clobber the fresh save on both banks. Serial arithmetic
// picks the newer counter across the wrap; an unreadable or trailer-less
// bank simply loses the vote, and with neither readable the MEMORY1 error
// propagates as before.
inline std::string readBank(const fs::path& volume, volume::Bank bank)
{
    std::map<int, std::string> texts;
    std::map<int, std::uint32_t> generations;
    for (const int fileNo : { 1, 2 }) {
        try {
            std::string text = readBank(volume, bank, fileNo);
            if (const auto marker = rc0::tailMarker(text))
                generations[fileNo] = *marker;
            texts[fileNo] = std::move(text);
        } catch (const Error&) {
            // this bank cannot vote
        }
    }
    if (texts.empty())
        return readBank(volume, bank, 1); // no bank readable: surface the first one's error
    if (generations.size() == 2) {
        const bool secondNewer =
            static_cast<std::int32_t>(generations[2] - generations[1]) > 0;
        return texts[secondNewer ? 2 : 1];
    }
    if (generations.size() == 1)
        return texts[generations.begin()->first]; // a counted bank beats a trailer-less one
    return texts.contains(1) ? texts[1] : texts[2];
}

inline std::string readMemory(const fs::path& volume)
{
    return readBank(volume, volume::Bank::memory);
}

// The pedal's own settings, from the bank its counter names as newest — the
// same rule, because it is the same pair mechanism (SystemFile.hpp).
inline std::string readSystem(const fs::path& volume)
{
    return readBank(volume, volume::Bank::system);
}

// --- the write discipline ---

struct BackupResult {
    fs::path dest;
    std::vector<std::string> copied;
};

// Copy every non-junk file from ROLAND/DATA into <backupRoot>/<opId>/.
inline BackupResult backup(const fs::path& volume, const fs::path& backupRoot,
                           const std::string& opId)
{
    if (backupRoot.empty() || opId.empty())
        throw Error("backup requires a destination root and an operation id");
    BackupResult result;
    result.dest = backupRoot / opId;
    std::error_code ec;
    fs::create_directories(result.dest, ec);
    if (ec)
        throw Error("cannot create backup directory " + result.dest.string());
    for (fs::directory_iterator it(volume::dataDir(volume), ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (volume::isJunkName(name) || it->is_directory())
            continue;
        requireFreshArchivePath(result.dest / name);
        copyContent(it->path(), result.dest / name);
        result.copied.push_back(name);
    }
    if (result.copied.empty())
        throw Error("backup copied nothing from " + volume::dataDir(volume).string());
    return result;
}

// Where a command puts a take it is about to replace or remove. It is called
// BEFORE the file on the card changes, with the whole take, and the card
// changes only after it returns: an archive that throws aborts the command
// with the take still in its slot. What "kept" means — a folder, a database —
// is the caller's business; the order is the core's guarantee.
using Archive = std::function<void(int slot, const std::string& fileName, std::string_view bytes)>;

// What a write does to one slot: its body as the card holds it, and the body
// the write puts in its place. Bodies are the <mem> blocks, byte for byte.
struct SlotChange {
    int slot;
    std::string before;
    std::string after;
};

// What a command tells the history while it runs. Each resource is reported
// before IT changes: a take goes to the archive before its file does, and
// the bodies are reported before the memory pair is written — until then
// the old bodies are still on the card, whatever happened to the audio. Both
// are observers, nothing in the write depends on them, except that a throw
// from either stops the command where it stands: a history that cannot keep
// up must not be outrun.
struct Journal {
    // Just before the memory pair is written. Empty when the write only
    // restamps the pair — downmix and normalize change audio alone.
    std::function<void(const std::vector<SlotChange>&)> bodiesChanging;
    // After a take has landed on the card, with its bytes: the post-state's
    // audio, so the history never has to read it back over USB.
    std::function<void(int slot, const std::string& fileName, std::string_view bytes)> audioWritten;
};

struct WriteOptions {
    fs::path backupRoot;    // where pre-write backups land; empty ONLY with skipBackup
    // The identity of this operation, and the name of its backup directory.
    // It must be unique per operation — a wall clock is not, and reusing one
    // costs a pre-state (see requireFreshArchivePath). Any readability inside
    // it is decoration: the core only compares it.
    std::string opId;
    bool skipBackup = false;
    // REQUIRED by every command that replaces or removes audio (push over an
    // occupied slot, trim, downmix, normalize, clear): a take is never
    // destroyed without having been handed here first.
    Archive archive;
    Journal journal;
};

// The archive as it has always looked on disk: <root>/<opId>/<NNN_1>/<file>.
// It stays through the move to the history store (#72): until the History tab
// (#50) opens the database, this folder is the only door a player has to a
// take the app replaced.
inline Archive trashFolder(const fs::path& root, const std::string& opId)
{
    if (root.empty() || opId.empty())
        throw Error("the trash folder needs a root and an operation id");
    return [operationDir = root / opId](int slot, const std::string& fileName,
                                        std::string_view bytes) {
        const fs::path dir = operationDir / volume::slotDirName(slot);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
            throw Error("cannot create " + dir.string());
        requireFreshArchivePath(dir / fileName);
        writeFileBytes(dir / fileName, bytes);
    };
}

// The one entry point every command uses, so the refusal reads the same
// wherever a take is about to go.
inline void archiveTake(const WriteOptions& options, const char* command, int slot,
                        const std::string& fileName, std::string_view bytes)
{
    if (!options.archive)
        throw Error(std::string(command) + " on slot " + std::to_string(slot)
                    + " needs an archive — a take is never destroyed without one");
    options.archive(slot, fileName, bytes);
}

struct WriteResult {
    std::optional<BackupResult> backedUp;
    std::vector<fs::path> swept;
    std::vector<fs::path> sweepFailed; // junk still on the volume — a warning, the write succeeded
};

// Every slot whose body differs between the document on the card and the one
// about to replace it — and a refusal if they differ ANYWHERE else. The
// history records a write as the slot bodies it changed, so a change outside
// them is one no row could describe; it is stopped here rather than recorded
// as less than it was. The trailer is the one exception by design: the write
// stamps it (see writeMemoryPair), and it belongs to no slot.
inline std::vector<SlotChange> slotChanges(std::string_view current, std::string_view next)
{
    std::vector<SlotChange> changes;
    std::string patched(current);
    for (int slot = 1; slot <= rc0::kSlotCount; ++slot) {
        std::string before = rc0::slotBody(current, slot);
        std::string after = rc0::slotBody(next, slot);
        if (before == after)
            continue;
        patched = rc0::replaceSlotBody(patched, slot, after);
        changes.push_back({ slot, std::move(before), std::move(after) });
    }
    if (rc0::splitFile(patched).document != rc0::splitFile(next).document)
        throw Error("the write changes the memory document outside its slots — the history"
                    " could not describe it");
    return changes;
}

// The mutation tail shared by every command: back up, write the SAME document
// to both memory files, verify each byte-for-byte by re-reading, sweep junk.
// The sweep is best-effort and runs after the pair write has succeeded: a
// locked sidecar lands in sweepFailed, it never turns the completed write
// into a reported failure.
//
// Trailers continue the pedal's own write-generation count instead of
// rewinding it: both banks get the document stamped base+1 (MEMORY1) and
// base+2 (MEMORY2) past the highest generation found on the volume — the
// same shape as the factory pair 0x38/0x39. An unreadable bank cannot vote
// (the write is what heals it); with neither readable the count restarts at
// the factory pair. The counter is a uint32 (field pedals sit far past one
// byte — see Rc0.hpp); plain max and natural wrap at 2^32.
namespace detail {

    // The generation to continue from: the higher counter the volume carries,
    // or no value when neither bank offers one (unreadable, or trailer-less).
    inline std::optional<std::uint32_t> highestGeneration(const fs::path& volume,
                                                          volume::Bank bank)
    {
        std::optional<std::uint32_t> base;
        for (const int fileNo : { 1, 2 }) {
            try {
                if (const auto marker =
                        rc0::tailMarker(readFileBytes(volume::bankPath(volume, bank, fileNo))))
                    base = base ? std::max(*base, *marker) : *marker;
            } catch (const Error&) {
                // unreadable bank: no generation to continue from
            }
        }
        return base;
    }

    // Both banks of one pair, stamped one generation apart and each verified
    // by re-reading. Shared by the memory pair and the settings pair: the
    // trailer mechanism is the card's, not one file kind's.
    inline void writePairStamped(const fs::path& volume, volume::Bank bank,
                                 std::string_view text, std::uint32_t base)
    {
        for (const int fileNo : { 1, 2 }) {
            const std::string withTail =
                rc0::setTailGeneration(text, base + static_cast<std::uint32_t>(fileNo));
            const fs::path path = volume::bankPath(volume, bank, fileNo);
            writeFileBytes(path, withTail);
            if (readFileBytes(path) != withTail)
                throw Error("verification failed: " + std::string(volume::bankStem(bank))
                            + std::to_string(fileNo) + ".RC0 read back differently");
        }
    }

} // namespace detail

inline WriteResult writeMemoryPair(const fs::path& volume, std::string_view text,
                                   const WriteOptions& options)
{
    // Described before anything is written: a write the history could not
    // describe is refused with the volume as it was.
    const std::vector<SlotChange> changes = slotChanges(readMemory(volume), text);
    WriteResult result;
    if (!options.skipBackup)
        result.backedUp = backup(volume, options.backupRoot, options.opId);
    if (options.journal.bodiesChanging)
        options.journal.bodiesChanging(changes);
    // one below the factory pair: a fresh volume lands on 0x38/0x39
    const std::uint32_t base = detail::highestGeneration(volume, volume::Bank::memory)
                                  .value_or(rc0::tailMarkerFor(1) - 1);
    detail::writePairStamped(volume, volume::Bank::memory, text, base);
    volume::SweepResult sweep = volume::sweepJunk(volume);
    result.swept = std::move(sweep.removed);
    result.sweepFailed = std::move(sweep.failed);
    return result;
}

// The settings pair, under the same discipline: validate, back the card up,
// stamp both banks past the highest generation on the volume, verify each by
// re-reading, sweep the sidecars macOS leaves behind.
//
// Two deliberate differences from the memory pair, both because we know less
// here. There is no factory pair to restart from — the RC-5's own SYSTEM
// counters sit wherever that pedal's history left them (0x0524/0x0525 on one
// field unit, 0x21e on another), and inventing a starting point would hand
// the pedal a settings file claiming to be older than the one it wrote. So a
// volume whose settings banks cannot be read is refused rather than healed.
// And there is no journal hook yet: #72's journal speaks in slots, and what a
// settings change should record belongs with the feature that first makes one.
//
// Nothing in the app calls this yet. Writing a settings file to hardware is
// unproven — the pedal has to still boot afterwards — and that experiment
// belongs to the feature that needs it, with a backup at hand.
inline WriteResult writeSystemPair(const fs::path& volume, std::string_view text,
                                   const WriteOptions& options)
{
    sysfile::assertSystemFile(text);
    const auto base = detail::highestGeneration(volume, volume::Bank::system);
    if (!base)
        throw Error("refusing to write settings: neither SYSTEM bank on " + volume.string()
                    + " can be read, so there is no write generation to continue from");
    WriteResult result;
    if (!options.skipBackup)
        result.backedUp = backup(volume, options.backupRoot, options.opId);
    detail::writePairStamped(volume, volume::Bank::system, text, *base);
    volume::SweepResult sweep = volume::sweepJunk(volume);
    result.swept = std::move(sweep.removed);
    result.sweepFailed = std::move(sweep.failed);
    return result;
}

// --- mutations ---
//
// Every field a mutation reads or writes is addressed by the section that
// owns it (rc0::sectionField / rc0::setSectionField): the loop's facts —
// One, WavStat, WavLen, MeasLen, Measure, RecTmp — in TRACK1, the playback
// tempo and loop length — Tempo, LpLen — in MASTER. Tag names are not unique
// across sections, and a memory with a second track carries every TRACK
// field twice: the section is the address, and the one-occurrence rule
// holds inside it.

inline WriteResult rename(const fs::path& volume, int slot, std::string_view name,
                          const WriteOptions& options)
{
    const std::string text = readMemory(volume);
    const std::string body = rc0::slotBody(text, slot);
    return writeMemoryPair(volume, rc0::replaceSlotBody(text, slot, rc0::setName(body, name)),
                           options);
}

inline WriteResult setOneShot(const fs::path& volume, const std::vector<int>& slots, bool on,
                              const WriteOptions& options)
{
    std::string text = readMemory(volume);
    for (const int slot : slots) {
        const std::string body = rc0::slotBody(text, slot);
        text = rc0::replaceSlotBody(
            text, slot, rc0::setSectionField(body, rc0::kSectionTrack1, "One", on ? 1 : 0));
    }
    return writeMemoryPair(volume, text, options);
}

// Play Count-In (#34) across a set of slots. The field surgery lives in
// usecases::countin, which owns the rules about what the feature may touch
// (the count always; the rhythm's State/Pattern only while it is otherwise
// silent). This is the transaction around it: one read, one backup, one
// pair-write. Everything else in the RHYTHM block (kit, beat, level, ...) is
// left untouched either way.
inline WriteResult setCountIn(const fs::path& volume, const std::vector<int>& slots, bool on,
                              const WriteOptions& options)
{
    std::string text = readMemory(volume);
    for (const int slot : slots)
        text = rc0::replaceSlotBody(text, slot,
                                    usecases::countin::apply(rc0::slotBody(text, slot), on));
    return writeMemoryPair(volume, text, options);
}

// The pedal's supported tempo range, tenths of BPM (RC-5 display: 40.0–300.0).
inline constexpr long long kTempoTenthsMin = 400;
inline constexpr long long kTempoTenthsMax = 3000;

// Whole 4/4 bars a true tempo spans over a frame count, minimum one bar.
// Shared by setTempo and trim: both write a bar count that FOLLOWS from the
// tempo in the config, instead of re-running the pedal's power-of-two import
// guess (which would overwrite a user-set tempo — hardware QA 2026-08-01,
// QA-4). Bars assume 4/4 — every observed slot carries RHYTHM.Beat = 2 (4/4);
// other signatures await a decoded Beat enum.
inline long long barsFromTempo(long long tempoTenths, std::int64_t frames)
{
    const double seconds = static_cast<double>(frames) / wav::kSampleRate;
    const double beats = static_cast<double>(tempoTenths) / 10.0 * seconds / 60.0;
    return std::max(1LL, static_cast<long long>(std::llround(beats / params::kBeatsPerMeasure)));
}

// Assign a slot's true tempo. The pedal never analyzes audio — on import it
// assumes a power-of-two bar count (#10 analysis, hardware-verified
// 2026-07-24) — so a loop that isn't 16/32/64/… bars gets a wrong tempo and
// the onboard rhythm drifts against the music. This writes the user's actual
// BPM: Tempo and RecTmp in tenths, and for indexed audio the bar count that
// follows from it — MeasLen = whole bars, Measure = MeasLen + 7 (the UI-enum
// offset). Exact integer beats are not required by the firmware: the pedal's
// own imports land at e.g. 511.91 beats after its rounding to tenths.
// Bars assume 4/4 — every observed slot carries RHYTHM.Beat = 2 (4/4); other
// signatures await a decoded Beat enum.
inline WriteResult setTempo(const fs::path& volume, int slot, long long tempoTenths,
                            const WriteOptions& options)
{
    if (tempoTenths < kTempoTenthsMin || tempoTenths > kTempoTenthsMax)
        throw Error("tempo out of the pedal's 40.0-300.0 BPM range: "
                    + std::to_string(tempoTenths / 10) + "." + std::to_string(tempoTenths % 10));
    std::string text = readMemory(volume);
    std::string body = rc0::slotBody(text, slot);
    body = rc0::setSectionField(body, rc0::kSectionMaster, "Tempo", tempoTenths);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "RecTmp", tempoTenths);
    if (rc0::sectionField(body, rc0::kSectionTrack1, "WavStat") == 1) {
        const long long bars =
            barsFromTempo(tempoTenths, rc0::sectionField(body, rc0::kSectionTrack1, "WavLen"));
        body = rc0::setSectionField(body, rc0::kSectionTrack1, "MeasLen", bars);
        body = rc0::setSectionField(body, rc0::kSectionTrack1, "Measure",
                                    bars + params::kMeasureFieldOffset);
    }
    return writeMemoryPair(volume, rc0::replaceSlotBody(text, slot, body), options);
}

// --- push ---

struct PushOptions {
    std::optional<std::string> name; // also rename the slot
    bool oneShot = false;
    bool writeConfig = true;         // false = drop the file only, let the pedal index it on boot
    bool force = false;              // replace existing slot audio (it goes to write.archive first)
    WriteOptions write;
};

struct PushResult {
    wav::Info info;
    fs::path dest;
    bool configured;
    std::optional<params::SlotParams> slotParams;
    std::vector<std::string> archived; // the replaced takes, by file name (force only)
    std::optional<WriteResult> written;
};

// Upload a wav into a slot: canonicalize (what the pedal's boot indexer would
// do anyway — handing it a pre-normalized file means it never touches the
// upload), then validate EVERYTHING — the audio AND the slot's full new
// config document — before the first write: a failed push must leave the
// pedal exactly as it was, and a slot whose config cannot be edited must
// fail while the volume is still untouched, not after the audio landed.
// Replacing occupied audio hands the old wav to the archive first,
// clear-style — push never destroys a take.
inline PushResult push(const fs::path& volume, const fs::path& wavPath, int slot,
                       const PushOptions& options)
{
    const std::string raw = readFileBytes(wavPath);
    const wav::Bytes wavBytes = wav::canonicalize(
        wav::BytesView(reinterpret_cast<const unsigned char*>(raw.data()), raw.size()));
    const wav::Info info = wav::assertUploadable(wav::readWavInfo(wavBytes));

    if (options.name)
        rc0::encodeName(*options.name); // validates; applied in the document below

    std::optional<params::SlotParams> slotParams;
    std::string newDocument;
    if (options.writeConfig) {
        slotParams = params::computeSlotParams(info.frames);
        const std::string memoryText = readMemory(volume);
        std::string body = rc0::slotBody(memoryText, slot);
        body = rc0::setSectionField(body, rc0::kSectionTrack1, "WavStat", 1);
        body = rc0::setSectionField(body, rc0::kSectionTrack1, "WavLen", info.frames);
        body = rc0::setSectionField(body, rc0::kSectionTrack1, "MeasLen", slotParams->measures);
        body = rc0::setSectionField(body, rc0::kSectionTrack1, "Measure",
                                    slotParams->measureField());
        body = rc0::setSectionField(body, rc0::kSectionTrack1, "RecTmp", slotParams->tempoTenths);
        body = rc0::setSectionField(body, rc0::kSectionMaster, "Tempo", slotParams->tempoTenths);
        body = rc0::setSectionField(body, rc0::kSectionMaster, "LpLen", slotParams->measures);
        if (options.oneShot)
            body = rc0::setSectionField(body, rc0::kSectionTrack1, "One", 1);
        if (options.name)
            body = rc0::setName(body, *options.name);
        newDocument = rc0::replaceSlotBody(memoryText, slot, body);
    }

    const std::vector<std::string> existing = volume::listSlotWavs(volume, slot);
    if (!existing.empty() && !options.force) {
        std::string files;
        for (const auto& f : existing)
            files += (files.empty() ? "" : ", ") + f;
        throw Error("slot " + std::to_string(slot) + " already has audio (" + files
                    + "); pass force to replace");
    }
    if (!existing.empty() && !options.write.archive)
        throw Error("replacing slot " + std::to_string(slot)
                    + " needs an archive — the current audio is kept first, it is never"
                      " deleted outright");

    // All checks passed — the writes begin. The replaced audio's safety net
    // comes first: hand it to the archive, remove it from the slot only after
    // the archive has it.
    const fs::path dir = volume::wavDir(volume, slot);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        throw Error("cannot create " + dir.string());
    PushResult result { info, dir / wavPath.filename(), false, slotParams, {}, std::nullopt };
    for (const auto& old : existing) {
        archiveTake(options.write, "push", slot, old, readFileBytes(dir / old));
        result.archived.push_back(old);
        if (!fs::remove(dir / old, ec) || ec)
            throw Error("cannot remove " + (dir / old).string());
    }
    const std::string_view landed(reinterpret_cast<const char*>(wavBytes.data()), wavBytes.size());
    writeFileBytes(result.dest, landed);
    if (options.write.journal.audioWritten)
        options.write.journal.audioWritten(slot, result.dest.filename().string(), landed);

    if (!options.writeConfig) {
        // Best-effort sweep; survivors keep the volume boot-risky, and
        // doctor() reports each one — this drop-only path has no write
        // report to attach them to.
        volume::sweepJunk(volume);
        return result;
    }

    result.written = writeMemoryPair(volume, newDocument, options.write);
    result.configured = true;
    return result;
}

// --- pull ---

struct PullOptions {
    fs::path dest;          // REQUIRED: where the wavs land
    bool rawNames = false;  // keep on-pedal filenames even when technical
    bool force = false;     // overwrite existing destination files
};

struct PullJob {
    int slot;
    std::string base;    // destination filename
    std::string onPedal; // source filename on the volume
    fs::path src, destFile;
};

// Copy slot audio from the pedal to disk — read-only with respect to the
// pedal. Technical DOS 8.3 names become "NN - Slot Name.wav"; duplicates
// across slots are disambiguated by slot number instead of silently
// overwriting within one run.
inline std::vector<PullJob> pull(const fs::path& volume, const std::vector<int>& slots,
                                 const PullOptions& options)
{
    if (options.dest.empty())
        throw Error("pull requires a destination directory");
    const std::string text = readMemory(volume);
    std::vector<PullJob> jobs;
    for (const int slot : slots) {
        const std::vector<std::string> files = volume::listSlotWavs(volume, slot);
        if (files.empty())
            throw Error("slot " + std::to_string(slot) + " has no audio to pull");
        const std::string name = rc0::decodeName(rc0::slotBody(text, slot));
        const std::string base =
            options.rawNames ? files.front() : wav::pullFileName(slot, name, files.front());
        jobs.push_back({ slot, base, files.front(), volume::wavDir(volume, slot) / files.front(), {} });
    }
    std::map<std::string, int> seen;
    for (const auto& job : jobs)
        ++seen[job.base];
    for (auto& job : jobs) {
        if (seen[job.base] > 1) {
            const std::string n = std::to_string(job.slot);
            job.base = (n.size() < 2 ? "0" + n : n) + " - " + job.onPedal;
        }
        job.destFile = options.dest / job.base;
    }
    for (const auto& job : jobs) {
        std::error_code ec;
        if (fs::exists(job.destFile, ec) && !options.force)
            throw Error(job.destFile.string() + " already exists; pass force to overwrite");
    }
    std::error_code ec;
    fs::create_directories(options.dest, ec);
    if (ec)
        throw Error("cannot create " + options.dest.string());
    for (const auto& job : jobs)
        copyContent(job.src, job.destFile);
    return jobs;
}

// --- trim ---

struct TrimOptions {
    WriteOptions write; // write.archive REQUIRED: the original goes there first (the undo)
};

struct TrimResult {
    std::string archivedOriginal; // the file name the archive was handed
    std::int64_t frames;
    params::SlotParams slotParams; // what the config now carries: kept tempo + derived bars
    WriteResult written;
};

// Cut a slot's loop down to [startFrame, endFrame): the slice is rewritten in
// canonical form under the same on-pedal filename, and the ORIGINAL file
// moves to the trash root first — trim is the one command that rewrites
// audio, so the pre-trim take is always recoverable. The slot's tempo is
// PRESERVED: trim changes length, not speed, so only the length fields and
// the bar count that follows from the kept tempo are rewritten (hardware QA
// 2026-08-01, QA-4: recomputing via the pedal's power-of-two import formula
// overwrote a user-set true tempo). Everything — the slice AND the full new
// config document — validates before the first write: a failed trim leaves
// the volume exactly as it was.
inline TrimResult trim(const fs::path& volume, int slot, std::int64_t startFrame,
                       std::int64_t endFrame, const TrimOptions& options)
{
    if (!options.write.archive)
        throw Error("trim needs an archive — the original is kept first, it is the undo");

    const std::vector<std::string> files = volume::listSlotWavs(volume, slot);
    if (files.empty())
        throw Error("slot " + std::to_string(slot) + " has no audio to trim");
    const fs::path source = volume::wavDir(volume, slot) / files.front();

    const std::string raw = readFileBytes(source);
    const wav::BytesView rawView(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
    const wav::Bytes slice = wav::trimmed(rawView, startFrame, endFrame); // validates the range
    const wav::Info info = wav::readWavInfo(slice);

    const std::string memoryText = readMemory(volume);
    std::string body = rc0::slotBody(memoryText, slot);
    const long long tempoTenths = rc0::sectionField(body, rc0::kSectionMaster, "Tempo");
    if (tempoTenths < kTempoTenthsMin || tempoTenths > kTempoTenthsMax)
        throw Error("slot " + std::to_string(slot) + " carries tempo "
                    + std::to_string(tempoTenths) + " tenths, outside the pedal's 40.0-300.0 BPM"
                      " range — not trimming a slot with a broken config");
    const long long bars = barsFromTempo(tempoTenths, info.frames);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "WavLen", info.frames);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "MeasLen", bars);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "Measure",
                                bars + params::kMeasureFieldOffset);
    body = rc0::setSectionField(body, rc0::kSectionMaster, "LpLen", bars);
    const std::string newDocument = rc0::replaceSlotBody(memoryText, slot, body);

    // All checks passed — the writes begin. The archive first: the original
    // must be safe before anything replaces it.
    TrimResult result { files.front(), info.frames,
                        { static_cast<int>(bars), static_cast<int>(tempoTenths) }, {} };
    archiveTake(options.write, "trim", slot, files.front(), raw);

    const std::string_view landed(reinterpret_cast<const char*>(slice.data()), slice.size());
    writeFileBytes(source, landed);
    if (options.write.journal.audioWritten)
        options.write.journal.audioWritten(slot, files.front(), landed);

    result.written = writeMemoryPair(volume, newDocument, options.write);
    return result;
}

// --- downmix ---

struct DownmixOptions {
    wav::Placement placement = wav::Placement::BothOutputs;
    WriteOptions write; // write.archive REQUIRED: the stereo original goes there first (the undo)
};

struct DownmixResult {
    std::string archivedOriginal; // the file name the archive was handed
    std::int64_t frames;
    WriteResult written;
};

// Fold a slot's loop to mono in place (issue #43) and put the result where
// `placement` says — both jacks, OUTPUT A alone or OUTPUT B alone — under the
// same on-pedal filename, with the ORIGINAL stereo file moved to the trash
// root first. This is the second command that rewrites audio, and it is as
// recoverable as the first.
//
// The config document is rewritten UNCHANGED, on purpose. Folding moves no
// frame, so WavLen, MeasLen, Measure and LpLen all still describe this loop
// exactly and there is nothing to recompute — but the pair write is the
// mutation tail every command shares, and it is what backs the memory up,
// sweeps the sidecars macOS leaves on the volume, and carries the pedal's
// write generation forward for a memory whose audio just changed.
//
// A fold that would not change a single byte is refused rather than performed:
// it would spend a trash copy and a pedal write generation on nothing, so
// saying so is more use than doing it. Note this is per placement — a loop
// already folded across both jacks is a no-op for BothOutputs and a real
// rewrite for OUTPUT B alone.
inline DownmixResult downmixToMono(const fs::path& volume, int slot,
                                   const DownmixOptions& options)
{
    if (!options.write.archive)
        throw Error("downmix needs an archive — the stereo original is kept first, it is the undo");

    const std::vector<std::string> files = volume::listSlotWavs(volume, slot);
    if (files.empty())
        throw Error("slot " + std::to_string(slot) + " has no audio to fold");
    const fs::path source = volume::wavDir(volume, slot) / files.front();

    const std::string raw = readFileBytes(source);
    const wav::BytesView rawView(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
    // validates the format before it answers
    if (wav::foldWouldChangeNothing(rawView, options.placement))
        throw Error("slot " + std::to_string(slot) + " is already folded to "
                    + wav::placementName(options.placement));
    const wav::Bytes folded = wav::downmixedToMono(rawView, options.placement);
    const wav::Info info = wav::readWavInfo(folded);

    // The config has to be readable before the audio is touched: a fold that
    // could not write its memory pair afterwards would leave the volume in a
    // state no undo describes.
    const std::string memoryText = readMemory(volume);

    // All checks passed — the writes begin. The archive first: the original
    // must be safe before anything replaces it.
    DownmixResult result { files.front(), info.frames, {} };
    archiveTake(options.write, "downmix", slot, files.front(), raw);

    const std::string_view landed(reinterpret_cast<const char*>(folded.data()), folded.size());
    writeFileBytes(source, landed);
    if (options.write.journal.audioWritten)
        options.write.journal.audioWritten(slot, files.front(), landed);

    result.written = writeMemoryPair(volume, memoryText, options.write);
    return result;
}

// --- normalize ---

struct NormalizeOptions {
    double targetLufs = 0.0; // REQUIRED: 0 is not a target and is refused as one
    WriteOptions write;      // write.archive REQUIRED: the original goes there first (the undo)
    // Optional observer: hears 0..1 across the whole command — the measure
    // pass as the first half, the rewrite as the second — on the calling
    // thread. The batch overlay's current-file bar (issue #61).
    std::function<void(double)> progress;
};

struct NormalizeResult {
    bool applied = false;       // false: nothing was written — see gainDb/cappedByPeak for why
    bool cappedByPeak = false;  // the boost stopped at the -1 dBTP true-peak ceiling
    double measuredLufs = 0.0;
    double gainDb = 0.0;        // the gain baked in; 0 with applied=false means "already there"
    std::string archivedOriginal; // the file name the archive was handed; empty when nothing was written
    WriteResult written;        // empty when nothing was written
};

// Level a slot's loop to the target loudness in place (issue #53): measure
// per BS.1770 straight from the card's bytes, bake one constant gain into the
// samples under the same on-pedal filename, with the ORIGINAL moved to the
// trash root first — the third command that rewrites audio, as recoverable as
// the other two. A gain moves no frame, so like the fold this rewrites the
// config document unchanged (the pair write is the shared mutation tail:
// backup, sidecar sweep, write generation).
//
// Two outcomes deliberately write NOTHING and say so instead of erroring —
// they are answers, not failures, and a bulk apply must be able to walk over
// them: already within kAlreadyAtTargetLu of the target (nothing audible to
// gain), and a wanted boost fully swallowed by the peak ceiling (the loop
// already peaks at -1 dBTP — there is nothing to give it). An unmeasurable
// slot — silence, or under one gating block — IS an error: the player asked
// to normalize this slot, and no gain would do what they asked.
inline NormalizeResult normalize(const fs::path& volume, int slot,
                                 const NormalizeOptions& options)
{
    if (!options.write.archive)
        throw Error("normalize needs an archive — the original is kept first, it is the undo");
    // 0.0 is what an unset field reads as, and no loudness war ever pushed a
    // target out of this window — outside it is a bug, not a taste.
    if (options.targetLufs >= loudness::kPeakCeilingDb
        || options.targetLufs <= loudness::kAbsoluteGateLufs)
        throw Error("normalize target must sit between -70 and -1 LUFS, got "
                    + std::to_string(options.targetLufs));

    const std::vector<std::string> files = volume::listSlotWavs(volume, slot);
    if (files.empty())
        throw Error("slot " + std::to_string(slot) + " has no audio to normalize");
    const fs::path source = volume::wavDir(volume, slot) / files.front();

    const auto report = [&options](double v) {
        if (options.progress)
            options.progress(v);
    };
    // Phase weights are pragmatic, not measured: on a USB card the three
    // file passes (read, trash copy, write-back) own the wall clock, in RAM
    // the two DSP passes do — these segments keep the bar in honest motion
    // through every phase either way. Past 0.96 is the flush and the pair.
    const auto segment = [&report](double from, double to) {
        return std::function<void(double)>(
            [&report, from, to](double v) { report(from + (to - from) * v); });
    };

    const std::string raw = readFileBytes(source, segment(0.0, 0.30));
    const wav::BytesView rawView(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
    const wav::LoudnessReading reading = wav::measureLoudness( // validates the shape
        rawView, segment(0.30, 0.45));
    // Garbage first: a "loudness" read off non-audio bytes would compute a
    // gain of hundreds of dB and bake it in — that is how a damaged take
    // becomes a silent one. Refuse, and say what to do instead.
    if (reading.wildSamples > 0)
        throw Error("slot " + std::to_string(slot) + " contains "
                    + std::to_string(reading.wildSamples)
                    + " impossible sample value(s) — bytes that are not audio. The take looks "
                      "damaged; re-push it from the original instead of normalizing it");
    if (!reading.integratedLufs.has_value())
        throw Error("slot " + std::to_string(slot)
                    + " is silent or shorter than the 400 ms a loudness measurement needs");

    NormalizeResult result;
    result.measuredLufs = *reading.integratedLufs;
    const double wanted = options.targetLufs - result.measuredLufs;
    if (std::abs(wanted) < loudness::kAlreadyAtTargetLu)
        return result; // already there — applied=false, gainDb=0

    const double gainDb = loudness::normalizeGainDb(result.measuredLufs, options.targetLufs,
                                                    reading.truePeakDb,
                                                    loudness::kPeakCeilingDb);
    result.cappedByPeak = wanted > 0.0 && gainDb + 1.0e-9 < wanted;
    if (std::abs(gainDb) < 1.0e-9)
        return result; // the ceiling ate the whole boost — rewriting would change nothing

    const wav::Bytes rewritten = wav::withGainDb(rawView, gainDb, segment(0.45, 0.60));

    // The config has to be readable before the audio is touched: a rewrite
    // that could not write its memory pair afterwards would leave the volume
    // in a state no undo describes.
    const std::string memoryText = readMemory(volume);

    // All checks passed — the writes begin. The archive first: the original
    // must be safe before anything replaces it.
    result.archivedOriginal = files.front();
    archiveTake(options.write, "normalize", slot, files.front(), raw);
    report(0.78);

    const std::string_view landed(reinterpret_cast<const char*>(rewritten.data()),
                                  rewritten.size());
    writeFileBytes(source, landed, segment(0.78, 0.96));
    if (options.write.journal.audioWritten)
        options.write.journal.audioWritten(slot, files.front(), landed);

    result.applied = true;
    result.gainDb = gainDb;
    result.written = writeMemoryPair(volume, memoryText, options.write);
    report(1.0); // the config pair is part of the job; done means all of it
    return result;
}

// --- clear ---

struct ClearOptions {
    bool keepName = false;
    bool trash = true;      // false: "Delete permanently" — the take is removed unkept
    WriteOptions write;     // write.archive REQUIRED unless trash=false
};

struct ClearResult {
    std::vector<std::string> archived; // file names handed to the archive
    std::vector<fs::path> deleted;     // trash=false: removed without a copy
    WriteResult written;
};

// Clear slots back to factory state (what MEMORY CLEAR on the device does).
// The wav is never deleted outright unless the player said so: it goes to the
// archive first — the only command that removes audio, so it gets a net.
inline ClearResult clear(const fs::path& volume, const std::vector<int>& slots,
                         const ClearOptions& options)
{
    if (options.trash && !options.write.archive)
        throw Error("clear needs an archive (or trash=false)");
    std::string text = readMemory(volume);

    struct Plan {
        int slot;
        std::string body;
        std::vector<std::string> files;
    };
    std::vector<Plan> plans;
    for (const int slot : slots) {
        std::string body = rc0::factorySlotBody(slot);
        if (options.keepName) {
            std::string existing = rc0::decodeName(rc0::slotBody(text, slot));
            while (!existing.empty() && existing.back() == ' ')
                existing.pop_back();
            if (!existing.empty())
                body = rc0::setName(body, existing);
        }
        plans.push_back({ slot, std::move(body), volume::listSlotWavs(volume, slot) });
    }

    ClearResult result;
    for (const auto& plan : plans) {
        for (const auto& file : plan.files) {
            const fs::path src = volume::wavDir(volume, plan.slot) / file;
            if (options.trash) {
                archiveTake(options.write, "clear", plan.slot, file, readFileBytes(src));
                result.archived.push_back(file);
            } else {
                result.deleted.push_back(src);
            }
            std::error_code ec;
            if (!fs::remove(src, ec) || ec)
                throw Error("cannot remove " + src.string());
        }
        text = rc0::replaceSlotBody(text, plan.slot, plan.body);
    }
    result.written = writeMemoryPair(volume, text, options.write);
    return result;
}

// --- restore ---

// The take a slot's folder holds: its on-card file name and the whole file.
// The name is whatever the pedal or a push left there ("005_1.WAV",
// "My Song.wav"); a restored take goes back under that same name.
struct Take {
    std::string fileName;
    std::string bytes;
};

// A slot's state as ONE unit: the <mem> body byte for byte, and the take
// that sat in the slot with it — none for a slot that held no audio. This is
// the pair the history keeps on each side of an operation, and the pair the
// pedal needs to agree with itself: the body's WavLen counts the frames of
// the file next to it.
struct SlotState {
    std::string body;
    std::optional<Take> take;
};

struct RestoreResult {
    std::vector<std::string> archived; // the takes the slot held, by file name — kept first
    std::int64_t frames;               // frames the slot now holds; 0 without a take
    WriteResult written;
};

// A file name a take can carry inside its slot folder: a bare name, not a
// path — a state is data the app stored, and a name with a directory in it
// would write outside the slot — and not one the sweep would delete.
inline bool isTakeFileName(std::string_view name)
{
    if (name.empty() || name == "." || name == "..")
        return false;
    const fs::path asPath { std::string(name) };
    return asPath.filename() == asPath && !volume::isJunkName(name);
}

// Put a recorded slot state back (issue #50). The body is spliced into the
// LIVE memory document through slotBody/replaceSlotBody — the other 98 slots
// belong to the present, an old document is never written wholesale — and
// the take goes back into the slot's folder under its own name, byte for
// byte.
//
// This is not push. push derives Tempo, RecTmp, MeasLen and Measure from the
// frame count, which is right for an import and wrong here: the state
// carries the tempo the player had, and a restore that recomputed it would
// undo the Set tempo they did. Nothing in the body is recomputed and nothing
// in the take is canonicalized: what was recorded is what goes back.
//
// The body and the take are restored TOGETHER or not at all. A body restored
// beside somebody else's audio is how a slot ends up unplayable, and it is
// the natural result of "restore the settings" and "restore the take" being
// two buttons — so there is one primitive, and it refuses a state that
// disagrees with itself. The body's audio fields must describe the take that
// will sit in the slot: WavStat=1 and WavLen equal to its frames with a take,
// WavStat=0 and WavLen=0 without one. WavStat is what the pedal — and
// doctor — read as "this slot holds a take", push sets it together with the
// audio, and a body that claims a take beside an empty folder is exactly the
// slot doctor reports. The take itself must be in the pedal's own format —
// float32, stereo, 44.1 kHz, the gate push uses: no state recorded from a
// card fails it (the pedal records and indexes nothing else), and what the
// app puts on a card is the pedal's format and nothing else (issue #44). A
// state that arrives from elsewhere is refused here rather than discarded by
// the pedal at its next boot.
//
// Discipline as everywhere: everything validates before the first write; the
// takes the slot holds go to the archive before they leave the card; the
// journal hears the body change from the pair write. A restore is an
// operation like any other, so it can be undone in turn. It is not refused
// for changing nothing — the caller holds both states and decides that.
inline RestoreResult restore(const fs::path& volume, int slot, const SlotState& state,
                             const WriteOptions& options)
{
    const fs::path dir = volume::wavDir(volume, slot); // validates the slot range
    const std::string where = "restore of slot " + std::to_string(slot);
    if (state.body.empty())
        throw Error(where + " needs a slot body");
    // A body is what lies between a <mem> opener and its closer, and nothing
    // more: one carrying either would splice as a different document.
    if (state.body.find("<mem id=\"") != std::string::npos
        || state.body.find("</mem>") != std::string::npos)
        throw Error(where + ": the body is not the content of one <mem> block");

    // The frames the slot will hold, counted from the take that will sit there.
    std::int64_t frames = 0;
    if (state.take) {
        if (!isTakeFileName(state.take->fileName))
            throw Error(where + ": \"" + state.take->fileName
                        + "\" is not a file name a take can carry");
        const wav::BytesView takeView(
            reinterpret_cast<const unsigned char*>(state.take->bytes.data()),
            state.take->bytes.size());
        // The shape first (the frames come from it), then the pedal's format.
        frames = wav::assertUploadable(wav::readWavInfo(takeView)).frames;
    }
    // Each field exactly once in the section that owns it, or the body is not one.
    const long long wavStat = rc0::sectionField(state.body, rc0::kSectionTrack1, "WavStat");
    const long long wavLen = rc0::sectionField(state.body, rc0::kSectionTrack1, "WavLen");
    const long long wavStatForTake = state.take ? rc0::kWavStatIndexed : rc0::kWavStatNone;
    if (wavStat != wavStatForTake || wavLen != frames) {
        const std::string held = state.take
            ? "its take \"" + state.take->fileName + "\" holds " + std::to_string(frames) + " frames"
            : "it carries no take";
        throw Error(where + " refused: the body says WavStat=" + std::to_string(wavStat)
                    + ", WavLen=" + std::to_string(wavLen) + " but " + held
                    + " — a body and a take that disagree would leave the slot unplayable, and"
                      " they are only restored together");
    }

    const std::string memoryText = readMemory(volume);
    const std::string newDocument = rc0::replaceSlotBody(memoryText, slot, state.body);

    const std::vector<std::string> existing = volume::listSlotWavs(volume, slot);
    if (!existing.empty() && !options.archive)
        throw Error(where + " needs an archive — the take it replaces is kept first, it is never"
                            " deleted outright");

    // All checks passed — the writes begin. The archive first: whatever the
    // slot holds is handed over whole, and leaves the card only after that.
    RestoreResult result { {}, frames, {} };
    std::error_code ec;
    for (const auto& old : existing) {
        archiveTake(options, "restore", slot, old, readFileBytes(dir / old));
        result.archived.push_back(old);
        if (!fs::remove(dir / old, ec) || ec)
            throw Error("cannot remove " + (dir / old).string());
    }
    if (state.take) {
        fs::create_directories(dir, ec);
        if (ec)
            throw Error("cannot create " + dir.string());
        writeFileBytes(dir / state.take->fileName, state.take->bytes);
        if (options.journal.audioWritten)
            options.journal.audioWritten(slot, state.take->fileName, state.take->bytes);
    }
    result.written = writeMemoryPair(volume, newDocument, options);
    return result;
}

// --- swap ---

// The temporary address used while two occupied slots trade WAVE folders;
// 8.3-safe, so even an interrupted swap leaves a name FAT tooling can show.
inline constexpr const char* kSwapParkName = "SWAP_TMP";

// The audio half of a swap: the two slots' WAVE folders trade addresses by
// rename — metadata-only on FAT, so no audio bytes rewrite and loop length
// does not matter. Either folder may be absent (an empty slot): the swap then
// degenerates into a move. A pedal-recorded take is named after its folder
// (004_1/004_1.WAV); that technical name follows the move, exactly as if the
// pedal had recorded at the new address. Other filenames travel unchanged.
// The whole operation is its own inverse: running it again puts all back.
inline void swapSlotAudio(const fs::path& volume, int slotA, int slotB)
{
    const auto move = [](const fs::path& from, const fs::path& to) {
        std::error_code ec;
        fs::rename(from, to, ec);
        if (ec)
            throw Error("cannot move " + from.string() + " to " + to.string() + ": "
                        + ec.message());
    };
    const fs::path dirA = volume::wavDir(volume, slotA);
    const fs::path dirB = volume::wavDir(volume, slotB);
    std::error_code ec;
    const bool hasA = fs::exists(dirA, ec);
    const bool hasB = fs::exists(dirB, ec);
    if (hasA && hasB) {
        const fs::path parked = dirA.parent_path() / kSwapParkName;
        if (fs::exists(parked, ec))
            throw Error("an interrupted swap left " + parked.string()
                        + " behind; restore that audio to its slot first");
        move(dirA, parked);
        move(dirB, dirA);
        move(parked, dirB);
    } else if (hasA) {
        move(dirA, dirB);
    } else if (hasB) {
        move(dirB, dirA);
    }
    const auto retitle = [&](int fromSlot, int toSlot) {
        const fs::path home = volume::wavDir(volume, toSlot);
        std::error_code existsEc;
        if (fs::exists(home / (volume::slotDirName(fromSlot) + ".WAV"), existsEc))
            move(home / (volume::slotDirName(fromSlot) + ".WAV"),
                 home / (volume::slotDirName(toSlot) + ".WAV"));
    };
    if (hasB)
        retitle(slotB, slotA); // B's take now lives at A's address
    if (hasA)
        retitle(slotA, slotB);
}

// Exchange two memories wholesale (issue #32): the name, every TRACK1/MASTER/
// RHYTHM setting, and the audio all trade places — so "collect the parts of
// one song into consecutive slots" is a few drags. The <mem id> wrappers stay
// put: ids number document POSITIONS, only bodies travel. Discipline order:
// backup first (nothing has moved yet if it fails), then the audio, then the
// memory pair; a failed config write moves the audio back, so a failed swap
// leaves the volume as it was.
inline WriteResult swap(const fs::path& volume, int slotA, int slotB,
                        const WriteOptions& options)
{
    if (slotA == slotB)
        throw Error("swap needs two different slots, got slot " + std::to_string(slotA)
                    + " twice");
    const std::string text = readMemory(volume);
    const std::string bodyA = rc0::slotBody(text, slotA); // validates the range too
    const std::string bodyB = rc0::slotBody(text, slotB);
    const std::string swapped =
        rc0::replaceSlotBody(rc0::replaceSlotBody(text, slotA, bodyB), slotB, bodyA);

    std::optional<BackupResult> backedUp;
    if (!options.skipBackup)
        backedUp = backup(volume, options.backupRoot, options.opId);
    WriteOptions afterBackup = options;
    afterBackup.skipBackup = true; // taken above, before anything moved

    swapSlotAudio(volume, slotA, slotB);
    try {
        WriteResult result = writeMemoryPair(volume, swapped, afterBackup);
        result.backedUp = std::move(backedUp);
        return result;
    } catch (const Error& writeError) {
        try {
            swapSlotAudio(volume, slotA, slotB); // its own inverse: audio back home
        } catch (const Error& undoError) {
            throw Error(std::string(writeError.what())
                        + "; undoing the audio move then failed: " + undoError.what()
                        + " — restore from the backup");
        }
        throw;
    }
}

// --- doctor ---

enum class Level { info, warn, error };

struct Finding {
    Level level;
    std::string message;

    bool operator==(const Finding&) const = default;
};

// Health report: junk on the volume, trailer damage, memory-pair divergence,
// slots whose config and audio disagree.
inline std::vector<Finding> doctor(const fs::path& volume)
{
    std::vector<Finding> findings;
    for (const auto& junk : volume::findJunk(volume))
        findings.push_back({ Level::error,
                             "AppleDouble junk (pedal may refuse to boot): " + junk.string() });

    {
        std::error_code ec;
        const fs::path parked = volume / "ROLAND" / "WAVE" / kSwapParkName;
        if (fs::exists(parked, ec))
            findings.push_back({ Level::error,
                                 "interrupted swap: " + parked.string()
                                     + " holds parked slot audio \xe2\x80\x94 restore it to its "
                                       "slot before the next write" });
    }

    const auto hexGeneration = [](std::uint32_t v) {
        constexpr char digits[] = "0123456789abcdef";
        std::string s;
        do {
            s.insert(s.begin(), digits[v & 0xf]);
            v >>= 4;
        } while (v != 0);
        return "0x" + s;
    };

    // Trailers are write-generation counters, not fixed markers — any value
    // is legal; only a structurally broken trailer is boot-fatal.
    std::map<int, std::string> texts;
    std::map<int, std::uint32_t> generations;
    for (const int fileNo : { 1, 2 }) {
        try {
            texts[fileNo] = readMemory(volume, fileNo);
            if (const auto marker = rc0::tailMarker(texts[fileNo]))
                generations[fileNo] = *marker;
            else
                findings.push_back({ Level::error,
                                     "MEMORY" + std::to_string(fileNo) + ".RC0 trailer is "
                                         "malformed \xe2\x80\x94 causes LOOPER DATA READ ERR" });
        } catch (const Error& e) {
            findings.push_back({ Level::error,
                                 "MEMORY" + std::to_string(fileNo) + ".RC0: " + e.what() });
        }
    }

    if (generations.contains(1) && generations.contains(2)) {
        // A healthy pair sits within one generation of itself (either order:
        // the factory ships 0x38/0x39, a pedal-side save leaves e.g.
        // 0x3a/0x39), and the distance is mod-2^32 — the pair stays healthy
        // across the counter wrap.
        const std::uint32_t delta = generations[1] - generations[2];
        if (delta != 0 && delta != 1 && delta != 0xffffffffu)
            findings.push_back({ Level::warn,
                                 "MEMORY write generations " + hexGeneration(generations[1]) + " / "
                                     + hexGeneration(generations[2])
                                     + " are more than one step apart \xe2\x80\x94 unexpected "
                                       "state, consider a Backup before writing" });
    }

    if (texts.contains(1) && texts.contains(2)
        && rc0::splitFile(texts[1]).document != rc0::splitFile(texts[2]).document) {
        std::string generationNote;
        if (generations.contains(1) && generations.contains(2))
            generationNote = " (write generations " + hexGeneration(generations[1]) + " / "
                           + hexGeneration(generations[2]) + ")";
        findings.push_back({ Level::info,
                             "MEMORY1 and MEMORY2 differ" + generationNote
                                 + " \xe2\x80\x94 normal right after a save on the pedal; "
                                   "a reboot reconciles the pair" });
    }

    if (texts.contains(1)) {
        for (const auto& slot : catalog::listSlots(texts[1])) {
            const auto wavs = volume::listSlotWavs(volume, slot.slot);
            std::string trimmed = slot.name;
            while (!trimmed.empty() && trimmed.back() == ' ')
                trimmed.pop_back();
            if (slot.hasAudio && wavs.empty())
                findings.push_back({ Level::warn,
                                     "slot " + std::to_string(slot.slot) + " (\"" + trimmed
                                         + "\") is configured with audio but its folder is empty" });
            if (!slot.hasAudio && !wavs.empty()) {
                // "Reboot to index it" is only true for the pedal's own
                // format. The RC-5 indexer takes 32-bit float ONLY; a
                // non-float file is DISCARDED from the slot on the next boot,
                // not indexed (hardware: issue #44/#45). So telling a user to
                // reboot a 16-bit take would cost them the take. Read the file
                // and only promise a reboot when the pedal could keep it.
                std::string files;
                bool allFloat32 = true;
                for (const auto& f : wavs) {
                    files += (files.empty() ? "" : ", ") + f;
                    try {
                        const std::string raw =
                            readFileBytes(volume::wavDir(volume, slot.slot) / f);
                        const wav::BytesView view(
                            reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
                        if (wav::readWavInfo(view).format() != "float32")
                            allFloat32 = false;
                    } catch (const Error&) {
                        // Unreadable or not a WAV the pedal understands: it
                        // will not index this either, so it must not be sent
                        // to a hopeful reboot.
                        allFloat32 = false;
                    }
                }
                if (allFloat32)
                    findings.push_back(
                        { Level::info,
                          "slot " + std::to_string(slot.slot) + " has " + files
                              + " not indexed yet — reboot the pedal to index it" });
                else
                    findings.push_back(
                        { Level::warn,
                          "slot " + std::to_string(slot.slot) + " has " + files
                              + ", which the pedal cannot index: it plays 32-bit float only. "
                                "Re-push it through LooperCat to convert it \xe2\x80\x94 a "
                                "reboot would discard it, not index it" });
            }
        }
    }
    return findings;
}

} // namespace loopercat::commands
