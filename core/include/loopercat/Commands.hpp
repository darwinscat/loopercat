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
#include "Error.hpp"
#include "MemorySettings.hpp"
#include "Params.hpp"
#include "Rc0.hpp"
#include "Volume.hpp"
#include "Wav.hpp"

#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace loopercat::commands {

namespace fs = std::filesystem;

// --- raw byte file I/O (a memory file is bytes, never text) ---

inline std::string readFileBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error("cannot read " + path.string());
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof())
        throw Error("cannot read " + path.string());
    return bytes;
}

inline void writeFileBytes(const fs::path& path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw Error("cannot write " + path.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
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

inline std::string readMemory(const fs::path& volume, int fileNo = 1)
{
    const std::string text = readFileBytes(volume::memoryPath(volume, fileNo));
    rc0::assertMemoryFile(text);
    return text;
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
};

// The mutation tail shared by every command: back up, write the SAME document
// to both memory files, verify each byte-for-byte by re-reading, sweep junk.
//
// Trailers continue the pedal's own write-generation count instead of
// rewinding it: both banks get the document stamped base+1 (MEMORY1) and
// base+2 (MEMORY2) past the highest generation found on the volume — the
// same shape as the factory pair 8/9. An unreadable bank cannot vote (the
// write is what heals it); with neither readable the count restarts at the
// factory pair. Plain max here: the wrap past 0xff is unobserved hardware
// territory.
inline WriteResult writeMemoryPair(const fs::path& volume, std::string_view text,
                                   const WriteOptions& options)
{
    WriteResult result;
    if (!options.skipBackup)
        result.backedUp = backup(volume, options.backupRoot, options.stamp);
    unsigned char base = 0x37; // one below '8': a fresh volume lands on the factory pair
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
            rc0::setTailGeneration(text, static_cast<unsigned char>(base + fileNo));
        const fs::path path = volume::memoryPath(volume, fileNo);
        writeFileBytes(path, withTail);
        if (readFileBytes(path) != withTail)
            throw Error("verification failed: MEMORY" + std::to_string(fileNo)
                        + ".RC0 read back differently");
    }
    result.swept = volume::sweepJunk(volume);
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

// The pedal's supported tempo range, tenths of BPM (RC-5 display: 40.0–300.0).
inline constexpr long long kTempoTenthsMin = 400;
inline constexpr long long kTempoTenthsMax = 3000;

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
        const long long frames = rc0::field(body, "WavLen");
        const double seconds = static_cast<double>(frames) / wav::kSampleRate;
        const double beats = static_cast<double>(tempoTenths) / 10.0 * seconds / 60.0;
        const long long bars = std::max(1LL, static_cast<long long>(std::llround(beats / 4.0)));
        body = rc0::setField(body, "MeasLen", bars);
        body = rc0::setField(body, "Measure", bars + 7);
    }
    return writeMemoryPair(volume, rc0::replaceSlotBody(text, slot, body), options);
}

// Apply Tier-1 memory settings edits to one slot (issue #30). The field
// model and validation live in memsettings; this adds only the write
// discipline.
inline WriteResult setMemorySettings(const fs::path& volume, int slot,
                                     const memsettings::Edits& edits,
                                     const WriteOptions& options)
{
    const std::string text = readMemory(volume);
    const std::string body = rc0::slotBody(text, slot);
    return writeMemoryPair(volume,
                           rc0::replaceSlotBody(text, slot, memsettings::apply(body, edits)),
                           options);
}

// --- push ---

struct PushOptions {
    std::optional<std::string> name; // also rename the slot
    bool oneShot = false;
    bool writeConfig = true;         // false = drop the file only, let the pedal index it on boot
    bool force = false;              // replace existing slot audio
    WriteOptions write;
};

struct PushResult {
    wav::Info info;
    fs::path dest;
    bool configured;
    std::optional<params::SlotParams> slotParams;
    std::optional<WriteResult> written;
};

// Upload a wav into a slot: canonicalize (what the pedal's boot indexer would
// do anyway — handing it a pre-normalized file means it never touches the
// upload), validate EVERYTHING before the first write (a failed push must
// leave the pedal exactly as it was), then write audio + config.
inline PushResult push(const fs::path& volume, const fs::path& wavPath, int slot,
                       const PushOptions& options)
{
    const std::string raw = readFileBytes(wavPath);
    const wav::Bytes wavBytes = wav::canonicalize(
        wav::BytesView(reinterpret_cast<const unsigned char*>(raw.data()), raw.size()));
    const wav::Info info = wav::assertUploadable(wav::readWavInfo(wavBytes));

    if (options.name)
        rc0::encodeName(*options.name); // validates; the value is applied below
    std::optional<params::SlotParams> slotParams;
    if (options.writeConfig)
        slotParams = params::computeSlotParams(info.frames);
    const std::string memoryText = options.writeConfig ? readMemory(volume) : std::string();

    const std::vector<std::string> existing = volume::listSlotWavs(volume, slot);
    if (!existing.empty() && !options.force) {
        std::string files;
        for (const auto& f : existing)
            files += (files.empty() ? "" : ", ") + f;
        throw Error("slot " + std::to_string(slot) + " already has audio (" + files
                    + "); pass force to replace");
    }

    const fs::path dir = volume::wavDir(volume, slot);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        throw Error("cannot create " + dir.string());
    for (const auto& old : existing)
        if (!fs::remove(dir / old, ec) || ec)
            throw Error("cannot remove " + (dir / old).string());
    PushResult result { info, dir / wavPath.filename(), false, slotParams, std::nullopt };
    writeFileBytes(result.dest,
                   std::string_view(reinterpret_cast<const char*>(wavBytes.data()), wavBytes.size()));

    if (!options.writeConfig) {
        volume::sweepJunk(volume);
        return result;
    }

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
    result.written = writeMemoryPair(volume, rc0::replaceSlotBody(memoryText, slot, body),
                                     options.write);
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
    params::SlotParams slotParams;
    WriteResult written;
};

// Cut a slot's loop down to [startFrame, endFrame): the slice is rewritten in
// canonical form under the same on-pedal filename, the slot's boot-index
// config is recomputed for the new length, and the ORIGINAL file moves to the
// trash root first — trim is the one command that rewrites audio, so the
// pre-trim take is always recoverable. Everything validates before the first
// write: a failed trim leaves the volume exactly as it was.
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
    const params::SlotParams slotParams = params::computeSlotParams(info.frames);
    const std::string memoryText = readMemory(volume);

    // All checks passed — the writes begin. Trash copy first: the original
    // must be safe before anything replaces it.
    const fs::path trashDir = options.trashRoot / options.write.stamp / volume::slotDirName(slot);
    std::error_code ec;
    fs::create_directories(trashDir, ec);
    if (ec)
        throw Error("cannot create " + trashDir.string());
    TrimResult result { trashDir / files.front(), info.frames, slotParams, {} };
    writeFileBytes(result.trashedOriginal, raw);

    writeFileBytes(source,
                   std::string_view(reinterpret_cast<const char*>(slice.data()), slice.size()));

    std::string body = rc0::slotBody(memoryText, slot);
    body = rc0::setField(body, "WavLen", info.frames);
    body = rc0::setField(body, "MeasLen", slotParams.measures);
    body = rc0::setField(body, "Measure", slotParams.measureField());
    body = rc0::setField(body, "RecTmp", slotParams.tempoTenths);
    body = rc0::setField(body, "Tempo", slotParams.tempoTenths);
    body = rc0::setField(body, "LpLen", slotParams.measures);
    result.written = writeMemoryPair(volume, rc0::replaceSlotBody(memoryText, slot, body),
                                     options.write);
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

    const auto hexByte = [](unsigned char b) {
        constexpr char digits[] = "0123456789abcdef";
        return std::string { '0', 'x', digits[b >> 4], digits[b & 0xf] };
    };

    // Trailer bytes are write-generation counters, not fixed markers — any
    // value is legal; only a structurally broken trailer is boot-fatal.
    std::map<int, std::string> texts;
    std::map<int, unsigned char> generations;
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
        // the factory ships 8/9, a pedal-side save leaves e.g. 0x3a/0x39).
        const auto delta = static_cast<unsigned char>(generations[1] - generations[2]);
        if (delta != 0 && delta != 1 && delta != 0xff)
            findings.push_back({ Level::warn,
                                 "MEMORY write generations " + hexByte(generations[1]) + " / "
                                     + hexByte(generations[2])
                                     + " are more than one step apart \xe2\x80\x94 unexpected "
                                       "state, consider a Backup before writing" });
    }

    if (texts.contains(1) && texts.contains(2)
        && rc0::splitFile(texts[1]).document != rc0::splitFile(texts[2]).document) {
        std::string generationNote;
        if (generations.contains(1) && generations.contains(2))
            generationNote = " (write generations " + hexByte(generations[1]) + " / "
                           + hexByte(generations[2]) + ")";
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
                std::string files;
                for (const auto& f : wavs)
                    files += (files.empty() ? "" : ", ") + f;
                findings.push_back({ Level::info,
                                     "slot " + std::to_string(slot.slot) + " has " + files
                                         + " not indexed yet — reboot the pedal to index it" });
            }
        }
    }
    return findings;
}

} // namespace loopercat::commands
