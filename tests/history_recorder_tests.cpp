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
//   - the history keeps a take BEFORE the trash folder does, and before the
//     card changes
//   - a history that cannot open, or a hook for an op that never began, stops
//     the command with the card untouched
//   - an op cut off mid-way reads as interrupted, and its take is still kept

#include "support.hpp"

#include "../app/history/HistoryRecorder.h"

#include <loopercat/Commands.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

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
    return std::make_shared<HistoryRecorder>(dir, "RC-5", tick);
}

commands::WriteOptions options(const std::shared_ptr<HistoryRecorder>& rec, const std::string& opId,
                               const fs::path& trash)
{
    return history::withHistory(rec, { .opId = opId, .skipBackup = true },
                                commands::trashFolder(trash, opId));
}

} // namespace

int main()
{
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
                           { .write = options(rec, "op-trim", tmp.path / "trash") });
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
        sqlite::Statement after(db, "SELECT hash, size FROM slot_audio WHERE side = 'after' AND slot = 4");
        CHECK(after.step());
        CHECK(after.blob(0) == HistoryStore::contentHash(onCard));
        CHECK_EQ(after.integer(1), static_cast<std::int64_t>(onCard.size()));
        // the bodies, both sides, byte-exact
        sqlite::Statement bodies(db, "SELECT before_body, after_body FROM slot_changes WHERE slot = 4");
        CHECK(bodies.step());
        CHECK(bodies.blob(0) == bodyBefore);
        CHECK(bodies.blob(1) == rc0::slotBody(commands::readMemory(volume), 4));
        // and the transitional folder has it too
        CHECK(commands::readFileBytes(tmp.path / "trash" / "op-trim" / "004_1" / "take.wav") == original);
    }

    // --- rename and swap: bodies only, no audio moved into the history ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 3, "003_1.WAV", 132300);
        auto rec = recorderAt(tmp.path / "history");
        CHECK_EQ(run(*rec, "op-rename", "rename", volume, [&] {
                     commands::rename(volume, 3, "Intro", options(rec, "op-rename", tmp.path / "trash"));
                 }),
                 std::string());
        CHECK_EQ(run(*rec, "op-swap", "swap", volume, [&] {
                     commands::swap(volume, 3, 7, options(rec, "op-swap", tmp.path / "trash"));
                 }),
                 std::string());
        sqlite::Db& db = rec->store().db();
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_changes sc JOIN ops o ON o.seq = sc.op "
                           "WHERE o.id = 'op-rename'"),
                 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM slot_changes sc JOIN ops o ON o.seq = sc.op "
                           "WHERE o.id = 'op-swap'"),
                 2);
        CHECK_EQ(count(db, "SELECT count(*) FROM blobs"), 0);
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
            commands::trim(volume, 4, 500, 100, { .write = options(rec, "op-bad", tmp.path / "trash") });
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
            commands::clear(volume, { 6 }, { .write = options(rec, "op-half", tmp.path / "trash") });
        });
        fs::permissions(volume::memoryPath(volume, 1), fs::perms::owner_all, fs::perm_options::replace);
        CHECK(!error.empty());
        sqlite::Db& db = rec->store().db();
        CHECK_EQ(text(db, "SELECT status FROM ops WHERE id = 'op-half'"), std::string("failed"));
        CHECK(rec->store().takeBytes(HistoryStore::contentHash(original)) == original);
    }

    // --- the history keeps the take before the folder does ---
    {
        // The folder refuses (a file squats on its directory). The history must
        // already hold the take by then, and the card must not have changed.
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putWav(volume, 4, "take.wav", 132300);
        const std::string original = commands::readFileBytes(volume::wavDir(volume, 4) / "take.wav");
        commands::writeFileBytes(tmp.path / "trash", "not a directory");
        const auto before = volumeBytes(volume);
        auto rec = recorderAt(tmp.path / "history");
        const std::string error = run(*rec, "op-order", "trim", volume, [&] {
            commands::trim(volume, 4, 0, 66150, { .write = options(rec, "op-order", tmp.path / "trash") });
        });
        CHECK(!error.empty());
        CHECK(rec->store().takeBytes(HistoryStore::contentHash(original)) == original);
        CHECK(volumeBytes(volume) == before);
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
                                    { .write = options(rec, "op-unbegun", tmp.path / "trash") }),
                     "without having begun");
        CHECK(volumeBytes(volume) == before);
        CHECK_THROWS(([&] {
                         rec->begin("op-twice", "rename", volume);
                         rec->begin("op-twice", "rename", volume);
                     }()),
                     "already begun");
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
                                          options(rec, step.id, tmp.path / "trash"));
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

    // --- the wiring refuses to be built without what it needs ---
    {
        TempDir tmp;
        CHECK_THROWS(history::withHistory(nullptr, { .opId = "x" }, nullptr), "without a recorder");
        CHECK_THROWS(history::withHistory(recorderAt(tmp.path), {}, nullptr), "without an id");
        CHECK_THROWS(HistoryRecorder(tmp.path, "RC-5", nullptr), "needs a clock");
    }

    return testkit::summary("history_recorder_tests");
}
