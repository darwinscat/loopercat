// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat - see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ATTACK (review of feat/history-row-menu): "Export take..." as the worker
// runs it. What the job owes: the bytes the history keeps, whole, at the file
// the player chose - through a folder named in any alphabet - with no card
// asked for, nothing read from or written to the pedal, and no operation
// recorded; a take no longer kept is a sentence; a refusal names the file
// the player chose.

#include "support.hpp"

#include "../app/Strings.h"
#include "../app/history/ExportTakeJob.h"
#include "../app/history/HistoryRecorder.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
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
        path = fs::temp_directory_path() / ("loopercat-export-job-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

// Deterministic bytes that are not text - every value 0..255 appears.
std::string take(std::size_t size, unsigned seed)
{
    std::string out(size, '\0');
    std::uint32_t x = 2463534242u ^ seed;
    for (auto& c : out) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = static_cast<char>(x & 0xFF);
    }
    return out;
}

std::int64_t count(sqlite::Db& db, const std::string& sql)
{
    sqlite::Statement read(db, sql);
    if (!read.step())
        throw Error("count returned no row: " + sql);
    return read.integer(0);
}

fs::path partialOf(const fs::path& file)
{
    return fs::path(file.u8string() + std::u8string(u8".part"));
}

} // namespace

int main()
{
    TempDir tmp;
    std::int64_t clock = 1'000'000;
    auto rec = std::make_shared<HistoryRecorder>(tmp.path / "history", [&clock] { return clock += 1000; });
    HistoryStore& store = rec->store();
    const auto session = store.openSession(store.card("test-RC-5", "RC-5", "BOSS RC-5", 1000), 1000);
    const std::string bytes = take(300000, 11);
    const std::string hash = HistoryStore::contentHash(bytes);
    const auto trim = store.beginOp(session, "op-trim", "trim", 2000);
    store.keepAudio(trim, 7, 1, "loop.wav", bytes, 2000);
    store.finishOp(trim, history::OpStatus::done, "");
    const auto opsBefore = count(store.db(), "SELECT count(*) FROM ops");
    const auto sessionsBefore = count(store.db(), "SELECT count(*) FROM sessions");
    const auto sightingsBefore = count(store.db(), "SELECT count(*) FROM take_sightings");

    // --- a folder and a name in another alphabet, as the save dialog hands
    // them over (juce::File -> fsPath), and the job as the worker reads it ---
    {
        const fs::path dir = tmp.path / fs::path(std::u8string(u8"papá - café"));
        fs::create_directories(dir);
        const fs::path chosen = dir / fs::path(std::u8string(u8"take ñ — one.wav"));
        const juce::File asChosen(utf8(chosen.u8string()));
        CHECK(fsPath(asChosen).u8string() == chosen.u8string());

        PedalWorker::Job job = history::exportTakeJob(rec, hash, fsPath(asChosen));
        CHECK(!job.needsVolume); // no card is asked for
        CHECK(!job.background);
        CHECK(!job.quiet); // the player is told how it went
        CHECK_EQ(job.slot, 0);
        CHECK(job.before == nullptr); // no operation opens...
        CHECK(job.after == nullptr);  // ...and none closes
        CHECK(job.description == juce::String::fromUTF8("Export take to take \xc3\xb1 \xe2\x80\x94 one.wav"));
        job.work(volume::fs::path()); // the empty path a job without a card is handed
        CHECK(commands::readFileBytes(chosen) == bytes);
        CHECK(!fs::exists(partialOf(chosen)));
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM ops"), opsBefore);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM sessions"), sessionsBefore);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM take_sightings"), sightingsBefore);
    }

    // --- a take no longer kept is a sentence, and nothing is written ---
    {
        const fs::path never = tmp.path / "never.wav";
        PedalWorker::Job job = history::exportTakeJob(rec, HistoryStore::contentHash("never"), never);
        CHECK_THROWS(job.work(volume::fs::path()), "no longer kept");
        CHECK(!fs::exists(never));
        CHECK(!fs::exists(partialOf(never)));
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM ops"), opsBefore);
    }

    // --- a destination that is a folder is refused, and no partial file is left beside it ---
    {
        const fs::path folder = tmp.path / "folder.wav";
        fs::create_directories(folder);
        CHECK_THROWS(history::exportTakeJob(rec, hash, folder).work(volume::fs::path()), "");
        CHECK(fs::is_directory(folder));
        CHECK(!fs::exists(partialOf(folder)));
    }

    // --- a folder that is not there: the refusal names the file the player
    // chose, not the partial name the write went under ---
    {
        const fs::path chosen = tmp.path / "nowhere" / "take.wav";
        std::string said;
        try {
            history::exportTakeJob(rec, hash, chosen).work(volume::fs::path());
        } catch (const Error& e) {
            said = e.what();
        }
        CHECK(!said.empty());
        CHECK(said.find("take.wav") != std::string::npos);
        CHECK(said.find(".part") == std::string::npos);
        CHECK(!fs::exists(chosen));
    }

    return testkit::summary("export_take_job_tests");
}
