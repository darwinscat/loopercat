// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The history of a two-track card. A memory's state is its body and the set
// of takes its track folders hold, one per track: a state may hold a take on
// track 1, on track 2, on both or on neither. What must hold, and what these
// try to break:
//
//   - the first sighting keeps every track's take, each row with its own
//     track and its own hash; a memory with only the second track recorded
//     has one row, on track 2, and no empty track-1 row beside it
//   - two tracks holding files of one name, one size and one stamp are two
//     files: what one track held is never handed to the other
//   - a take that lands on a track is stamped from that track's folder, and
//     a track an operation did not write is written down from its own folder
//   - a sighting speaks for the first track only — the table keeps no track
//     — and a loudness reading is a fact about bytes, whichever track holds
//     them

#include "support.hpp"
#include "two_track_card.hpp"

#include "../app/history/HistoryRecorder.h"
#include "../app/history/HistoryStore.h"

#include <loopercat/Commands.hpp>

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
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
        path = fs::temp_directory_path() / ("loopercat-tracks-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec) {
            std::printf("FAIL cannot remove %s: %s\n", path.string().c_str(), ec.message().c_str());
            ++testkit::checksFailed;
        }
    }
};

std::int64_t clockAt = 5'000'000;
std::int64_t tick() { return ++clockAt; }

std::int64_t count(sqlite::Db& db, const std::string& sql)
{
    sqlite::Statement read(db, sql);
    if (!read.step())
        throw Error("count returned no row: " + sql);
    return read.integer(0);
}

// The 'after' rows one operation left for one slot: track, name, hash.
struct Row {
    int track;
    std::string name;
    std::optional<std::string> hash;
    bool stamped;
};

std::vector<Row> afterRows(HistoryStore& store, std::int64_t op, int slot)
{
    sqlite::Statement read(store.db(), "SELECT track, name, hash, modified IS NOT NULL FROM slot_audio "
                                       "WHERE op = ?1 AND slot = ?2 AND side = 'after' "
                                       "ORDER BY track, name");
    read.bind(1, op).bind(2, slot);
    std::vector<Row> rows;
    while (read.step())
        rows.push_back({ static_cast<int>(read.integer(0)), read.text(1),
                         read.isNull(2) ? std::nullopt : std::optional<std::string>(read.blob(2)),
                         read.integer(3) != 0 });
    return rows;
}

std::string hashOf(const fs::path& file)
{
    return HistoryStore::contentHash(commands::readFileBytes(file));
}

fs::path takePath(const fs::path& volume, int slot, int track, const std::string& name)
{
    return volume::trackDir(volume, profile::kRc500, slot, track) / name;
}

// Both files of a two-track memory given the same stamp, so that name, size
// and stamp cannot tell them apart — only the track can.
void sameStamp(const fs::path& a, const fs::path& b)
{
    const auto when = fs::last_write_time(a);
    fs::last_write_time(b, when);
}

} // namespace

int main()
{
    // --- the first sighting keeps every track ---
    {
        TempDir tmp;
        const fs::path volume = testkit::makeTwoTrackCard(
            tmp.path, { { 1, 1, "001_1.WAV", 4410, 1 }, { 1, 2, "001_2.WAV", 8820, 2 },
                        { 3, 1, "take.wav", 2205, 3 }, { 3, 2, "take.wav", 2205, 4 },
                        { 11, 2, "011_2.WAV", 4410, 5 } });
        sameStamp(takePath(volume, 3, 1, "take.wav"), takePath(volume, 3, 2, "take.wav"));
        const auto rec = std::make_shared<HistoryRecorder>(tmp.path / "history", tick);
        const auto snapshot = rec->firstSeen(volume);
        CHECK(snapshot.has_value());
        if (snapshot)
            for (int slot = 1; slot <= 99; ++slot)
                rec->snapshotStep(*snapshot, slot);
        HistoryStore& store = rec->store();
        const std::int64_t op = snapshot ? snapshot->op : 0;
        CHECK_EQ(store.opStatus(op), std::string("done"));

        const auto one = afterRows(store, op, 1);
        CHECK_EQ(one.size(), 2u);
        if (one.size() == 2u) {
            CHECK(one[0].track == 1 && one[0].name == "001_1.WAV");
            CHECK(one[1].track == 2 && one[1].name == "001_2.WAV");
            CHECK(one[0].hash == hashOf(takePath(volume, 1, 1, "001_1.WAV")));
            CHECK(one[1].hash == hashOf(takePath(volume, 1, 2, "001_2.WAV")));
            CHECK(one[0].stamped && one[1].stamped);
        }
        // One name on both tracks: two rows, two hashes, each its own track's.
        const auto three = afterRows(store, op, 3);
        CHECK_EQ(three.size(), 2u);
        if (three.size() == 2u) {
            CHECK(three[0].track == 1 && three[1].track == 2);
            CHECK(three[0].hash == hashOf(takePath(volume, 3, 1, "take.wav")));
            CHECK(three[1].hash == hashOf(takePath(volume, 3, 2, "take.wav")));
            CHECK(three[0].hash != three[1].hash);
        }
        // The second track alone: one row, on track 2 — no empty track-1 row.
        const auto eleven = afterRows(store, op, 11);
        CHECK_EQ(eleven.size(), 1u);
        if (eleven.size() == 1u)
            CHECK(eleven[0].track == 2 && eleven[0].name == "011_2.WAV");
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM slot_audio WHERE slot = 11 AND track = 1"), 0);
        // Every take's bytes are kept, each once.
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM blobs"), 5);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM slot_audio"), 5);
        for (const int slot : { 1, 3, 11 })
            CHECK(store.slotTimeline(slot).size() == 1u);

        // --- what an operation leaves, track by track ---
        //
        // A write on slot 3 that replaces the second track's take and leaves
        // the first alone, reported the way the core reports it: the take
        // that goes out is kept, the one that lands is stamped from the
        // second track's folder, and the first track — not written — is
        // written down from its own folder with its own hash, never the
        // second's, though name, size and stamp are the same.
        const fs::path t1 = takePath(volume, 3, 1, "take.wav");
        const fs::path t2 = takePath(volume, 3, 2, "take.wav");
        const std::string firstBytes = commands::readFileBytes(t1);
        const std::string oldSecond = commands::readFileBytes(t2);
        const std::string newSecond = testkit::floatTake(2205, 99);
        const std::string body = rc0::slotBody(commands::readMemory(volume), 3);
        const std::string error = [&] {
            try {
                rec->begin("op-t2", "restore", volume);
                const commands::WriteOptions o = history::withHistory(rec, { .opId = "op-t2" });
                o.archive(3, 2, "take.wav", oldSecond);
                commands::writeFileBytes(t2, newSecond);
                o.journal.audioWritten(3, 2, "take.wav", newSecond);
                o.journal.bodiesChanging({ { 3, body, body } });
            } catch (const std::exception& e) {
                return std::string(e.what());
            }
            return std::string();
        }();
        CHECK_EQ(error, std::string());
        rec->finish("op-t2", error);
        sqlite::Statement opRow(store.db(), "SELECT seq FROM ops WHERE id = 'op-t2'");
        CHECK(opRow.step());
        const std::int64_t t2op = opRow.integer(0);
        const auto after = afterRows(store, t2op, 3);
        CHECK_EQ(after.size(), 2u);
        if (after.size() == 2u) {
            CHECK(after[0].track == 1 && after[0].hash == HistoryStore::contentHash(firstBytes));
            CHECK(after[1].track == 2 && after[1].hash == HistoryStore::contentHash(newSecond));
        }
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM slot_audio WHERE side = 'before' AND track = 2 "
                                   "AND slot = 3"), 1);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM slot_audio WHERE side = 'before' AND track = 1"), 0);
        CHECK(store.takeBytes(HistoryStore::contentHash(oldSecond)) == oldSecond);
        // The landed row's stamp is the second track's file's: asked through
        // the store with track 2's facts, the history knows the bytes.
        const auto card = store.cardFor(marker::read(volume)->id);
        CHECK(card.has_value());
        const auto stampOf = [](const fs::path& file) {
            return juce::File(juce::String(file.string())).getLastModificationTime().toMilliseconds();
        };
        if (card) {
            const HistoryStore::TakeSighting second { "take.wav",
                                                      static_cast<std::int64_t>(newSecond.size()),
                                                      stampOf(t2), 2205, 2 };
            CHECK(store.hashOfSighting(*card, 3, second) == HistoryStore::contentHash(newSecond));
            // The frames are asked of the sighted track's own section.
            HistoryStore::TakeSighting wrongFrames = second;
            wrongFrames.frames = 4410;
            CHECK(!store.hashOfSighting(*card, 3, wrongFrames).has_value());
            HistoryStore::TakeSighting first = second;
            first.track = 1;
            first.modifiedMs = stampOf(t1);
            CHECK(store.hashOfSighting(*card, 3, first) == HistoryStore::contentHash(firstBytes));
            HistoryStore::TakeSighting none = second;
            none.track = 0;
            CHECK_THROWS(store.hashOfSighting(*card, 3, none), "track 0");

            // A read of the first track's file, filed: it speaks for track 1
            // and for nothing on track 2, though the facts are the same.
            const std::string other = testkit::floatTake(2205, 7);
            const auto began = rec->newestOp();
            CHECK(began.has_value());
            CHECK(rec->sighted(volume, 3, "take.wav", static_cast<std::int64_t>(firstBytes.size()),
                               stampOf(t1), HistoryStore::contentHash(other), began.value_or(0)));
            CHECK(store.hashOfSighting(*card, 3, first) == HistoryStore::contentHash(other));
            CHECK(store.hashOfSighting(*card, 3, second) == HistoryStore::contentHash(newSecond));
        }
    }

    // --- a reading is about bytes; a sighting about the first track ---
    {
        TempDir tmp;
        const fs::path volume = testkit::makeTwoTrackCard(
            tmp.path, { { 4, 1, "004_1.WAV", 4410, 1 }, { 4, 2, "004_2.WAV", 4410, 2 } });
        const auto rec = std::make_shared<HistoryRecorder>(tmp.path / "history", tick);
        rec->begin("op-measure", "normalize", volume);
        const commands::WriteOptions o = history::withHistory(rec, { .opId = "op-measure" });
        const std::string second = commands::readFileBytes(takePath(volume, 4, 2, "004_2.WAV"));
        const std::string first = commands::readFileBytes(takePath(volume, 4, 1, "004_1.WAV"));
        wav::LoudnessReading reading;
        reading.integratedLufs = -20.0;
        reading.truePeakDb = -3.0;
        reading.samplePeak = 0.5f;
        o.journal.loudnessMeasured(4, 2, "004_2.WAV", second, reading);
        HistoryStore& store = rec->store();
        CHECK(store.readingFor(HistoryStore::contentHash(second)).has_value());
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM take_sightings"), 0);
        o.journal.loudnessMeasured(4, 1, "004_1.WAV", first, reading);
        CHECK(store.readingFor(HistoryStore::contentHash(first)).has_value());
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM take_sightings WHERE name = '004_1.WAV'"), 1);
        rec->finish("op-measure", "");
    }

    return testkit::summary("history_tracks_tests");
}
