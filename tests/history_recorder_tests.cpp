// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The recorder end to end (issue #72): real core commands on a synthetic
// pedal, wired through history::withHistory exactly as the app wires them,
// read back from the store's own tables. What must hold, and what these try
// to break:
//
//   - every mutation leaves one op row that tells the truth about its outcome
//   - a replaced take is in the history byte-exact — even when the command
//     then failed, because that is exactly when it is needed
//   - the history keeps a take before the
//     card changes
//   - a history that cannot open, or a hook for an op that never began, stops
//     the command with the card untouched
//   - an op cut off mid-way reads as interrupted, and its take is still kept
//   - an op names the slot it is about once it has begun, and keeps it when
//     it then writes nothing (#144); maintenance is about no slot
//   - every take row carries the stamp the card's directory entry showed
//     (#141), and what normalize measured is in the history under the bytes
//     it measured — written or not (#140); a reading has nowhere to go while
//     no card is in front of the history
//   - a loudness check files what it read under the hash of the file read,
//     and still answers when the history will not take it
//   - the measure-first step of Normalize (#142) takes the history's reading
//     of exactly these bytes over the meter, measures what it has never seen
//     — a take swapped in under the same name, size and date included — and
//     leaves the card's timeline as it was, while a real normalize adds one row
//   - a stored reading the meter could not have taken, or one the caller
//     cannot use, is measured over once and replaced; a store that cannot be
//     asked is not asked again to file; the trouble named is the one that was

#include "support.hpp"

#include "../app/history/SlotLoudness.h"
#include "../app/history/WriteOptionsFactory.h"
#include "../app/NormalizePlan.h"
#include "../app/OperationsLog.h"

#include <loopercat/Commands.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace loopercat;
using history::HistoryRecorder;
using history::HistoryStore;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-recorder-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

fs::path makePedal(const fs::path& root, const std::string& name = "BOSS RC-5")
{
    const fs::path volume = root / name;
    fs::create_directories(volume / "ROLAND" / "WAVE");
    fs::create_directories(volume::dataDir(volume));
    const std::string text = testkit::syntheticMemoryText();
    for (const int fileNo : { 1, 2 })
        commands::writeFileBytes(volume::memoryPath(volume, fileNo),
                                 rc0::setTailMarker(text, fileNo));
    return volume;
}

void putWav(const fs::path& volume, int slot, const std::string& name, int frames)
{
    const auto bytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = frames });
    fs::create_directories(volume::wavDir(volume, slot));
    commands::writeFileBytes(volume::wavDir(volume, slot) / name,
                             std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                              bytes.size()));
}

// A float32 stereo take of a 997 Hz sine at `dbfs` peak in both channels —
// the tone BS.1770 calibrates on, so a -28 dBFS take reads -28 LUFS — which
// is what a measurement has to read off before anything can be said about it.
void putSineWav(const fs::path& volume, int slot, const std::string& name, int frames, double dbfs)
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
        sample(v);
        sample(v);
    }
    fs::create_directories(volume::wavDir(volume, slot));
    commands::writeFileBytes(volume::wavDir(volume, slot) / name,
                             std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
}

// A file's modification time as the OS reports it, ms since the epoch — the
// oracle the rows are compared against.
std::int64_t fileStamp(const fs::path& file)
{
    return juce::File(juce::String(file.string())).getLastModificationTime().toMilliseconds();
}

std::map<std::string, std::string> volumeBytes(const fs::path& volume)
{
    std::map<std::string, std::string> map;
    for (fs::recursive_directory_iterator it(volume), end; it != end; ++it)
        if (!it->is_directory())
            map[fs::relative(it->path(), volume).string()] = commands::readFileBytes(it->path());
    return map;
}

std::int64_t count(sqlite::Db& db, const std::string& sql)
{
    sqlite::Statement read(db, sql);
    if (!read.step())
        throw Error("count returned no row: " + sql);
    return read.integer(0);
}

std::string text(sqlite::Db& db, const std::string& sql)
{
    sqlite::Statement read(db, sql);
    if (!read.step())
        throw Error("query returned no row: " + sql);
    return read.isNull(0) ? std::string("<null>") : read.text(0);
}

// What the pedal worker does around every recorded job: open, run, close with
// the outcome. The worker's own gate is not under test here.
template <typename Work>
std::string run(HistoryRecorder& rec, const std::string& opId, const std::string& kind,
                const fs::path& volume, Work work)
{
    std::string error;
    try {
        rec.begin(opId, kind, volume);
        work();
    } catch (const std::exception& e) {
        error = e.what();
    }
    rec.finish(opId, error);
    return error;
}

std::int64_t clockAt = 1'000'000;
std::int64_t tick() { return ++clockAt; }

std::shared_ptr<HistoryRecorder> recorderAt(const fs::path& dir)
{
    return std::make_shared<HistoryRecorder>(dir, tick);
}

commands::WriteOptions options(const std::shared_ptr<HistoryRecorder>& rec, const std::string& opId)
{
    return history::withHistory(rec, { .opId = opId });
}

} // namespace

int main()
{
    // Every write uses the app's history wiring. The data home stays free of
    // extra directories while the store records exactly what the card held.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const auto dataHome = tmp.path / "app-data";
        auto rec = recorderAt(dataHome / "history");
        CHECK_EQ(count(rec->store().db(), "SELECT count(*) FROM ops"), 0);
        putWav(volume, 4, "take.wav", 132300);
        const fs::path incoming = tmp.path / "incoming.wav";
        const auto wav = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 176400 });
        commands::writeFileBytes(incoming,
            std::string_view(reinterpret_cast<const char*>(wav.data()), wav.size()));
        for (const std::string kind : { "push", "trim", "rename", "clear" }) {
            const std::string beforeBody = rc0::slotBody(commands::readMemory(volume), 4);
            const auto files = volume::listSlotWavs(volume, 4);
            CHECK_EQ(files.size(), 1u);
            const auto beforeTake = commands::readFileBytes(volume::wavDir(volume, 4) / files.front());
            const auto write = history::makeWriteOptions(rec);
            CHECK_EQ(run(*rec, write.opId, kind, volume, [&] {
                if (kind == "push")
                    commands::push(volume, incoming, 4, { .force = true, .write = write });
                else if (kind == "trim")
                    commands::trim(volume, 4, 0, 88200, { .write = write });
                else if (kind == "rename")
                    commands::rename(volume, 4, "New name", write);
                else
                    commands::clear(volume, { 4 }, { .write = write });
            }), std::string());
            oplog::append(juce::File(dataHome.string()), juce::String(kind));
            CHECK(fs::exists(dataHome / "operations.log"));
            for (const auto& entry : fs::directory_iterator(dataHome))
                CHECK(entry.path().filename() == "history"
                      || entry.path().filename() == "operations.log");
            for (const auto& entry : fs::directory_iterator(dataHome / "history"))
                CHECK(entry.path().filename() == "history.db");
            sqlite::Statement bodies(rec->store().db(),
                "SELECT before_body, after_body FROM slot_changes c JOIN ops o ON o.seq = c.op "
                "WHERE o.id = ?1 AND c.slot = 4");
            bodies.bindText(1, write.opId);
            CHECK(bodies.step());
            CHECK(bodies.blob(0) == beforeBody);
            CHECK(bodies.blob(1) == rc0::slotBody(commands::readMemory(volume), 4));
            if (kind != "rename")
                CHECK(rec->store().takeBytes(HistoryStore::contentHash(beforeTake)) == beforeTake);
        }
        CHECK(volume::listSlotWavs(volume, 4).empty());
        CHECK(rc0::slotBody(commands::readMemory(volume), 4) == rc0::factorySlotBody(4));
    }

    // Existing folders are inert: even unreadable contents do not block a
    // write, and no file in them is changed, imported or removed.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 4, "take.wav", 132300);
        for (const auto* name : { "backups", "trash" }) {
            fs::create_directories(tmp.path / name / "old");
            commands::writeFileBytes(tmp.path / name / "old" / "untouched", "old bytes");
        }
        const auto beforeBackup = volumeBytes(tmp.path / "backups");
        const auto beforeTrash = volumeBytes(tmp.path / "trash");
        fs::permissions(tmp.path / "backups", fs::perms::none);
        fs::permissions(tmp.path / "trash", fs::perms::none);
        auto rec = recorderAt(tmp.path / "history");
        const auto error = run(*rec, "clear", "clear", volume, [&] {
            commands::clear(volume, { 4 }, { .write = options(rec, "clear") });
        });
        fs::permissions(tmp.path / "backups", fs::perms::owner_all);
        fs::permissions(tmp.path / "trash", fs::perms::owner_all);
        CHECK_EQ(error, std::string());
        CHECK(volumeBytes(tmp.path / "backups") == beforeBackup);
        CHECK(volumeBytes(tmp.path / "trash") == beforeTrash);
        CHECK_EQ(count(rec->store().db(), "SELECT count(*) FROM ops WHERE kind <> 'snapshot'"), 1);
        CHECK_EQ(count(rec->store().db(), "SELECT count(*) FROM blobs"), 1);
    }

    // --- trim: the take kept, the landed take named, both bodies, op done ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 4, "take.wav", 132300);
        const std::string original = commands::readFileBytes(volume::wavDir(volume, 4) / "take.wav");
        const std::string bodyBefore = rc0::slotBody(commands::readMemory(volume), 4);
        auto rec = recorderAt(tmp.path / "history");

        const std::string error = run(*rec, "op-trim", "trim", volume, [&] {
            commands::trim(volume, 4, 0, 66150,
                           { .write = options(rec, "op-trim") });
        });
        CHECK_EQ(error, std::string());

        sqlite::Db& db = rec->store().db();
        CHECK_EQ(text(db, "SELECT status FROM ops WHERE id = 'op-trim'"), std::string("done"));
        CHECK_EQ(text(db, "SELECT kind FROM ops WHERE id = 'op-trim'"), std::string("trim"));
        // the original, byte-exact, in the history
        CHECK(rec->store().takeBytes(HistoryStore::contentHash(original)) == original);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE side = 'before' AND slot = 4 "
                           "AND name = 'take.wav' AND track = 1"),
                 1);
        // the take that landed: named and hashed, the hash of what the card holds
        const std::string onCard = commands::readFileBytes(volume::wavDir(volume, 4) / "take.wav");
        sqlite::Statement after(db, "SELECT hash, size FROM slot_audio WHERE side = 'after' AND slot = 4 AND op = (SELECT seq FROM ops WHERE id = 'op-trim')");
        CHECK(after.step());
        CHECK(after.blob(0) == HistoryStore::contentHash(onCard));
        CHECK_EQ(after.integer(1), static_cast<std::int64_t>(onCard.size()));
        // the bodies, both sides, byte-exact
        sqlite::Statement bodies(db, "SELECT before_body, after_body FROM slot_changes WHERE slot = 4 AND op = (SELECT seq FROM ops WHERE id = 'op-trim')");
        CHECK(bodies.step());
        CHECK(bodies.blob(0) == bodyBefore);
        CHECK(bodies.blob(1) == rc0::slotBody(commands::readMemory(volume), 4));
        CHECK(!fs::exists(tmp.path / "trash"));
        CHECK(!fs::exists(tmp.path / "backups"));
    }

    // --- rename and swap: bodies only; first sighting separately keeps the baseline ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 3, "003_1.WAV", 132300);
        auto rec = recorderAt(tmp.path / "history");
        CHECK_EQ(run(*rec, "op-rename", "rename", volume, [&] {
                     commands::rename(volume, 3, "Intro", options(rec, "op-rename"));
                 }),
                 std::string());
        CHECK_EQ(run(*rec, "op-swap", "swap", volume, [&] {
                     commands::swap(volume, 3, 7, options(rec, "op-swap"));
                 }),
                 std::string());
        sqlite::Db& db = rec->store().db();
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_changes sc JOIN ops o ON o.seq = sc.op "
                           "WHERE o.id = 'op-rename'"),
                 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_changes sc JOIN ops o ON o.seq = sc.op "
                           "WHERE o.id = 'op-swap'"),
                 2);
        CHECK_EQ(count(db, "SELECT count(*) FROM blobs"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE status = 'done'"), 2);
        CHECK(!fs::exists(tmp.path / "trash"));
    }

    // --- a command refused before it wrote: failed, with the reason, nothing kept ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 4, "take.wav", 132300);
        auto rec = recorderAt(tmp.path / "history");
        const std::string error = run(*rec, "op-bad", "trim", volume, [&] {
            commands::trim(volume, 4, 500, 100, { .write = options(rec, "op-bad") });
        });
        CHECK(error.find("bad frame range") != std::string::npos);
        sqlite::Db& db = rec->store().db();
        CHECK_EQ(text(db, "SELECT status FROM ops WHERE id = 'op-bad'"), std::string("failed"));
        CHECK(text(db, "SELECT note FROM ops WHERE id = 'op-bad'").find("bad frame range")
              != std::string::npos);
        CHECK_EQ(count(db, "SELECT count(*) FROM blobs"), 0);
    }

    // --- a command that failed AFTER keeping the take: failed, and the take is safe ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 6, "gone.wav", 132300);
        const std::string original = commands::readFileBytes(volume::wavDir(volume, 6) / "gone.wav");
        auto rec = recorderAt(tmp.path / "history");
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_read, fs::perm_options::replace);
        const std::string error = run(*rec, "op-half", "clear", volume, [&] {
            commands::clear(volume, { 6 }, { .write = options(rec, "op-half") });
        });
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_all, fs::perm_options::replace);
        CHECK(!error.empty());
        sqlite::Db& db = rec->store().db();
        CHECK_EQ(text(db, "SELECT status FROM ops WHERE id = 'op-half'"), std::string("failed"));
        CHECK(rec->store().takeBytes(HistoryStore::contentHash(original)) == original);
    }

    // --- a history that cannot open stops every command, card untouched ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 4, "take.wav", 132300);
        commands::writeFileBytes(tmp.path / "history", "a file where the directory should be");
        const auto before = volumeBytes(volume);
        auto rec = recorderAt(tmp.path / "history");
        bool ran = false;
        const std::string first = run(*rec, "op-x", "trim", volume, [&] { ran = true; });
        CHECK(first.find("the history is unavailable") != std::string::npos);
        CHECK(!ran);
        // and it stays refused, with the same reason, without retrying blindly
        const std::string second = run(*rec, "op-y", "rename", volume, [&] { ran = true; });
        CHECK(second.find("the history is unavailable") != std::string::npos);
        CHECK(!ran);
        CHECK(volumeBytes(volume) == before);
    }

    // --- a hook for an op that never began refuses before the card changes ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 4, "take.wav", 132300);
        const auto before = volumeBytes(volume);
        auto rec = recorderAt(tmp.path / "history");
        CHECK_THROWS(commands::trim(volume, 4, 0, 66150,
                                    { .write = options(rec, "op-unbegun") }),
                     "without having begun");
        CHECK(volumeBytes(volume) == before);
        CHECK_THROWS(commands::clear(volume, { 4 }, { .write = options(rec, "op-unbegun") }),
                     "without having begun");
        CHECK(volumeBytes(volume) == before);
        CHECK_THROWS(([&] {
                         rec->begin("op-twice", "rename", volume);
                         rec->begin("op-twice", "rename", volume);
                     }()),
                     "already begun");
    }

    // One operation cannot archive the same slot twice: preserve both takes.
    {
        TempDir tmp;
        const auto volume = makePedal(tmp.path);
        auto rec = recorderAt(tmp.path / "history");
        putWav(volume, 5, "005_1.WAV", 4410);
        const auto first = commands::readFileBytes(volume::wavDir(volume, 5) / "005_1.WAV");
        const auto write = history::makeWriteOptions(rec);
        rec->begin(write.opId, "clear", volume);
        commands::clear(volume, { 5 }, { .write = write });
        putWav(volume, 5, "005_1.WAV", 8820);
        const auto beforeSecond = volumeBytes(volume);
        CHECK_THROWS(commands::clear(volume, { 5 }, { .write = write }), "UNIQUE");
        CHECK(volumeBytes(volume) == beforeSecond);
        CHECK(rec->store().takeBytes(HistoryStore::contentHash(first)) == first);
        CHECK_EQ(count(rec->store().db(), "SELECT count(*) FROM slot_audio WHERE side = 'before'"), 1);
        rec->finish(write.opId, "duplicate clear refused");
    }

    // --- sessions follow the volume ---
    {
        TempDir tmp;
        const fs::path a = makePedal(tmp.path / "a");
        const fs::path b = makePedal(tmp.path / "b", "BOSS RC-5 1");
        auto rec = recorderAt(tmp.path / "history");
        struct Step {
            std::string id;
            fs::path volume;
        };
        for (const Step& step : { Step { "op-1", a }, Step { "op-2", a }, Step { "op-3", b } })
            CHECK_EQ(run(*rec, step.id, "rename", step.volume, [&] {
                         commands::rename(step.volume, 1, step.id,
                                          options(rec, step.id));
                     }),
                     std::string());
        sqlite::Db& db = rec->store().db();
        CHECK_EQ(count(db, "SELECT count(*) FROM sessions"), 2);
        CHECK_EQ(count(db, "SELECT count(*) FROM sessions WHERE disconnected_at IS NULL"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM cards"), 2);
        rec.reset(); // the app exits
        HistoryStore reopened(tmp.path / "history");
        CHECK_EQ(count(reopened.db(), "SELECT count(*) FROM sessions WHERE disconnected_at IS NULL"), 0);
    }

    // --- cut off mid-way: interrupted on the next start, and the take kept ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const std::string take(40000, '\x5a');
        {
            auto rec = recorderAt(tmp.path / "history");
            rec->begin("op-cut", "clear", volume);
            rec->keepAudio("op-cut", 5, "005_1.WAV", take);
            // the app dies here: no finish
        }
        HistoryStore reopened(tmp.path / "history");
        CHECK_EQ(text(reopened.db(), "SELECT status FROM ops WHERE id = 'op-cut'"),
                 std::string("interrupted"));
        CHECK(reopened.takeBytes(HistoryStore::contentHash(take)) == take);
    }

    // --- every slot's timeline reads on its own ---
    //
    // Theory: after an operation, each slot it touched carries a record of
    // what it now holds. Without it a rename leaves no sign that the slot had
    // a take at all, and a swap sends the reader — and Restore — into the
    // other slot's rows for bytes. Hashes are carried only where they are
    // certain; a file the store has never seen gets none rather than a guess.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        auto rec = recorderAt(tmp.path / "history");
        const fs::path source = tmp.path / "incoming.wav";
        const auto sourceBytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 132300 });
        commands::writeFileBytes(source,
                                 std::string_view(reinterpret_cast<const char*>(sourceBytes.data()),
                                                  sourceBytes.size()));
        CHECK_EQ(run(*rec, "op-push", "push", volume, [&] {
                     commands::push(volume, source, 3, { .write = options(rec, "op-push") });
                 }),
                 std::string());
        sqlite::Db& db = rec->store().db();
        const std::string takeHash = text(db, "SELECT hex(hash) FROM slot_audio "
                                              "WHERE slot = 3 AND side = 'after'");
        CHECK(takeHash != "<null>");

        // rename: the take did not move, and the row says so with its hash
        CHECK_EQ(run(*rec, "op-rename", "rename", volume, [&] {
                     commands::rename(volume, 3, "Kept", options(rec, "op-rename"));
                 }),
                 std::string());
        CHECK_EQ(text(db, "SELECT hex(hash) FROM slot_audio a JOIN ops o ON o.seq = a.op "
                          "WHERE o.id = 'op-rename' AND a.slot = 3 AND a.side = 'after'"),
                 takeHash);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio a JOIN ops o ON o.seq = a.op "
                           "WHERE o.id = 'op-rename'"),
                 1);

        // swap: slot 7 took the take, slot 3 holds nothing — each said in its own rows
        CHECK_EQ(run(*rec, "op-swap", "swap", volume, [&] {
                     commands::swap(volume, 3, 7, options(rec, "op-swap"));
                 }),
                 std::string());
        CHECK_EQ(text(db, "SELECT hex(hash) FROM slot_audio a JOIN ops o ON o.seq = a.op "
                          "WHERE o.id = 'op-swap' AND a.slot = 7 AND a.side = 'after'"),
                 takeHash);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio a JOIN ops o ON o.seq = a.op "
                           "WHERE o.id = 'op-swap' AND a.slot = 3"),
                 0);

        // clear: the slot holds nothing afterwards, and no after-row claims it does
        CHECK_EQ(run(*rec, "op-clear", "clear", volume, [&] {
                     commands::clear(volume, { 7 }, { .write = options(rec, "op-clear") });
                 }),
                 std::string());
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio a JOIN ops o ON o.seq = a.op "
                           "WHERE o.id = 'op-clear' AND a.side = 'after'"),
                 0);
    }

    {
        // A take the store has never seen — the pedal recorded it while the
        // app was away — is written down by name and size, with no hash: a
        // row that cannot fetch bytes is honest, a guessed hash is not.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        auto rec = recorderAt(tmp.path / "history");
        const auto first = rec->firstSeen(volume);
        for (int slot = 1; slot <= 99; ++slot) rec->snapshotStep(*first, slot);
        // The pedal records a new take after the original sighting.
        putWav(volume, 12, "012_1.WAV", 132300);
        CHECK_EQ(run(*rec, "op-blind", "rename", volume, [&] {
                     commands::rename(volume, 12, "Stranger", options(rec, "op-blind"));
                 }),
                 std::string());
        sqlite::Db& db = rec->store().db();
        sqlite::Statement row(db, "SELECT name, size, hash IS NULL FROM slot_audio a "
                                  "JOIN ops o ON o.seq = a.op WHERE o.id = 'op-blind'");
        CHECK(row.step());
        CHECK_EQ(row.text(0), std::string("012_1.WAV"));
        CHECK(row.integer(1) > 0);
        CHECK_EQ(row.integer(2), 1); // no hash, and none invented
    }

    {
        // A take re-recorded in place under the same name and size is another
        // file (#141, review). Take A is renamed, so its row carries A's hash
        // with A's stamp; the pedal then records B over it — same name, same
        // size, a later stamp — and the next rename must not pair B's stamp
        // with A's hash. And once a row without a hash stands for the file,
        // no older row with one may speak over it: the stamp is the tell.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 3, "003_1.WAV", 132300);
        const fs::path file = volume::wavDir(volume, 3) / "003_1.WAV";
        const std::string a = commands::readFileBytes(file);
        const std::int64_t stampA = 1'600'000'000'000;
        CHECK(juce::File(juce::String(file.string())).setLastModificationTime(juce::Time(stampA)));
        auto rec = recorderAt(tmp.path / "history");
        sqlite::Db& db = rec->store().db();
        const auto rowOf = [&db](const std::string& opId) {
            sqlite::Statement row(db, "SELECT hex(a.hash), a.modified, hash IS NULL FROM slot_audio a "
                                      "JOIN ops o ON o.seq = a.op WHERE o.id = ?1 AND a.slot = 3 AND a.side = 'after'");
            row.bindText(1, opId);
            if (!row.step())
                throw Error("no after-row for " + opId);
            return std::tuple<std::string, std::int64_t, bool>(row.text(0), row.integer(1), row.integer(2) != 0);
        };
        const auto hex = [](const std::string& raw) {
            static constexpr char digits[] = "0123456789ABCDEF";
            std::string out;
            for (const char c : raw) {
                const auto b = static_cast<unsigned char>(c);
                out += digits[b >> 4];
                out += digits[b & 0xF];
            }
            return out;
        };

        CHECK_EQ(run(*rec, "op-r1", "rename", volume, [&] {
                     commands::rename(volume, 3, "First", options(rec, "op-r1"));
                 }),
                 std::string());
        const auto [hash1, stamp1, null1] = rowOf("op-r1");
        CHECK(!null1);
        CHECK_EQ(hash1, hex(HistoryStore::contentHash(a))); // A, photographed and carried
        CHECK_EQ(stamp1, stampA);

        // the pedal records B over A: same name, same size, other bytes, later stamp
        std::string b = a;
        b.back() = static_cast<char>(b.back() ^ 0x5a);
        CHECK(b != a && b.size() == a.size());
        commands::writeFileBytes(file, b);
        const std::int64_t stampB = stampA + 10'000;
        CHECK(juce::File(juce::String(file.string())).setLastModificationTime(juce::Time(stampB)));

        CHECK_EQ(run(*rec, "op-r2", "rename", volume, [&] {
                     commands::rename(volume, 3, "Second", options(rec, "op-r2"));
                 }),
                 std::string());
        const auto [hash2, stamp2, null2] = rowOf("op-r2");
        CHECK(null2); // B is a stranger: no hash, and never A's
        CHECK_EQ(stamp2, stampB);

        // a third rename: the newest row for the file has no hash and is the
        // last word — A's row two operations back may not speak over it
        CHECK_EQ(run(*rec, "op-r3", "rename", volume, [&] {
                     commands::rename(volume, 3, "Third", options(rec, "op-r3"));
                 }),
                 std::string());
        const auto [hash3, stamp3, null3] = rowOf("op-r3");
        CHECK(null3);
        CHECK_EQ(stamp3, stampB);

        // once the app itself lands a take there, the hash travels again — for that file
        CHECK_EQ(run(*rec, "op-trim", "trim", volume, [&] {
                     commands::trim(volume, 3, 0, 66150, { .write = options(rec, "op-trim") });
                 }),
                 std::string());
        const auto [hashC, stampC, nullC] = rowOf("op-trim");
        CHECK(!nullC);
        CHECK_EQ(hashC, hex(HistoryStore::contentHash(commands::readFileBytes(file))));
        CHECK_EQ(run(*rec, "op-r4", "rename", volume, [&] {
                     commands::rename(volume, 3, "Fourth", options(rec, "op-r4"));
                 }),
                 std::string());
        const auto [hash4, stamp4, null4] = rowOf("op-r4");
        CHECK(!null4);
        CHECK_EQ(hash4, hashC);
        CHECK_EQ(stamp4, stampC);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE slot = 3 AND side = 'after' AND hash = "
                           "(SELECT hash FROM slot_audio a2 JOIN ops o2 ON o2.seq = a2.op WHERE o2.id = 'op-r1' AND a2.slot = 3)"),
                 2); // A's hash: the snapshot's row and the first rename's, nowhere else
    }

    {
        // A history migrated from version 8 or older (every preview tester's)
        // has rows with no stamp. Its slots must stay restorable: the newest
        // such row carries its hash on name and size, as it was written —
        // while a stamped row beside it is still held to its stamp.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 5, "005_1.WAV", 132300);
        putWav(volume, 6, "006_1.WAV", 132300);
        const fs::path fileFive = volume::wavDir(volume, 5) / "005_1.WAV";
        const fs::path fileSix = volume::wavDir(volume, 6) / "006_1.WAV";
        const std::string five = commands::readFileBytes(fileFive);
        auto rec = recorderAt(tmp.path / "history");
        CHECK_EQ(run(*rec, "op-old", "rename", volume, [&] {
                     commands::rename(volume, 5, "Old", options(rec, "op-old"));
                     commands::rename(volume, 6, "Older", options(rec, "op-old"));
                 }),
                 std::string());
        sqlite::Db& db = rec->store().db();
        // the rows as a pre-v9 store left them: no stamp
        db.exec("UPDATE slot_audio SET modified = NULL");
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE modified IS NOT NULL"), 0);
        // slot 5 untouched: the next rename carries the hash on name and size alone
        CHECK_EQ(run(*rec, "op-five", "rename", volume, [&] {
                     commands::rename(volume, 5, "Five", options(rec, "op-five"));
                 }),
                 std::string());
        {
            sqlite::Statement rowFive(db, "SELECT hex(a.hash), a.modified IS NULL FROM slot_audio a JOIN ops o ON o.seq = a.op "
                                          "WHERE o.id = 'op-five' AND a.slot = 5 AND a.side = 'after'");
            CHECK(rowFive.step());
            std::string hexFive;
            for (const char c : HistoryStore::contentHash(five)) {
                static constexpr char digits[] = "0123456789ABCDEF";
                const auto b = static_cast<unsigned char>(c);
                hexFive += digits[b >> 4];
                hexFive += digits[b & 0xF];
            }
            CHECK_EQ(rowFive.text(0), hexFive);
            CHECK_EQ(rowFive.integer(1), 0); // and the new row is stamped
        }
        // slot 6 re-recorded in place, same size: the old row still vouches by
        // name and size — the allowance's price, confined to pre-v9 rows —
        // and from here on the slot's rows are stamped and held to it
        std::string six = commands::readFileBytes(fileSix);
        six.back() = static_cast<char>(six.back() ^ 0x5a);
        commands::writeFileBytes(fileSix, six);
        CHECK(juce::File(juce::String(fileSix.string())).setLastModificationTime(juce::Time(1'600'000'000'000)));
        CHECK_EQ(run(*rec, "op-six", "rename", volume, [&] {
                     commands::rename(volume, 6, "Six", options(rec, "op-six"));
                 }),
                 std::string());
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio a JOIN ops o ON o.seq = a.op "
                           "WHERE o.id = 'op-six' AND a.slot = 6 AND a.hash IS NOT NULL AND a.modified = 1600000000000"),
                 1);
        // now re-record again under the stamped row: another stamp, no hash
        six.back() = static_cast<char>(six.back() ^ 0x3c);
        commands::writeFileBytes(fileSix, six);
        CHECK(juce::File(juce::String(fileSix.string())).setLastModificationTime(juce::Time(1'600'000'010'000)));
        CHECK_EQ(run(*rec, "op-six-again", "rename", volume, [&] {
                     commands::rename(volume, 6, "Six again", options(rec, "op-six-again"));
                 }),
                 std::string());
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio a JOIN ops o ON o.seq = a.op "
                           "WHERE o.id = 'op-six-again' AND a.slot = 6 AND a.hash IS NULL AND a.modified = 1600000010000"),
                 1);
    }

    {
        // A failed operation is not written down as a state. The failure that
        // discriminates is one whose audio hooks never fire — a rename cannot
        // touch a take — and that still gets far enough to be announced: the
        // body change is reported before the pair is written, and the write is
        // what fails. The slot holds its take throughout; no row may claim
        // that as the state the operation left, because it left none.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 4, "004_1.WAV", 132300);
        auto rec = recorderAt(tmp.path / "history");
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_read,
                        fs::perm_options::replace);
        const std::string error = run(*rec, "op-failed", "rename", volume, [&] {
            commands::rename(volume, 4, "Never", options(rec, "op-failed"));
        });
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_all,
                        fs::perm_options::replace);
        CHECK(!error.empty());
        sqlite::Db& db = rec->store().db();
        CHECK_EQ(text(db, "SELECT status FROM ops WHERE id = 'op-failed'"), std::string("failed"));
        // it got as far as announcing the body change...
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_changes c JOIN ops o ON o.seq = c.op "
                           "WHERE o.id = 'op-failed'"),
                 1);
        // ...the take never moved and is still there...
        CHECK_EQ(volume::listSlotWavs(volume, 4).size(), 1u);
        // ...and nothing claims a state this operation never reached
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio a JOIN ops o ON o.seq = a.op "
                           "WHERE o.id = 'op-failed'"),
                 0);
    }

    // --- the job's own line reaches the history ---
    //
    // An operation can finish having changed nothing on the card — normalize
    // finds the slot already at target and writes no byte. Its row then has no
    // slot_changes and no slot_audio to speak through, and the line the app
    // wrote about it is the only record there is.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        auto rec = recorderAt(tmp.path / "history");

        rec->begin("op-said", "normalize", volume);
        rec->finish("op-said", "", "already at -18.0 LUFS");

        rec->begin("op-quiet", "rename", volume);
        rec->finish("op-quiet", "", "");

        rec->begin("op-broke", "trim", volume);
        rec->finish("op-broke", "cannot write MEMORY1.RC0", "trimmed to 12.0 s");

        sqlite::Db& db = rec->store().db();
        CHECK_EQ(text(db, "SELECT note FROM ops WHERE id = 'op-said'"),
                 std::string("already at -18.0 LUFS"));
        CHECK_EQ(text(db, "SELECT status FROM ops WHERE id = 'op-said'"), std::string("done"));
        // nothing to say: NULL, not an empty string pretending to be a line
        CHECK_EQ(text(db, "SELECT note FROM ops WHERE id = 'op-quiet'"), std::string("<null>"));
        // a failure keeps its reason: it outranks the story
        CHECK_EQ(text(db, "SELECT note FROM ops WHERE id = 'op-broke'"),
                 std::string("cannot write MEMORY1.RC0"));
        CHECK_EQ(text(db, "SELECT status FROM ops WHERE id = 'op-broke'"), std::string("failed"));
    }

    // --- the slot an operation is about (#144): named once it has begun, kept when it wrote nothing ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        auto rec = recorderAt(tmp.path / "history");
        CHECK_THROWS(rec->subject("op-unbegun", 7), "without having begun");
        rec->begin("op-about", "normalize", volume);
        rec->subject("op-about", 7);
        CHECK_THROWS(rec->subject("op-about", 7), "already a subject");
        CHECK_THROWS(rec->subject("op-about", 0), "1..99");
        CHECK_THROWS(rec->subject("op-about", 100), "1..99");
        rec->finish("op-about", "", "already at -18.0 LUFS (measured -18.1), nothing to do");
        auto& db = rec->store().db();
        CHECK_EQ(text(db, "SELECT status FROM ops WHERE id = 'op-about'"), std::string("done"));
        CHECK_EQ(count(db, "SELECT count(*) FROM op_subjects WHERE slot = 7"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_changes"), 0);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio"), 0); // wrote nothing, so holds nothing it can say
        const auto rows = rec->store().slotTimeline(7);
        CHECK_EQ(rows.size(), 1u);
        CHECK(rows.size() == 1u && rows.front().subjectOnly);
        CHECK(rows.size() == 1u && rows.front().kind == "normalize");
        // maintenance is about no slot: a subject on it is refused, not written
        const auto card = *rec->store().selectedCard();
        rec->beginMaintenance("op-forget", card);
        CHECK_THROWS(rec->subject("op-forget", 7), "about no slot");
        CHECK_EQ(count(db, "SELECT count(*) FROM op_subjects"), 1);
        rec->finish("op-forget", "");
        CHECK_EQ(text(db, "SELECT status FROM ops WHERE id = 'op-forget'"), std::string("done"));
        CHECK_EQ(rec->store().slotTimeline(7).size(), 1u);
    }

    // --- every take row carries the stamp the card's directory entry showed (#141) ---
    //
    // Theory: whatever way a take reaches a row — photographed by the first
    // sighting, found in place after an operation, landed by a write — the
    // row says what the directory entry said, exactly, so a later connect
    // can compare the two without reading the file.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 4, "take.wav", 132300);
        const fs::path takeFile = volume::wavDir(volume, 4) / "take.wav";
        // a stamp of the test's choosing, with milliseconds, years in the past
        const std::int64_t stamped = 1'600'000'000'123;
        CHECK(juce::File(juce::String(takeFile.string())).setLastModificationTime(juce::Time(stamped)));
        CHECK_EQ(fileStamp(takeFile), stamped); // the file system kept it to the millisecond
        auto rec = recorderAt(tmp.path / "history");
        CHECK_EQ(run(*rec, "op-rename", "rename", volume, [&] {
                     commands::rename(volume, 4, "Stamped", options(rec, "op-rename"));
                 }),
                 std::string());
        sqlite::Db& db = rec->store().db();
        // the first sighting photographed the take with its stamp...
        CHECK_EQ(count(db, "SELECT modified FROM slot_audio a JOIN ops o ON o.seq = a.op "
                           "WHERE o.kind = 'snapshot' AND a.slot = 4"),
                 stamped);
        // ...and the rename, which did not touch the file, wrote the same stamp down again
        CHECK_EQ(count(db, "SELECT modified FROM slot_audio a JOIN ops o ON o.seq = a.op "
                           "WHERE o.id = 'op-rename' AND a.slot = 4"),
                 stamped);
        // a trim lands a new file: its row carries the new entry's stamp — the
        // write's own time, not the old file's
        const std::int64_t before = juce::Time::currentTimeMillis();
        CHECK_EQ(run(*rec, "op-trim", "trim", volume, [&] {
                     commands::trim(volume, 4, 0, 66150, { .write = options(rec, "op-trim") });
                 }),
                 std::string());
        const std::int64_t after = juce::Time::currentTimeMillis();
        const std::int64_t landed = count(db, "SELECT modified FROM slot_audio a JOIN ops o ON o.seq = a.op "
                                              "WHERE o.id = 'op-trim' AND a.side = 'after'");
        CHECK(landed != stamped);
        CHECK(landed >= before - 2000 && landed <= after + 2000); // a file system may round to whole seconds
        CHECK_EQ(landed, fileStamp(takeFile));
        // every row that says what the slot holds has its stamp; the archived
        // take's row names bytes on their way out, and a connect never meets them
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE side = 'after' AND modified IS NULL"), 0);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio WHERE side = 'after'"), 3); // snapshot, rename, trim
    }

    // --- what normalize measured is in the history, under the bytes it measured (#140) ---
    //
    // Theory: the reading is of the take as the command found it, so it is
    // filed under the hash of those bytes — the same hash the archive names
    // when a write follows — and it is filed whether a write follows or not.
    // The bytes a gain lands get no derived number: nobody measured them. And
    // a reading has nowhere to go while no card is in front of the history.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putSineWav(volume, 6, "take.wav", 44100, -28.0);
        const fs::path takeFile = volume::wavDir(volume, 6) / "take.wav";
        const std::string original = commands::readFileBytes(takeFile);
        auto rec = recorderAt(tmp.path / "history");
        // nothing in front of the history yet: the reading is not taken, and the store is not even opened for it
        CHECK(!rec->reading(HistoryStore::contentHash(original), { -28.0, 0.04f, -28.0, 0 }));
        CHECK(!fs::exists(tmp.path / "history" / "history.db"));

        CHECK_EQ(run(*rec, "op-norm", "normalize", volume, [&] {
                     commands::normalize(volume, 6, { .targetLufs = -18.0, .write = options(rec, "op-norm") });
                 }),
                 std::string());
        sqlite::Db& db = rec->store().db();
        const auto before = rec->store().readingFor(HistoryStore::contentHash(original));
        CHECK(before.has_value());
        CHECK(before.has_value() && before->reading.integratedLufs.has_value()
              && std::abs(*before->reading.integratedLufs - (-28.0)) <= 0.1);
        CHECK(before.has_value() && before->reading.wildSamples == 0);
        CHECK(before.has_value() && before->measuredMs > 1'000'000); // the recorder's clock, not a zero
        // the same hash the archive filed the original under
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio a JOIN ops o ON o.seq = a.op "
                           "WHERE o.id = 'op-norm' AND a.side = 'before' AND a.hash = (SELECT hash FROM loudness_readings)"),
                 1);
        // the bytes the gain landed have no number of their own
        const std::string landed = commands::readFileBytes(takeFile);
        CHECK(landed != original);
        CHECK(!rec->store().readingFor(HistoryStore::contentHash(landed)).has_value());

        // the slot is at target now: the second normalize writes nothing,
        // archives nothing, has nothing to say about the slot (#144 names it
        // as a subject in the app) — and still files what it measured
        CHECK_EQ(run(*rec, "op-again", "normalize", volume, [&] {
                     commands::normalize(volume, 6, { .targetLufs = -18.0, .write = options(rec, "op-again") });
                 }),
                 std::string());
        CHECK_EQ(text(db, "SELECT status FROM ops WHERE id = 'op-again'"), std::string("done"));
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_audio a JOIN ops o ON o.seq = a.op WHERE o.id = 'op-again'"), 0);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_changes c JOIN ops o ON o.seq = c.op WHERE o.id = 'op-again'"), 0);
        const auto now = rec->store().readingFor(HistoryStore::contentHash(landed));
        CHECK(now.has_value());
        CHECK(now.has_value() && now->reading.integratedLufs.has_value()
              && std::abs(*now->reading.integratedLufs - (-18.0)) <= 0.1);
        CHECK_EQ(count(db, "SELECT count(*) FROM loudness_readings"), 2);

        // the card goes away: a reading has nowhere to go again, and nothing is filed
        rec->disconnect();
        CHECK(!rec->reading(HistoryStore::contentHash("later"), { -20.0, 0.5f, -3.0, 0 }));
        CHECK_EQ(count(db, "SELECT count(*) FROM loudness_readings"), 2);
        // and the store's refusals come through, a session or not
        rec->selectVolume(volume);
        CHECK_THROWS(rec->reading("short", { -20.0, 0.5f, -3.0, 0 }), "32 bytes");
    }

    // --- a loudness check files what it read, and answers even when the history will not (#140) ---
    //
    // Theory: the job reads the take once, measures it, and files the reading
    // under the hash of exactly the bytes it read. The answer does not depend
    // on the filing: no card in front of the history, nothing filed and no
    // failure; a store that refuses, the refusal reported and the reading
    // still returned. A slot with no take is an error, as before.
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putSineWav(volume, 9, "009_1.WAV", 44100, -23.0);
        const std::string bytes = commands::readFileBytes(volume::wavDir(volume, 9) / "009_1.WAV");
        auto rec = recorderAt(tmp.path / "history");

        const auto cold = history::readSlotLoudness(volume, 9, *rec);
        CHECK(cold.hash == HistoryStore::contentHash(bytes));
        CHECK(cold.reading.integratedLufs.has_value()
              && std::abs(*cold.reading.integratedLufs - (-23.0)) <= 0.1);
        CHECK(!cold.kept);
        CHECK(cold.failure.empty());
        CHECK(!fs::exists(tmp.path / "history" / "history.db")); // not even opened for it

        rec->selectVolume(volume); // the card is in front of the history now
        const auto warm = history::readSlotLoudness(volume, 9, *rec);
        CHECK(warm.kept);
        CHECK(warm.failure.empty());
        const auto stored = rec->store().readingFor(HistoryStore::contentHash(bytes));
        CHECK(stored.has_value());
        CHECK(stored.has_value() && stored->reading.integratedLufs.has_value()
              && warm.reading.integratedLufs.has_value()
              && std::abs(*stored->reading.integratedLufs - *warm.reading.integratedLufs) <= 1.0e-12);
        CHECK(stored.has_value() && stored->reading.wildSamples == 0);
        CHECK_EQ(count(rec->store().db(), "SELECT count(*) FROM loudness_readings"), 1);

        // the history cannot take it: the answer still comes, with the refusal beside it
        rec->store().db().exec("DROP TABLE loudness_readings");
        const auto broken = history::readSlotLoudness(volume, 9, *rec);
        CHECK(broken.reading.integratedLufs.has_value());
        CHECK(broken.hash == HistoryStore::contentHash(bytes));
        CHECK(!broken.kept);
        CHECK(!broken.failure.empty());

        CHECK_THROWS(history::readSlotLoudness(volume, 10, *rec), "no audio to measure");
    }

    // --- the measure-first step of Normalize asks the history before the meter (#142) ---
    //
    // Theory: the bytes come off the card and are hashed; a reading the
    // history holds for exactly that hash is the answer and nothing is
    // measured — proven by planting a reading the meter could never take off
    // these bytes and getting it back, dated as planted. Bytes the history has
    // never seen are measured and filed under their own hash — even a take
    // swapped in under the same name, size and date as one the history has a
    // row and a reading for: what a slot's facts suggest (#141) is a guess,
    // and a guess never answers here. No card in front of the history:
    // measured, nothing filed, the store not opened. A history that cannot
    // answer: measured around, the trouble reported. And the step opens no
    // operation: after a take with nothing to do the card's timeline is as it
    // was, while a real normalize then adds exactly one row.
    {
        constexpr double kTarget = -18.0;
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        const fs::path take9 = volume::wavDir(volume, 9) / "009_1.WAV";
        putSineWav(volume, 9, "009_1.WAV", 44100, -23.0);
        const std::string hash = HistoryStore::contentHash(commands::readFileBytes(take9));
        auto rec = recorderAt(tmp.path / "history");
        const auto any = [](const wav::LoudnessReading&) { return true; };

        // No card in front of the history: measured, nothing filed, not even opened to ask.
        const auto cold = history::recallOrReadSlotLoudness(volume, 9, *rec, any);
        CHECK(!cold.recalled);
        CHECK(!cold.kept);
        CHECK(cold.failure.empty());
        CHECK(cold.hash == hash);
        CHECK(cold.reading.integratedLufs.has_value()
              && std::abs(*cold.reading.integratedLufs - (-23.0)) <= 0.1);
        CHECK(!fs::exists(tmp.path / "history" / "history.db"));

        // The card connects and its first snapshot is taken, as the app takes
        // it: the take's row now names it by name, size, date and hash. From
        // here on the card's timeline is the oracle for "no row".
        const auto first = rec->firstSeen(volume);
        CHECK(first.has_value());
        for (int slot = 1; first.has_value() && slot <= 99; ++slot)
            rec->snapshotStep(*first, slot);
        sqlite::Db& db = rec->store().db();
        const auto timelineRows = [&rec] { return rec->store().cardTimeline().size(); };
        const std::size_t rowsBefore = timelineRows();
        const std::int64_t opsBefore = count(db, "SELECT count(*) FROM ops");
        CHECK_EQ(count(db, "SELECT count(*) FROM loudness_readings"), 0); // a snapshot measures nothing

        // A reading no meter would take off a -23 dBFS tone, planted under
        // these bytes' hash: if it comes back, nothing was measured.
        const wav::LoudnessReading planted { -40.0, 0.25f, -12.0, 0 };
        constexpr std::int64_t kPlantedAt = 5;
        rec->store().recordReading(hash, planted, kPlantedAt);
        const auto recalled = history::recallOrReadSlotLoudness(volume, 9, *rec, any);
        CHECK(recalled.recalled);
        CHECK(recalled.kept);
        CHECK(recalled.failure.empty());
        CHECK(recalled.hash == hash);
        CHECK(recalled.reading.integratedLufs.has_value()
              && std::abs(*recalled.reading.integratedLufs - (-40.0)) <= 1.0e-12);
        CHECK(std::abs(recalled.reading.samplePeak - 0.25f) <= 1.0e-6f);
        CHECK(std::abs(recalled.reading.truePeakDb - (-12.0)) <= 1.0e-12);
        CHECK_EQ(recalled.reading.wildSamples, 0);
        CHECK_EQ(count(db, "SELECT count(*) FROM loudness_readings"), 1); // not filed anew
        const auto row = rec->store().readingFor(hash);
        CHECK(row.has_value() && row->measuredMs == kPlantedAt); // nor dated anew

        // Another sound under the same name, the same size and the same date:
        // everything a directory entry says matches the row, and the bytes do
        // not. The step reads the bytes, so the planted -40 must not answer.
        const auto sizeBefore = fs::file_size(take9);
        const auto stampBefore = fs::last_write_time(take9);
        putSineWav(volume, 9, "009_1.WAV", 44100, -30.0);
        fs::last_write_time(take9, stampBefore);
        CHECK(fs::file_size(take9) == sizeBefore);
        CHECK(fs::last_write_time(take9) == stampBefore);
        const std::string swappedHash = HistoryStore::contentHash(commands::readFileBytes(take9));
        CHECK(swappedHash != hash);
        const auto swapped = history::recallOrReadSlotLoudness(volume, 9, *rec, any);
        CHECK(!swapped.recalled);
        CHECK(swapped.kept);
        CHECK(swapped.failure.empty());
        CHECK(swapped.hash == swappedHash);
        CHECK(swapped.reading.integratedLufs.has_value()
              && std::abs(*swapped.reading.integratedLufs - (-30.0)) <= 0.1);
        CHECK(rec->store().readingFor(swappedHash).has_value());
        CHECK_EQ(count(db, "SELECT count(*) FROM loudness_readings"), 2);
        // ...and it is the history's answer from then on, to the last digit.
        const auto again = history::recallOrReadSlotLoudness(volume, 9, *rec, any);
        CHECK(again.recalled);
        CHECK(again.reading.integratedLufs.has_value() && swapped.reading.integratedLufs.has_value()
              && std::abs(*again.reading.integratedLufs - *swapped.reading.integratedLufs) <= 1.0e-12);

        // A take already at the target: read, filed, nothing to do — and the
        // card's timeline has not moved. The old flow left a row here.
        putSineWav(volume, 10, "010_1.WAV", 44100, kTarget);
        const auto atTarget = history::recallOrReadSlotLoudness(volume, 10, *rec, any);
        CHECK(!atTarget.recalled);
        CHECK(atTarget.kept);
        CHECK(normalizeplan::decide(10, atTarget.reading, kTarget).outcome
              == normalizeplan::Plan::Outcome::nothingToDo);
        CHECK_EQ(timelineRows(), rowsBefore);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops"), opsBefore);

        // A take with something to do: the step opened nothing for it either...
        putSineWav(volume, 11, "011_1.WAV", 44100, -28.0);
        const auto quiet = history::recallOrReadSlotLoudness(volume, 11, *rec, any);
        CHECK(normalizeplan::decide(11, quiet.reading, kTarget).outcome
              == normalizeplan::Plan::Outcome::apply);
        CHECK_EQ(timelineRows(), rowsBefore);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops"), opsBefore);
        // ...and the normalize it leads to adds exactly one row.
        CHECK_EQ(run(*rec, "op-142", "normalize", volume, [&] {
                     commands::normalize(volume, 11, { .targetLufs = kTarget,
                                                       .write = options(rec, "op-142") });
                 }),
                 std::string());
        CHECK_EQ(timelineRows(), rowsBefore + 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops"), opsBefore + 1);

        // The history cannot be asked: the bytes still can be, and the trouble is said.
        db.exec("DROP TABLE loudness_readings");
        const auto broken = history::recallOrReadSlotLoudness(volume, 9, *rec, any);
        CHECK(!broken.recalled);
        CHECK(!broken.kept);
        CHECK(broken.failure.rfind("the history could not be asked: ", 0) == 0);
        CHECK(broken.hash == swappedHash);
        CHECK(broken.reading.integratedLufs.has_value()
              && std::abs(*broken.reading.integratedLufs - (-30.0)) <= 0.1);

        CHECK_THROWS(history::recallOrReadSlotLoudness(volume, 12, *rec, any), "no audio to measure");
    }

    // --- a stored reading the meter could not have taken is measured over (#142 review) ---
    //
    // Theory: the store is believed for what the meter can say and nothing
    // else. A row planted past recordReading's checks — an infinite loudness,
    // a loudness beside a peak that is not finite, a sample peak that is not
    // finite, a loudness at or under the -70 LUFS gate — is no answer: the
    // bytes are measured once and the fresh reading replaces the row. A
    // plausible row the caller cannot use is measured over the same way, the
    // caller asked once and never about the fresh reading. A store that cannot
    // be asked is not asked again to file — shown with an authorizer that
    // refuses the ask and lets a filing through: no row appears. `failure`
    // is set only when nothing was kept, and says which of the two failed.
    {
        // What the meter can and cannot say, on its own.
        const double inf = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        CHECK(history::plausibleReading({ -30.0, 0.03f, -29.5, 0 }));
        CHECK(history::plausibleReading({ std::nullopt, 0.0f, -inf, 0 }));   // digital silence
        CHECK(history::plausibleReading({ std::nullopt, 0.5f, -6.0, 0 }));   // under one gating block
        CHECK(history::plausibleReading({ 767.0, 2.4e38f, 400.0, 1234 }));   // damaged, as read
        CHECK(!history::plausibleReading({ inf, 0.03f, -29.5, 0 }));
        CHECK(!history::plausibleReading({ -inf, 0.03f, -29.5, 0 }));
        CHECK(!history::plausibleReading({ nan, 0.03f, -29.5, 0 }));
        CHECK(!history::plausibleReading({ -30.0, 0.03f, -inf, 0 }));
        CHECK(!history::plausibleReading({ -30.0, 0.03f, inf, 0 }));
        CHECK(!history::plausibleReading({ -30.0, 0.03f, nan, 0 }));
        CHECK(!history::plausibleReading({ -30.0, -0.5f, -29.5, 0 }));
        CHECK(!history::plausibleReading({ -30.0, std::numeric_limits<float>::infinity(), -29.5, 0 }));
        CHECK(!history::plausibleReading({ -30.0, std::numeric_limits<float>::quiet_NaN(), -29.5, 0 }));
        CHECK(!history::plausibleReading({ loudness::kAbsoluteGateLufs, 0.03f, -29.5, 0 }));
        CHECK(!history::plausibleReading({ -80.0, 0.03f, -29.5, 0 }));
        CHECK(!history::plausibleReading({ std::nullopt, 0.0f, nan, 0 }));
        CHECK(!history::plausibleReading({ -30.0, 0.03f, -29.5, -1 }));

        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putSineWav(volume, 9, "009_1.WAV", 44100, -30.0);
        const std::string hash = HistoryStore::contentHash(
            commands::readFileBytes(volume::wavDir(volume, 9) / "009_1.WAV"));
        auto rec = recorderAt(tmp.path / "history");
        rec->selectVolume(volume);
        sqlite::Db& db = rec->store().db();
        const auto any = [](const wav::LoudnessReading&) { return true; };
        constexpr std::int64_t kPlantedAt = 7;
        const auto plant = [&db, &hash](std::optional<double> lufs, double samplePeak, double truePeak) {
            sqlite::Statement put(db, "INSERT OR REPLACE INTO loudness_readings"
                                      "(hash, integrated_lufs, sample_peak, true_peak_dbtp, wild_samples, measured) "
                                      "VALUES (?1, ?2, ?3, ?4, 0, 7)");
            put.bindBlob(1, hash);
            if (lufs.has_value())
                put.bindReal(2, *lufs);
            else
                put.bindNull(2);
            put.bindReal(3, samplePeak).bindReal(4, truePeak).run();
        };
        const auto measuredFresh = [&](const history::SlotLoudness& read) {
            const auto row = rec->store().readingFor(hash);
            return !read.recalled && read.kept && read.failure.empty() && read.hash == hash
                && read.reading.integratedLufs.has_value()
                && std::abs(*read.reading.integratedLufs - (-30.0)) <= 0.1
                && row.has_value() && row->measuredMs != kPlantedAt
                && row->reading.integratedLufs.has_value()
                && std::abs(*row->reading.integratedLufs - *read.reading.integratedLufs) <= 1.0e-12;
        };

        int asked = 0;
        const auto counting = [&asked](const wav::LoudnessReading&) {
            ++asked;
            return true;
        };
        plant(inf, 0.03, -29.5);
        CHECK(measuredFresh(history::recallOrReadSlotLoudness(volume, 9, *rec, counting)));
        plant(-inf, 0.03, -29.5);
        CHECK(measuredFresh(history::recallOrReadSlotLoudness(volume, 9, *rec, counting)));
        plant(-30.0, 0.03, -inf);
        CHECK(measuredFresh(history::recallOrReadSlotLoudness(volume, 9, *rec, counting)));
        plant(-30.0, 0.03, inf);
        CHECK(measuredFresh(history::recallOrReadSlotLoudness(volume, 9, *rec, counting)));
        plant(-30.0, inf, -29.5);
        CHECK(measuredFresh(history::recallOrReadSlotLoudness(volume, 9, *rec, counting)));
        plant(loudness::kAbsoluteGateLufs, 0.03, -29.5);
        CHECK(measuredFresh(history::recallOrReadSlotLoudness(volume, 9, *rec, counting)));
        plant(-80.0, 0.03, -29.5);
        CHECK(measuredFresh(history::recallOrReadSlotLoudness(volume, 9, *rec, counting)));
        CHECK_EQ(asked, 0); // an implausible row is never put to the caller

        // A plausible row the caller cannot use: asked once, about that row
        // only, and measured over.
        rec->store().recordReading(hash, { -40.0, 0.25f, -12.0, 0 }, kPlantedAt);
        std::vector<double> putToCaller;
        const auto refusing = [&putToCaller](const wav::LoudnessReading& known) {
            putToCaller.push_back(known.integratedLufs.value_or(0.0));
            return false;
        };
        CHECK(measuredFresh(history::recallOrReadSlotLoudness(volume, 9, *rec, refusing)));
        CHECK_EQ(putToCaller.size(), 1u);
        CHECK(!putToCaller.empty() && std::abs(putToCaller.front() - (-40.0)) <= 1.0e-12);
        CHECK_EQ(count(db, "SELECT count(*) FROM loudness_readings"), 1);

        // The ask refused, the filing allowed: nothing is filed, so nothing
        // was tried, and the trouble named is the ask's.
        db.exec("DELETE FROM loudness_readings");
        const auto denySelect = [](void*, int action, const char*, const char*, const char*,
                                   const char*) { return action == SQLITE_SELECT ? SQLITE_DENY : SQLITE_OK; };
        sqlite3_set_authorizer(db.raw(), denySelect, nullptr);
        const auto unasked = history::recallOrReadSlotLoudness(volume, 9, *rec, any);
        // The control: under the same authorizer a filing goes through.
        const std::string other = HistoryStore::contentHash("another take");
        rec->store().recordReading(other, { -20.0, 0.5f, -3.0, 0 }, 9);
        sqlite3_set_authorizer(db.raw(), nullptr, nullptr);
        CHECK(!unasked.recalled);
        CHECK(!unasked.kept);
        CHECK(unasked.failure.rfind("the history could not be asked: ", 0) == 0);
        CHECK(unasked.reading.integratedLufs.has_value()
              && std::abs(*unasked.reading.integratedLufs - (-30.0)) <= 0.1);
        CHECK(!rec->store().readingFor(hash).has_value());
        CHECK(rec->store().readingFor(other).has_value());

        // The ask answered, the filing refused: the trouble named is the filing's.
        const auto denyInsert = [](void*, int action, const char*, const char*, const char*,
                                   const char*) { return action == SQLITE_INSERT ? SQLITE_DENY : SQLITE_OK; };
        sqlite3_set_authorizer(db.raw(), denyInsert, nullptr);
        const auto unfiled = history::recallOrReadSlotLoudness(volume, 9, *rec, any);
        sqlite3_set_authorizer(db.raw(), nullptr, nullptr);
        CHECK(!unfiled.recalled);
        CHECK(!unfiled.kept);
        CHECK(!unfiled.failure.empty());
        CHECK(unfiled.failure.rfind("the history could not be asked", 0) != 0);
        CHECK(!rec->store().readingFor(hash).has_value());
    }

    // --- the wiring refuses to be built without what it needs ---
    {
        TempDir tmp;
        CHECK_THROWS(history::withHistory(nullptr, { .opId = "x" }), "without a recorder");
        CHECK_THROWS(history::withHistory(recorderAt(tmp.path), {}), "without an id");
        CHECK_THROWS(HistoryRecorder(tmp.path, nullptr), "needs a clock");
    }

    return testkit::summary("history_recorder_tests");
}
