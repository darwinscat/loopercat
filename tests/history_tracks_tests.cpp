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
//   - a row reads its takes back one per track, in track order; it can be
//     restored only when every track's take can be produced, and what it
//     restores is what the operation LEFT — the takes a clear archived are
//     no part of the empty slot it left, on any card
//   - Undo puts back each track's archived take, empties a track a take
//     arrived into, and leaves alone a track the operation did not change
//   - on the two-track model, through the recorder as the app wires it: a
//     clear takes both tracks off the card and keeps both; Restore this
//     state and Undo put both back, byte for byte; a rename writes down what
//     each track still holds, each with its own hash

#include "support.hpp"
#include "two_track_card.hpp"

#include "../app/history/CardRestore.h"
#include "../app/history/HistoryRecorder.h"
#include "../app/history/HistoryStore.h"
#include "../app/history/RecordedState.h"
#include "../app/history/SlotRows.h"
#include "../app/history/Undo.h"
#include "../app/history/UndoRun.h"

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

    // --- a row reads its takes back one per track ---
    {
        TempDir tmp;
        const fs::path volume = testkit::makeTwoTrackCard(
            tmp.path, { { 1, 1, "001_1.WAV", 4410, 1 }, { 1, 2, "001_2.WAV", 8820, 2 },
                        { 11, 2, "011_2.WAV", 4410, 5 } });
        const auto rec = std::make_shared<HistoryRecorder>(tmp.path / "history", tick);
        const auto snapshot = rec->firstSeen(volume);
        if (snapshot)
            for (int slot = 1; slot <= 99; ++slot)
                rec->snapshotStep(*snapshot, slot);
        HistoryStore& store = rec->store();
        const auto one = store.slotTimeline(1);
        CHECK_EQ(one.size(), 1u);
        if (one.size() == 1u) {
            const std::vector<HistoryStore::SlotTake> both {
                { 1, "001_1.WAV", hashOf(takePath(volume, 1, 1, "001_1.WAV")), true },
                { 2, "001_2.WAV", hashOf(takePath(volume, 1, 2, "001_2.WAV")), true } };
            CHECK(one.front().takes == both);
            CHECK(one.front().takeIsAfter);
            CHECK_EQ(one.front().takeCount, 2);
            // The state, as restore takes it: both takes, each on its own track.
            const commands::SlotState state = history::recordedState(store, one.front(), 1);
            CHECK_EQ(state.takes.size(), 2u);
            if (state.takes.size() == 2u) {
                CHECK(state.takes[0].track == 1 && state.takes[0].fileName == "001_1.WAV");
                CHECK(state.takes[1].track == 2 && state.takes[1].fileName == "001_2.WAV");
                CHECK(state.takes[1].bytes == commands::readFileBytes(takePath(volume, 1, 2, "001_2.WAV")));
            }
            CHECK(state.body == *one.front().afterBody);
        }
        const auto eleven = store.slotTimeline(11);
        CHECK(eleven.size() == 1u && eleven.front().takes.size() == 1u
              && eleven.front().takes.front().track == 2);
        // The card's view reads the same takes as the slot's.
        for (const auto& entry : store.cardTimeline())
            for (const auto& touched : entry.slots)
                if (touched.slot == 1)
                    CHECK(touched.facts.takes == one.front().takes);
    }

    // --- restorable only when every track's take can be produced ---
    {
        using Entry = HistoryStore::TimelineEntry;
        const std::string body = testkit::syntheticTwoTrackSlotBody("Two", { 1, 4410 }, { 1, 8820 }, 1200, 1);
        Entry restored;
        restored.op = 1;
        restored.kind = "restore";
        restored.actor = "app";
        restored.status = "done";
        restored.afterBody = body;
        restored.takeIsAfter = true;
        restored.takes = { { 1, "001_1.WAV", std::string(32, '\x11'), true },
                           { 2, "001_2.WAV", std::string(32, '\x22'), true } };
        Entry later = restored;
        later.op = 2;
        CHECK(history::rows::forSlot({ restored, later }).front().restorable);
        CHECK(history::rows::forSlot({ restored, later }).front().playable);
        CHECK_EQ(history::rows::forSlot({ restored, later }).front().takeHash, std::string(32, '\x11'));
        // The second track's bytes are gone: the pair cannot go back whole,
        // and the first still plays.
        restored.takes[1].kept = false;
        CHECK(!history::rows::forSlot({ restored, later }).front().restorable);
        CHECK(history::rows::forSlot({ restored, later }).front().playable);
        CHECK_EQ(history::rows::forSlot({ restored, later }).front().line.audio,
                 std::string("take no longer kept"));
        // The first track's are gone: what plays is the second's.
        restored.takes[1].kept = true;
        restored.takes[0].kept = false;
        CHECK(!history::rows::forSlot({ restored, later }).front().restorable);
        CHECK_EQ(history::rows::forSlot({ restored, later }).front().takeHash, std::string(32, '\x22'));
        // A take the store never saw (no hash) is not one Restore can produce.
        restored.takes[0] = { 1, "001_1.WAV", std::nullopt, false };
        CHECK(!history::rows::forSlot({ restored, later }).front().restorable);

        // The state reader refuses, by track, a take it cannot produce.
        TempDir tmp;
        HistoryStore store(tmp.path / "history");
        CHECK_THROWS(history::recordedState(store, restored, 7), "slot 7 on track 1 in that state");
        Entry noBody = later;
        noBody.afterBody.reset();
        CHECK_THROWS(history::recordedState(store, noBody, 7), "recorded no state for slot 7");
    }

    // --- what a clear left is the empty slot, on any card ---
    //
    // A clear archives the slot's takes and leaves it empty. Its row offers
    // the archived takes to listen to; its state is the empty slot, and
    // putting that state back archives what the slot holds by then and puts
    // back none of the cleared takes. On the RC-5, where it can be run.
    {
        TempDir tmp;
        const fs::path volume = tmp.path / "BOSS RC-5";
        fs::create_directories(volume / "ROLAND" / "WAVE");
        fs::create_directories(volume::dataDir(volume));
        const std::string text = testkit::syntheticMemoryText();
        for (const int fileNo : { 1, 2 })
            commands::writeFileBytes(volume::memoryPath(volume, fileNo), rc0::setTailMarker(text, fileNo));
        const auto rec = std::make_shared<HistoryRecorder>(tmp.path / "history", tick);
        const auto snapshot = rec->firstSeen(volume);
        if (snapshot)
            for (int slot = 1; slot <= 99; ++slot)
                rec->snapshotStep(*snapshot, slot);
        const fs::path source = tmp.path / "loop.wav";
        const auto runOp = [&](const std::string& id, const std::string& kind, const auto& work) {
            std::string error;
            try {
                rec->begin(id, kind, volume);
                work(history::withHistory(rec, { .opId = id }));
            } catch (const std::exception& e) {
                error = e.what();
            }
            rec->finish(id, error);
            return error;
        };
        commands::writeFileBytes(source, testkit::floatTake(132300, 1));
        CHECK_EQ(runOp("push-1", "push", [&](const commands::WriteOptions& o) {
            commands::push(volume, source, 4, { .write = o });
        }), std::string());
        CHECK_EQ(runOp("clear-1", "clear", [&](const commands::WriteOptions& o) {
            commands::clear(volume, { 4 }, { .write = o });
        }), std::string());
        commands::writeFileBytes(source, testkit::floatTake(176400, 2));
        CHECK_EQ(runOp("push-2", "push", [&](const commands::WriteOptions& o) {
            commands::push(volume, source, 4, { .write = o });
        }), std::string());
        HistoryStore& store = rec->store();
        std::int64_t clearOp = 0;
        for (const auto& entry : store.cardTimeline())
            if (entry.kind == "clear")
                clearOp = entry.op;
        const auto rows = history::rows::forCard(store.cardTimeline());
        for (const auto& row : rows)
            if (row.op == clearOp) {
                CHECK(row.restorable());
                CHECK(row.playable()); // the cleared take can still be heard
            }
        const std::string pushed = commands::readFileBytes(volume::wavDir(volume, 4) / "loop.wav");
        CHECK_EQ(runOp("restore-clear", "restore", [&](const commands::WriteOptions& o) {
            history::restoreOperation(store, clearOp, volume, o);
        }), std::string());
        CHECK(volume::listSlotWavs(volume, 4).empty());
        CHECK(rc0::slotBody(commands::readMemory(volume), 4) == rc0::factorySlotBody(4));
        CHECK(store.takeBytes(HistoryStore::contentHash(pushed)) == pushed); // archived on its way out
    }

    // --- Undo, track by track ---
    {
        using Card = HistoryStore::CardEntry;
        const auto twoTrack = [](long long t1, long long t2) {
            return testkit::syntheticTwoTrackSlotBody("Two", { t1 > 0 ? 1 : 0, t1 }, { t2 > 0 ? 1 : 0, t2 },
                                                      1200, 1);
        };
        const auto cardOf = [](const char* kind, Card::Slot slot) {
            Card entry;
            entry.op = 7;
            entry.at = 7000;
            entry.kind = kind;
            entry.actor = "app";
            entry.status = "done";
            entry.session = 1;
            entry.slots = { std::move(slot) };
            return entry;
        };
        // A clear of both tracks: both go back from the history.
        Card::Slot cleared;
        cleared.slot = 5;
        cleared.facts.beforeBody = twoTrack(4410, 8820);
        cleared.facts.afterBody = twoTrack(0, 0);
        cleared.archived = { { 1, "005_1.WAV", std::string(32, '\x01'), true },
                             { 2, "005_2.WAV", std::string(32, '\x02'), true } };
        const auto plan = history::undo::plan({ cardOf("clear", cleared) }, 7);
        CHECK(plan.possible());
        CHECK(plan.steps.size() == 1u && plan.steps.front().takes.size() == 2u);
        if (plan.steps.size() == 1u && plan.steps.front().takes.size() == 2u) {
            CHECK(plan.steps.front().takes[0].track == 1 && plan.steps.front().takes[0].name == "005_1.WAV");
            CHECK(plan.steps.front().takes[1].track == 2 && plan.steps.front().takes[1].hash == std::string(32, '\x02'));
            CHECK(plan.steps.front().emptied.empty());
        }
        // The second track's take is gone: refused, naming the track.
        cleared.archived[1].kept = false;
        const auto lost = history::undo::plan({ cardOf("clear", cleared) }, 7);
        CHECK(lost.refusal == history::undo::Refusal::takeNotKept);
        CHECK(lost.reason.find("on track 2") != std::string::npos);
        // A write that replaced the second track's take over a first track it
        // did not touch: track 2 goes back, track 1 is left as it is.
        Card::Slot second;
        second.slot = 5;
        second.facts.beforeBody = twoTrack(4410, 8820);
        second.facts.afterBody = twoTrack(4410, 2205);
        second.archived = { { 2, "005_2.WAV", std::string(32, '\x02'), true } };
        const auto mixed = history::undo::plan({ cardOf("restore", second) }, 7);
        CHECK(mixed.possible() && mixed.steps.size() == 1u);
        if (mixed.steps.size() == 1u) {
            CHECK(mixed.steps.front().takes.size() == 1u && mixed.steps.front().takes.front().track == 2);
            CHECK(mixed.steps.front().emptied.empty()); // track 1 held a take before: it stays
            CHECK(!mixed.steps.front().keepsAudio());
        }
        // A take that arrived on the second track into nothing leaves with
        // the undo; the first track, holding its take before and after, stays.
        Card::Slot arrived;
        arrived.slot = 5;
        arrived.facts.beforeBody = twoTrack(4410, 0);
        arrived.facts.afterBody = twoTrack(4410, 2205);
        const auto into = history::undo::plan({ cardOf("restore", arrived) }, 7);
        CHECK(into.possible() && into.steps.size() == 1u);
        if (into.steps.size() == 1u) {
            CHECK(into.steps.front().takes.empty());
            CHECK((into.steps.front().emptied == std::vector<int> { 2 }));
        }
        // The first track recorded alone into an empty memory: only it goes.
        Card::Slot firstOnly;
        firstOnly.slot = 5;
        firstOnly.facts.beforeBody = twoTrack(0, 8820);
        firstOnly.facts.afterBody = twoTrack(4410, 8820);
        const auto t1 = history::undo::plan({ cardOf("restore", firstOnly) }, 7);
        CHECK(t1.steps.size() == 1u && (t1.steps.front().emptied == std::vector<int> { 1 }));
        // A rename of a two-track memory touches no audio.
        Card::Slot renamed;
        renamed.slot = 5;
        renamed.facts.beforeBody = twoTrack(4410, 8820);
        renamed.facts.afterBody = twoTrack(4410, 8820);
        const auto r = history::undo::plan({ cardOf("rename", renamed) }, 7);
        CHECK(r.steps.size() == 1u && r.steps.front().keepsAudio());
        CHECK(history::trackSectionsIn(twoTrack(0, 0)) == 2);
        CHECK(history::undo::bodyHoldsTake(twoTrack(0, 8820), 2) && !history::undo::bodyHoldsTake(twoTrack(0, 8820), 1));
    }

    // --- the two-track model through the history ---
    {
        TempDir tmp;
        const fs::path volume = testkit::makeTwoTrackCard(
            tmp.path, { { 1, 1, "001_1.WAV", 4410, 1 }, { 1, 2, "001_2.WAV", 8820, 2 },
                        { 2, 1, "take.wav", 2205, 3 }, { 2, 2, "take.wav", 2205, 4 } });
        sameStamp(takePath(volume, 2, 1, "take.wav"), takePath(volume, 2, 2, "take.wav"));
        const std::string t1 = commands::readFileBytes(takePath(volume, 1, 1, "001_1.WAV"));
        const std::string t2 = commands::readFileBytes(takePath(volume, 1, 2, "001_2.WAV"));
        const std::string body1 = rc0::slotBody(commands::readMemory(volume), 1);
        const auto rec = std::make_shared<HistoryRecorder>(tmp.path / "history", tick);
        const auto snapshot = rec->firstSeen(volume);
        if (snapshot)
            for (int slot = 1; slot <= 99; ++slot)
                rec->snapshotStep(*snapshot, slot);
        HistoryStore& store = rec->store();
        const auto runOp = [&](const std::string& id, const std::string& kind, const auto& work) {
            std::string error;
            try {
                rec->begin(id, kind, volume);
                work(history::withHistory(rec, { .opId = id }));
            } catch (const std::exception& e) {
                error = e.what();
            }
            rec->finish(id, error);
            return error;
        };
        const auto opNamed = [&store](const std::string& id) {
            sqlite::Statement row(store.db(), "SELECT seq FROM ops WHERE id = ?1");
            row.bindText(1, id);
            return row.step() ? row.integer(0) : std::int64_t { 0 };
        };

        // A rename writes down what each track still holds, each with the
        // hash its own track's row had — though slot 2's two files share a
        // name, a size and a stamp.
        CHECK_EQ(runOp("rename-2", "rename", [&](const commands::WriteOptions& o) {
            commands::rename(volume, 2, "Same Names", o);
        }), std::string());
        {
            const auto rows = afterRows(store, opNamed("rename-2"), 2);
            CHECK_EQ(rows.size(), 2u);
            if (rows.size() == 2u) {
                CHECK(rows[0].track == 1 && rows[0].hash == hashOf(takePath(volume, 2, 1, "take.wav")));
                CHECK(rows[1].track == 2 && rows[1].hash == hashOf(takePath(volume, 2, 2, "take.wav")));
            }
        }

        // Clear: both tracks off the card, both kept.
        CHECK_EQ(runOp("clear-1", "clear", [&](const commands::WriteOptions& o) {
            commands::clear(volume, { 1 }, { .write = o });
        }), std::string());
        CHECK(volume::listTrackWavs(volume, profile::kRc500, 1, 1).empty());
        CHECK(volume::listTrackWavs(volume, profile::kRc500, 1, 2).empty());
        CHECK(rc0::slotBody(commands::readMemory(volume), 1) == rc0::factorySlotBody(profile::kRc500, 1));
        CHECK(store.takeBytes(HistoryStore::contentHash(t1)) == t1);
        CHECK(store.takeBytes(HistoryStore::contentHash(t2)) == t2);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM slot_audio WHERE side = 'before' AND slot = 1 "
                                   "AND op = (SELECT seq FROM ops WHERE id = 'clear-1')"), 2);
        CHECK(afterRows(store, opNamed("clear-1"), 1).empty());

        // Restore this state — the first sighting's slot 1: both back, byte for byte.
        CHECK_EQ(runOp("restore-1", "restore", [&](const commands::WriteOptions& o) {
            history::restoreOperation(store, snapshot ? snapshot->op : 0, volume, o, 1);
        }), std::string());
        CHECK(commands::readFileBytes(takePath(volume, 1, 1, "001_1.WAV")) == t1);
        CHECK(commands::readFileBytes(takePath(volume, 1, 2, "001_2.WAV")) == t2);
        CHECK(rc0::slotBody(commands::readMemory(volume), 1) == body1);
        {
            const auto rows = afterRows(store, opNamed("restore-1"), 1);
            CHECK_EQ(rows.size(), 2u);
            if (rows.size() == 2u)
                CHECK(rows[0].stamped && rows[1].stamped && rows[1].hash == HistoryStore::contentHash(t2));
        }

        // Clear again, and Undo it: both back from the history.
        CHECK_EQ(runOp("clear-2", "clear", [&](const commands::WriteOptions& o) {
            commands::clear(volume, { 1 }, { .write = o });
        }), std::string());
        const auto offered = store.offeredTargets().undo;
        CHECK(offered.has_value() && *offered == opNamed("clear-2"));
        if (offered) {
            std::string error;
            try {
                const history::undo::Checked checked = history::undo::beginPress(*rec, "undo-1", false, *offered, volume);
                history::undo::apply(store, checked.plan, volume, history::withHistory(rec, { .opId = "undo-1" }));
            } catch (const std::exception& e) {
                error = e.what();
            }
            CHECK_EQ(error, std::string());
            rec->finish("undo-1", error);
        }
        CHECK(commands::readFileBytes(takePath(volume, 1, 1, "001_1.WAV")) == t1);
        CHECK(commands::readFileBytes(takePath(volume, 1, 2, "001_2.WAV")) == t2);
        CHECK(rc0::slotBody(commands::readMemory(volume), 1) == body1);

        // A step that puts back the second track alone hands the core the
        // first track's take as the card holds it, byte for byte.
        history::undo::Step step;
        step.slot = 1;
        step.takes = { { 2, "001_2.WAV", HistoryStore::contentHash(t2) } };
        const commands::SlotState state = history::undo::stateFor(store, step, volume);
        CHECK_EQ(state.takes.size(), 2u);
        if (state.takes.size() == 2u) {
            CHECK(state.takes[0].track == 1 && state.takes[0].bytes == t1 && state.takes[0].fileName == "001_1.WAV");
            CHECK(state.takes[1].track == 2 && state.takes[1].bytes == t2);
        }
        CHECK(state.body == body1); // no body in the step: the card's own
        step.emptied = { 1 };
        CHECK_EQ(history::undo::stateFor(store, step, volume).takes.size(), 1u);
        history::undo::Step unknown = step;
        unknown.takes = { { 2, "001_2.WAV", std::string(32, '\x7f') } };
        CHECK_THROWS(history::undo::stateFor(store, unknown, volume), "no longer kept");
    }

    // --- the History tab's rows, from the store, for a two-track memory ---
    {
        TempDir tmp;
        const fs::path volume = testkit::makeTwoTrackCard(
            tmp.path, { { 1, 1, "001_1.WAV", 4410, 1 }, { 1, 2, "001_2.WAV", 8820, 2 } });
        const auto rec = std::make_shared<HistoryRecorder>(tmp.path / "history", tick);
        const auto snapshot = rec->firstSeen(volume);
        if (snapshot)
            for (int slot = 1; slot <= 99; ++slot)
                rec->snapshotStep(*snapshot, slot);
        std::string error;
        try {
            rec->begin("rename-1", "rename", volume);
            commands::rename(volume, 1, "Both", history::withHistory(rec, { .opId = "rename-1" }));
        } catch (const std::exception& e) {
            error = e.what();
        }
        rec->finish("rename-1", error);
        CHECK_EQ(error, std::string());
        const auto rows = history::rows::forSlot(rec->store().slotTimeline(1));
        CHECK_EQ(rows.size(), 2u);
        if (rows.size() == 2u) {
            // The Original names both takes by track, offers both for export,
            // and goes back whole.
            CHECK_EQ(rows[0].line.detail, std::string("T1 001_1.WAV, T2 001_2.WAV"));
            CHECK((rows[0].keptByTrack == std::vector<bool> { true, true }));
            CHECK(rows[0].restorable);
            CHECK_EQ(rows[0].takes.size(), 2u);
            // The rename's state is where the slot is: its takes are on the
            // card, not yet a copy the history keeps apart from them.
            CHECK(!rows[1].restorable);
            CHECK_EQ(rows[1].keptByTrack.size(), 2u);
        }
    }

    return testkit::summary("history_tracks_tests");
}
