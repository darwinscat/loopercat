// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Command implementations. Orchestration only — the invariants live in
// rc0/params/wav/volume. Every mutation follows the same discipline:
// back up, edit MEMORY1's content, write it to BOTH memory files with their
// own trailer markers, verify by re-reading, sweep AppleDouble junk.
//
// The core stays clock- and home-directory-free: callers supply the backup/
// trash roots and the timestamp string (the app derives them from its
// settings location and wall clock; tests pin them).
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

// --- reading ---

// A specific bank, pinned — the doctor and the tests look at each in turn.
inline std::string readMemory(const fs::path& volume, int fileNo)
{
    const std::string text = readFileBytes(volume::memoryPath(volume, fileNo));
    rc0::assertMemoryFile(text);
    return text;
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
inline std::string readMemory(const fs::path& volume)
{
    std::map<int, std::string> texts;
    std::map<int, std::uint32_t> generations;
    for (const int fileNo : { 1, 2 }) {
        try {
            std::string text = readMemory(volume, fileNo);
            if (const auto marker = rc0::tailMarker(text))
                generations[fileNo] = *marker;
            texts[fileNo] = std::move(text);
        } catch (const Error&) {
            // this bank cannot vote
        }
    }
    if (texts.empty())
        return readMemory(volume, 1); // no bank readable: surface MEMORY1's error
    if (generations.size() == 2) {
        const bool secondNewer =
            static_cast<std::int32_t>(generations[2] - generations[1]) > 0;
        return texts[secondNewer ? 2 : 1];
    }
    if (generations.size() == 1)
        return texts[generations.begin()->first]; // a counted bank beats a trailer-less one
    return texts.contains(1) ? texts[1] : texts[2];
}

// --- the write discipline ---

struct BackupResult {
    fs::path dest;
    std::vector<std::string> copied;
};

// Copy every non-junk file from ROLAND/DATA into <backupRoot>/<stamp>/.
inline BackupResult backup(const fs::path& volume, const fs::path& backupRoot,
                           const std::string& stamp)
{
    if (backupRoot.empty() || stamp.empty())
        throw Error("backup requires a destination root and a timestamp");
    BackupResult result;
    result.dest = backupRoot / stamp;
    std::error_code ec;
    fs::create_directories(result.dest, ec);
    if (ec)
        throw Error("cannot create backup directory " + result.dest.string());
    for (fs::directory_iterator it(volume::dataDir(volume), ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (volume::isJunkName(name) || it->is_directory())
            continue;
        copyContent(it->path(), result.dest / name);
        result.copied.push_back(name);
    }
    if (result.copied.empty())
        throw Error("backup copied nothing from " + volume::dataDir(volume).string());
    return result;
}

struct WriteOptions {
    fs::path backupRoot;    // where pre-write backups land; empty ONLY with skipBackup
    std::string stamp;      // timestamp label for backup/trash directories
    bool skipBackup = false;
};

struct WriteResult {
    std::optional<BackupResult> backedUp;
    std::vector<fs::path> swept;
    std::vector<fs::path> sweepFailed; // junk still on the volume — a warning, the write succeeded
};

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
inline WriteResult writeMemoryPair(const fs::path& volume, std::string_view text,
                                   const WriteOptions& options)
{
    WriteResult result;
    if (!options.skipBackup)
        result.backedUp = backup(volume, options.backupRoot, options.stamp);
    std::uint32_t base = 0x37; // one below the factory pair: a fresh volume lands on 0x38/0x39
    for (const int fileNo : { 1, 2 }) {
        try {
            if (const auto marker =
                    rc0::tailMarker(readFileBytes(volume::memoryPath(volume, fileNo))))
                base = std::max(base, *marker);
        } catch (const Error&) {
            // unreadable bank: no generation to continue from
        }
    }
    for (const int fileNo : { 1, 2 }) {
        const std::string withTail =
            rc0::setTailGeneration(text, base + static_cast<std::uint32_t>(fileNo));
        const fs::path path = volume::memoryPath(volume, fileNo);
        writeFileBytes(path, withTail);
        if (readFileBytes(path) != withTail)
            throw Error("verification failed: MEMORY" + std::to_string(fileNo)
                        + ".RC0 read back differently");
    }
    volume::SweepResult sweep = volume::sweepJunk(volume);
    result.swept = std::move(sweep.removed);
    result.sweepFailed = std::move(sweep.failed);
    return result;
}

// --- mutations ---

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
        text = rc0::replaceSlotBody(text, slot, rc0::setField(body, "One", on ? 1 : 0));
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
    body = rc0::setField(body, "Tempo", tempoTenths);
    body = rc0::setField(body, "RecTmp", tempoTenths);
    if (rc0::field(body, "WavStat") == 1) {
        const long long bars = barsFromTempo(tempoTenths, rc0::field(body, "WavLen"));
        body = rc0::setField(body, "MeasLen", bars);
        body = rc0::setField(body, "Measure", bars + params::kMeasureFieldOffset);
    }
    return writeMemoryPair(volume, rc0::replaceSlotBody(text, slot, body), options);
}

// --- push ---

struct PushOptions {
    std::optional<std::string> name; // also rename the slot
    bool oneShot = false;
    bool writeConfig = true;         // false = drop the file only, let the pedal index it on boot
    bool force = false;              // replace existing slot audio (it moves to trashRoot first)
    fs::path trashRoot;              // REQUIRED with force on an occupied slot: the replaced
                                     // audio lands here — it is never deleted outright
    WriteOptions write;
};

struct PushResult {
    wav::Info info;
    fs::path dest;
    bool configured;
    std::optional<params::SlotParams> slotParams;
    std::vector<fs::path> trashed;   // where the replaced audio went (force only)
    std::optional<WriteResult> written;
};

// Upload a wav into a slot: canonicalize (what the pedal's boot indexer would
// do anyway — handing it a pre-normalized file means it never touches the
// upload), then validate EVERYTHING — the audio AND the slot's full new
// config document — before the first write: a failed push must leave the
// pedal exactly as it was, and a slot whose config cannot be edited must
// fail while the volume is still untouched, not after the audio landed.
// Replacing occupied audio moves the old wav into the trash root first,
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
        body = rc0::setField(body, "WavStat", 1);
        body = rc0::setField(body, "WavLen", info.frames);
        body = rc0::setField(body, "MeasLen", slotParams->measures);
        body = rc0::setField(body, "Measure", slotParams->measureField());
        body = rc0::setField(body, "RecTmp", slotParams->tempoTenths);
        body = rc0::setField(body, "Tempo", slotParams->tempoTenths);
        body = rc0::setField(body, "LpLen", slotParams->measures);
        if (options.oneShot)
            body = rc0::setField(body, "One", 1);
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
    if (!existing.empty() && (options.trashRoot.empty() || options.write.stamp.empty()))
        throw Error("replacing slot " + std::to_string(slot)
                    + " requires a trash root and a timestamp — the current audio moves to the"
                      " trash, it is never deleted outright");

    // All checks passed — the writes begin. The replaced audio's safety net
    // comes first: copy into the trash, remove from the slot only after the
    // copy landed.
    const fs::path dir = volume::wavDir(volume, slot);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        throw Error("cannot create " + dir.string());
    PushResult result { info, dir / wavPath.filename(), false, slotParams, {}, std::nullopt };
    for (const auto& old : existing) {
        const fs::path trashDir =
            options.trashRoot / options.write.stamp / volume::slotDirName(slot);
        fs::create_directories(trashDir, ec);
        if (ec)
            throw Error("cannot create " + trashDir.string());
        copyContent(dir / old, trashDir / old);
        result.trashed.push_back(trashDir / old);
        if (!fs::remove(dir / old, ec) || ec)
            throw Error("cannot remove " + (dir / old).string());
    }
    writeFileBytes(result.dest,
                   std::string_view(reinterpret_cast<const char*>(wavBytes.data()), wavBytes.size()));

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
    fs::path trashRoot; // REQUIRED: the original wav lands here first (the undo)
    WriteOptions write;
};

struct TrimResult {
    fs::path trashedOriginal;
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
    if (options.trashRoot.empty() || options.write.stamp.empty())
        throw Error("trim requires a trash root and a timestamp");

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
    const long long tempoTenths = rc0::field(body, "Tempo");
    if (tempoTenths < kTempoTenthsMin || tempoTenths > kTempoTenthsMax)
        throw Error("slot " + std::to_string(slot) + " carries tempo "
                    + std::to_string(tempoTenths) + " tenths, outside the pedal's 40.0-300.0 BPM"
                      " range — not trimming a slot with a broken config");
    const long long bars = barsFromTempo(tempoTenths, info.frames);
    body = rc0::setField(body, "WavLen", info.frames);
    body = rc0::setField(body, "MeasLen", bars);
    body = rc0::setField(body, "Measure", bars + params::kMeasureFieldOffset);
    body = rc0::setField(body, "LpLen", bars);
    const std::string newDocument = rc0::replaceSlotBody(memoryText, slot, body);

    // All checks passed — the writes begin. Trash copy first: the original
    // must be safe before anything replaces it.
    const fs::path trashDir = options.trashRoot / options.write.stamp / volume::slotDirName(slot);
    std::error_code ec;
    fs::create_directories(trashDir, ec);
    if (ec)
        throw Error("cannot create " + trashDir.string());
    TrimResult result { trashDir / files.front(), info.frames,
                        { static_cast<int>(bars), static_cast<int>(tempoTenths) }, {} };
    writeFileBytes(result.trashedOriginal, raw);

    writeFileBytes(source,
                   std::string_view(reinterpret_cast<const char*>(slice.data()), slice.size()));

    result.written = writeMemoryPair(volume, newDocument, options.write);
    return result;
}

// --- downmix ---

struct DownmixOptions {
    fs::path trashRoot; // REQUIRED: the stereo original lands here first (the undo)
    wav::Placement placement = wav::Placement::BothOutputs;
    WriteOptions write;
};

struct DownmixResult {
    fs::path trashedOriginal;
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
    if (options.trashRoot.empty() || options.write.stamp.empty())
        throw Error("downmix requires a trash root and a timestamp");

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

    // All checks passed — the writes begin. Trash copy first: the original
    // must be safe before anything replaces it.
    const fs::path trashDir = options.trashRoot / options.write.stamp / volume::slotDirName(slot);
    std::error_code ec;
    fs::create_directories(trashDir, ec);
    if (ec)
        throw Error("cannot create " + trashDir.string());
    DownmixResult result { trashDir / files.front(), info.frames, {} };
    writeFileBytes(result.trashedOriginal, raw);

    writeFileBytes(source,
                   std::string_view(reinterpret_cast<const char*>(folded.data()), folded.size()));

    result.written = writeMemoryPair(volume, memoryText, options.write);
    return result;
}

// --- normalize ---

struct NormalizeOptions {
    fs::path trashRoot; // REQUIRED: the original lands here first (the undo)
    double targetLufs = 0.0; // REQUIRED: 0 is not a target and is refused as one
    WriteOptions write;
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
    fs::path trashedOriginal;   // empty when nothing was written
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
    if (options.trashRoot.empty() || options.write.stamp.empty())
        throw Error("normalize requires a trash root and a timestamp");
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

    // All checks passed — the writes begin. Trash copy first: the original
    // must be safe before anything replaces it.
    const fs::path trashDir = options.trashRoot / options.write.stamp / volume::slotDirName(slot);
    std::error_code ec;
    fs::create_directories(trashDir, ec);
    if (ec)
        throw Error("cannot create " + trashDir.string());
    result.trashedOriginal = trashDir / files.front();
    writeFileBytes(result.trashedOriginal, raw, segment(0.60, 0.78));

    writeFileBytes(source,
                   std::string_view(reinterpret_cast<const char*>(rewritten.data()),
                                    rewritten.size()),
                   segment(0.78, 0.96));

    result.applied = true;
    result.gainDb = gainDb;
    result.written = writeMemoryPair(volume, memoryText, options.write);
    report(1.0); // the config pair is part of the job; done means all of it
    return result;
}

// --- clear ---

struct ClearOptions {
    bool keepName = false;
    fs::path trashRoot;     // REQUIRED unless trash=false: cleared audio lands here first
    bool trash = true;
    WriteOptions write;
};

struct ClearResult {
    std::vector<fs::path> trashed;
    std::vector<fs::path> deleted;
    WriteResult written;
};

// Clear slots back to factory state (what MEMORY CLEAR on the device does).
// The wav is never deleted outright: it is moved into the trash root on the
// computer first — the only command that removes audio, so it gets a net.
inline ClearResult clear(const fs::path& volume, const std::vector<int>& slots,
                         const ClearOptions& options)
{
    if (options.trash && options.trashRoot.empty())
        throw Error("clear requires a trash root (or trash=false)");
    if (options.trash && options.write.stamp.empty())
        throw Error("clear requires a timestamp for the trash directory");
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
                const fs::path destDir =
                    options.trashRoot / options.write.stamp / volume::slotDirName(plan.slot);
                std::error_code ec;
                fs::create_directories(destDir, ec);
                if (ec)
                    throw Error("cannot create " + destDir.string());
                copyContent(src, destDir / file);
                result.trashed.push_back(destDir / file);
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
        backedUp = backup(volume, options.backupRoot, options.stamp);
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
