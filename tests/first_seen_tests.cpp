// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "support.hpp"
#include "../app/history/FirstSeenJob.h"
#include "../app/history/WriteOptionsFactory.h"
#include "../app/history/SlotRows.h"
#include "../app/history/UndoRun.h"
#include "../app/history/CardRestore.h"
#include "../app/PedalBook.h"

#include <chrono>
#include <bit>
#include <cmath>
#include <filesystem>
#include <map>

using namespace loopercat;
using history::HistoryRecorder;
using history::HistoryStore;
namespace fs = std::filesystem;

namespace {
struct Scratch {
    fs::path path = fs::temp_directory_path() / ("loopercat-first-seen-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { fs::create_directories(path); }
    ~Scratch() { fs::remove_all(path); }
};

fs::path cardAt(const fs::path& root)
{
    const auto card = root / "BOSS RC-5";
    fs::create_directories(volume::dataDir(card));
    fs::create_directories(card / "ROLAND/WAVE");
    for (int bank : { 1, 2 })
        commands::writeFileBytes(volume::memoryPath(card, bank),
            rc0::setTailMarker(testkit::syntheticMemoryText(), bank));
    return card;
}

std::string putTake(const fs::path& card, int slot, int frames, unsigned char salt = 0)
{
    auto raw = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = frames });
    if (salt != 0) raw.back() = salt;
    const std::string bytes(reinterpret_cast<const char*>(raw.data()), raw.size());
    fs::create_directories(volume::wavDir(card, slot));
    commands::writeFileBytes(volume::wavDir(card, slot) / "Original.wav", bytes);
    auto memory = commands::readMemory(card);
    auto body = rc0::slotBody(memory, slot);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "WavStat", 1);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "WavLen", frames);
    memory = rc0::replaceSlotBody(memory, slot, body);
    for (int bank : { 1, 2 })
        commands::writeFileBytes(volume::memoryPath(card, bank), rc0::setTailMarker(memory, bank));
    return bytes;
}

std::shared_ptr<HistoryRecorder> recorderAt(const fs::path& root)
{
    return std::make_shared<HistoryRecorder>(root, [] {
        static std::int64_t time = 1000;
        return ++time;
    });
}

std::int64_t number(HistoryStore& store, const std::string& sql)
{
    sqlite::Statement read(store.db(), sql);
    if (!read.step()) throw Error("no result: " + sql);
    return read.integer(0);
}

HistoryRecorder::Snapshot newSighting(HistoryRecorder& rec, const fs::path& card)
{
    const auto snapshot = rec.firstSeen(card);
    CHECK(snapshot.has_value());
    if (!snapshot) throw Error("a new card or interrupted snapshot was treated as complete");
    return *snapshot;
}

void complete(HistoryRecorder& rec, const HistoryRecorder::Snapshot& snapshot)
{
    for (int slot = 1; slot <= 99; ++slot)
        rec.snapshotStep(snapshot, slot);
}

template<class Work>
void write(const std::shared_ptr<HistoryRecorder>& rec, const fs::path& card,
           const std::string& kind, Work work)
{
    const auto options = history::makeWriteOptions(rec);
    rec->begin(options.opId, kind, card);
    try { work(options); }
    catch (const std::exception& error) { rec->finish(options.opId, error.what()); throw; }
    rec->finish(options.opId, "");
}
}

static int runTests()
{
    {
        Scratch tmp;
        const auto volumePath = cardAt(tmp.path);
        auto rec = recorderAt(tmp.path / "history");
        auto snapshot = newSighting(*rec, volumePath);
        rec->snapshotStep(snapshot, 4);
        rec->interruptSnapshot(snapshot, "disconnected");
        const auto cardId = *rec->store().selectedCard();
        rec->store().forgetSlot(cardId, 4, 2000);
        rec->disconnect();
        snapshot = newSighting(*rec, volumePath);
        complete(*rec, snapshot);
        CHECK(rec->store().slotTimeline(4).empty());
        CHECK_EQ(rec->store().touchedSlots(snapshot.op).size(), 98u);
        CHECK_EQ(rec->store().snapshotSlots(snapshot.op).size(), 99u);
        CHECK(!rec->firstSeen(volumePath));
    }

    // All 99 slots, empty and occupied, have one baseline with exact bodies
    // and bytes. Restore uses the same state and core command as the slot tab.
    {
        Scratch tmp;
        const auto card = cardAt(tmp.path);
        std::map<int, std::string> takes;
        for (int slot : { 1, 4, 37, 99 }) takes[slot] = putTake(card, slot, 132300 + slot);
        const auto before = commands::readMemory(card);
        auto rec = recorderAt(tmp.path / "history");
        CHECK(!marker::read(card));
        const auto snapshot = rec->firstSeen(card);
        CHECK(snapshot.has_value());
        const auto marker = marker::read(card);
        CHECK(marker.has_value());
        CHECK_EQ(snapshot->markerId, marker->id);
        complete(*rec, *snapshot);
        auto& store = rec->store();
        CHECK_EQ(number(store, "SELECT count(*) FROM cards"), 1);
        CHECK_EQ(number(store, "SELECT count(*) FROM ops WHERE kind = 'snapshot' AND actor = 'app' AND status = 'done'"), 1);
        CHECK_EQ(number(store, "SELECT count(*) FROM slot_changes WHERE before_body IS NULL"), 99);
        CHECK_EQ(number(store, "SELECT count(*) FROM slot_audio WHERE side = 'before'"), 0);
        CHECK_EQ(number(store, "SELECT count(*) FROM slot_audio WHERE side = 'after'"), 4);
        CHECK(!store.offeredTargets().undo);
        std::int64_t expectedBytes = 0;
        for (int slot = 1; slot <= 99; ++slot) {
            const auto timeline = store.slotTimeline(slot);
            CHECK_EQ(timeline.size(), 1u);
            const auto& entry = timeline.front();
            CHECK_EQ(entry.kind, std::string("snapshot"));
            CHECK(!entry.beforeBody);
            CHECK(entry.afterBody == rc0::slotBody(before, slot));
            const auto rows = history::rows::forSlot(timeline);
            CHECK_EQ(rows.front().line.action, std::string("Card first seen"));
            if (takes.contains(slot)) {
                CHECK_EQ(entry.takeName, std::string("Original.wav"));
                CHECK(entry.takeHash == HistoryStore::contentHash(takes[slot]));
                CHECK(entry.takeKept);
                CHECK(store.takeBytes(*entry.takeHash) == takes[slot]);
                CHECK_EQ(rows.front().line.detail, std::string("Original.wav"));
                CHECK_EQ(rows.front().line.audio, std::string("take kept"));
                expectedBytes += static_cast<std::int64_t>(takes[slot].size());
            } else {
                CHECK(!entry.takeHash);
                CHECK(entry.takeName.empty());
            }
        }
        CHECK_EQ(store.usage().audioBytes, expectedBytes);
        CHECK_EQ(store.keptBlobs(store.offeredTargets()).size(), 4u);
        const auto cardRows = history::rows::forCard(store.cardTimeline());
        CHECK_EQ(cardRows.size(), 1u);
        CHECK_EQ(cardRows.front().action, std::string("Card first seen"));
        CHECK_EQ(cardRows.front().takes.size(), 99u);
        CHECK_EQ(cardRows.front().detail, std::string("4 takes, ") + history::retention::bytesText(expectedBytes));

        const auto source = tmp.path / "Incoming.wav";
        commands::writeFileBytes(source, takes[99]);
        for (int slot : { 4, 8 }) {
            write(rec, card, "push", [&](const auto& options) {
                commands::push(card, source, slot, { .force = true, .write = options });
            });
            const auto timeline = store.slotTimeline(slot);
            CHECK(history::rows::forSlot(timeline).front().restorable);
            const auto row = history::rows::forCard(store.cardTimeline()).front();
            CHECK(row.restorable());
            const auto eligible = row.restorableSlots();
            CHECK(std::find(eligible.begin(), eligible.end(), slot) != eligible.end());
            CHECK(std::find(eligible.begin(), eligible.end(), 99) == eligible.end());
            write(rec, card, "restore", [&](const auto& options) {
                history::restoreOperation(store, snapshot->op, card, options, slot);
            });
            CHECK_EQ(rc0::slotBody(commands::readMemory(card), slot), rc0::slotBody(before, slot));
            if (slot == 4) {
                CHECK_EQ(volume::listSlotWavs(card, slot).size(), 1u);
                CHECK_EQ(commands::readFileBytes(volume::wavDir(card, slot) / "Original.wav"), takes[slot]);
            } else CHECK(volume::listSlotWavs(card, slot).empty());
        }
        rec->disconnect();
        CHECK(!rec->firstSeen(card));
        CHECK_EQ(number(store, "SELECT count(*) FROM ops WHERE kind = 'snapshot'"), 1);
        // Release a snapshot-only take while it is still the newest state.
        const auto hash = HistoryStore::contentHash(takes[37]);
        const auto beforeRelease = store.usage().audioBytes;
        CHECK_EQ(store.releaseBlobs({ hash }, store.offeredTargets(), 9999),
                 static_cast<std::int64_t>(takes[37].size()));
        const auto released = history::rows::forSlot(store.slotTimeline(37)).front();
        CHECK_EQ(released.line.audio, std::string("take no longer kept"));
        CHECK(!released.playable);
        CHECK_EQ(store.usage().audioBytes, beforeRelease - static_cast<std::int64_t>(takes[37].size()));
        rec.reset();
        rec = recorderAt(tmp.path / "history");
        CHECK(!rec->firstSeen(card));
        CHECK_EQ(number(rec->store(), "SELECT count(*) FROM ops WHERE kind = 'snapshot'"), 1);
    }

    // Same volume label, same marker name, different ids: independent slot,
    // operation, undo and card timelines, including a replacement at one path.
    {
        Scratch tmp;
        const auto a = cardAt(tmp.path / "a");
        const auto b = cardAt(tmp.path / "b");
        const auto aBytes = putTake(a, 1, 1111);
        const auto bBytes = putTake(b, 1, 2222);
        auto rec = recorderAt(tmp.path / "history");
        const auto first = newSighting(*rec, a);
        complete(*rec, first);
        const auto second = newSighting(*rec, b);
        complete(*rec, second);
        CHECK(first.markerId != second.markerId);
        CHECK_EQ(marker::read(a)->name, marker::read(b)->name);
        CHECK_EQ(number(rec->store(), "SELECT count(*) FROM cards"), 2);
        CHECK_EQ(rec->store().slotTimeline(1).size(), 1u);
        CHECK(rec->store().slotTimeline(1).front().takeHash == HistoryStore::contentHash(bBytes));
        CHECK_EQ(rec->store().cardTimeline().size(), 1u);
        CHECK_EQ(rec->store().operations().size(), 1u);
        CHECK(!rec->firstSeen(a));
        CHECK(rec->store().slotTimeline(1).front().takeHash == HistoryStore::contentHash(aBytes));
        const auto renamed = marker::rename(a, "Alice's card").card;
        CHECK(!rec->firstSeen(a));
        sqlite::Statement name(rec->store().db(), "SELECT name, model FROM cards WHERE marker_id = ?1");
        name.bindText(1, renamed.id);
        CHECK(name.step());
        CHECK_EQ(name.text(0), renamed.name);
        CHECK_EQ(name.text(1), renamed.model);
        pedalbook::Book book;
        book.remember({ "endpoint-a", first.markerId, renamed.name, renamed.model, 1000 });
        book.remember({ "endpoint-b", second.markerId, marker::read(b)->name, "RC-5", 1000 });
        const auto saved = pedalbook::Book::parse(book.serialize());
        CHECK_EQ(saved.find("endpoint-a")->cardId, first.markerId);
        CHECK_EQ(saved.find("endpoint-a")->name, renamed.name);
        CHECK_EQ(saved.find("endpoint-b")->cardId, second.markerId);
        // The mounted path stays the same, but now holds the other card.
        fs::rename(a, tmp.path / "away");
        fs::rename(b, a);
        CHECK(!rec->firstSeen(a));
        CHECK(rec->store().slotTimeline(1).front().takeHash == HistoryStore::contentHash(bBytes));
        CHECK_EQ(number(rec->store(), "SELECT count(*) FROM sessions WHERE disconnected_at IS NULL"), 1);
    }

    // An Undo queued on one card cannot apply to a replacement card when
    // both mount under the same path. The worker resolves identity again.
    {
        Scratch tmp;
        const auto a = cardAt(tmp.path / "a");
        const auto b = cardAt(tmp.path / "b");
        auto rec = recorderAt(tmp.path / "history");
        complete(*rec, newSighting(*rec, a));
        write(rec, a, "rename", [&](const auto& options) {
            commands::rename(a, 3, "Changed", options);
        });
        const auto target = *rec->store().offeredTargets().undo;
        fs::rename(a, tmp.path / "away");
        fs::rename(b, a);
        CHECK_THROWS(history::undo::beginPress(*rec, "stale-undo", false, target, a), "history moved on");
        CHECK_EQ(number(rec->store(), "SELECT count(*) FROM ops WHERE id = 'stale-undo'"), 0);
        CHECK(rec->store().slotTimeline(3).empty());
    }

    // A lifecycle refusal after N steps marks the SAME operation interrupted.
    // Already recorded slots retain their original state across resume, even
    // if the card changed while absent. Unreached slots are recorded once.
    for (bool restartApp : { false, true }) {
        Scratch tmp;
        const auto card = cardAt(tmp.path);
        const auto original = putTake(card, 1, 1234);
        auto rec = recorderAt(tmp.path / "history");
        const auto baseline = newSighting(*rec, card);
        auto run = std::make_shared<history::FirstSeenRun>(baseline);
        for (int slot = 1; slot <= 13; ++slot) {
            int progress = 0;
            auto job = history::firstSeenJob(rec, run, slot, [&](int count, const auto& error) {
                CHECK(error.empty()); progress = count;
            });
            CHECK(job.background && job.quiet && job.needsVolume);
            job.work(card); job.after("");
            CHECK_EQ(progress, slot);
        }
        auto refused = history::firstSeenJob(rec, run, 14, [](int, const auto&) {});
        refused.after("pedal is disconnected"); // worker gate: work never ran
        CHECK_EQ(rec->store().opStatus(baseline.op), std::string("interrupted"));
        CHECK_EQ(rec->store().touchedSlots(baseline.op).size(), 13u);
        putTake(card, 1, 4321);
        if (restartApp) { rec.reset(); rec = recorderAt(tmp.path / "history"); }
        else rec->disconnect();
        const auto resumed = newSighting(*rec, card);
        CHECK_EQ(resumed.op, baseline.op);
        complete(*rec, resumed);
        CHECK_EQ(rec->store().opStatus(baseline.op), std::string("done"));
        CHECK_EQ(number(rec->store(), "SELECT count(*) FROM ops"), 1);
        CHECK_EQ(number(rec->store(), "SELECT count(*) FROM slot_changes"), 99);
        CHECK_EQ(number(rec->store(), "SELECT count(*) FROM slot_audio"), 1);
        CHECK(rec->store().slotTimeline(1).front().takeHash == HistoryStore::contentHash(original));
    }

    // Foreground commands overtaking the background work must preserve the
    // slots they touch BEFORE either their bodies or their audio changes.
    {
        Scratch tmp;
        const auto card = cardAt(tmp.path);
        const auto original = putTake(card, 90, 132300);
        const auto before = commands::readMemory(card);
        auto rec = recorderAt(tmp.path / "history");
        const auto baseline = newSighting(*rec, card);
        rec->snapshotStep(baseline, 1);
        const auto source = tmp.path / "New.wav";
        commands::writeFileBytes(source, original);
        write(rec, card, "push", [&](const auto& options) {
            commands::push(card, source, 80, { .write = options });
        });
        write(rec, card, "rename", [&](const auto& options) {
            commands::rename(card, 70, "Changed", options);
        });
        write(rec, card, "swap", [&](const auto& options) {
            commands::swap(card, 90, 91, options);
        });
        complete(*rec, baseline);
        for (int slot : { 70, 80, 90, 91 }) {
            const auto entry = rec->store().slotTimeline(slot).front();
            CHECK_EQ(entry.op, baseline.op);
            CHECK(entry.afterBody == rc0::slotBody(before, slot));
            if (slot == 90) {
                CHECK(entry.takeHash == HistoryStore::contentHash(original));
                CHECK(rec->store().takeBytes(*entry.takeHash) == original);
            } else CHECK(entry.takeName.empty());
        }
    }

    // Body, audio references and bytes commit together. A failed second take
    // must not leave a body that resume mistakes for a completed slot.
    {
        Scratch tmp;
        auto rec = recorderAt(tmp.path / "history");
        const auto card = cardAt(tmp.path);
        const auto baseline = newSighting(*rec, card);
        CHECK_THROWS(rec->store().snapshotSlot(baseline.op, 1, "body",
           { { 1, "good.wav", "bytes" }, { 0, "bad.wav", "other bytes" } }, 2000), "CHECK");
        CHECK(rec->store().touchedSlots(baseline.op).empty());
        CHECK_EQ(rec->store().usage().audioBytes, 0);
        CHECK_EQ(number(rec->store(), "SELECT count(*) FROM blobs_meta"), 0);
        fs::remove(marker::markerPath(card));
        marker::mint(card, "RC-5");
        CHECK_THROWS(rec->snapshotStep(baseline, 1), "card changed");
        CHECK(rec->store().touchedSlots(baseline.op).empty());
    }
    // R1: a restarted store with no selected card returns no history.
    {
        Scratch scratch;
        const auto tmp = scratch.path;
        const auto a = cardAt(tmp / "a");
        const auto b = cardAt(tmp / "b");
        putTake(a, 1, 1111);
        putTake(b, 1, 2222);
       {
            auto rec = recorderAt(tmp / "history");
            complete(*rec, *rec->firstSeen(a));
            complete(*rec, *rec->firstSeen(b));
        }
        auto restarted = recorderAt(tmp / "history");
        const auto slot1 = restarted->store().slotTimeline(1);
        std::printf("R1 no-selection: slot 1 rows = %zu, card rows = %zu, ops = %zu\n",
                    slot1.size(), restarted->store().cardTimeline().size(),
                    restarted->store().operations().size());
        CHECK_EQ(slot1.size(), 0u); // no implicit all-card timeline
        CHECK_EQ(restarted->store().cardTimeline().size(), 0u);
        CHECK(restarted->store().operations().empty());
    }

    // R4: name and size never borrow another card's hash.
    {
        Scratch scratch;
        const auto tmp = scratch.path;
        const auto a = cardAt(tmp / "a");
        const auto b = cardAt(tmp / "b");
        const auto aBytes = putTake(a, 1, 5000, 1);
        const auto bBytes = putTake(b, 1, 5000, 2);
        CHECK_EQ(aBytes.size(), bBytes.size());
        auto rec = recorderAt(tmp / "history");
        const auto sb = *rec->firstSeen(b);
        complete(*rec, sb);
        const auto sa = *rec->firstSeen(a);
        complete(*rec, sa);
        // A newer op on card A names the same slot/name/size with A's bytes.
        write(rec, a, "rename", [&](const auto& options) { commands::rename(a, 1, "A one", options); });
        write(rec, b, "rename", [&](const auto& options) { commands::rename(b, 1, "B one", options); });
        const auto timeline = rec->store().slotTimeline(1); // card B selected
        CHECK_EQ(timeline.size(), 2u);
        CHECK(timeline.back().takeHash == HistoryStore::contentHash(bBytes));
    }

    // R9: another mounted card cannot release the bytes needed by Undo.
    {
        Scratch scratch;
        const auto tmp = scratch.path;
        const auto a = cardAt(tmp / "a");
        const auto b = cardAt(tmp / "b");
        const auto ta = putTake(a, 4, 4000, 5);
        auto rec = recorderAt(tmp / "history");
        complete(*rec, *rec->firstSeen(a));
        const auto source = tmp / "In.wav";
        commands::writeFileBytes(source, putTake(cardAt(tmp / "other"), 1, 88200));
        write(rec, a, "push", [&](const auto& options) {
            commands::push(a, source, 4, { .force = true, .write = options });
        });
        const auto undoA = rec->store().offeredTargets().undo;
        CHECK(undoA.has_value());
        rec->disconnect();
        complete(*rec, *rec->firstSeen(b)); // card B mounted now
        const auto hash = HistoryStore::contentHash(ta);
        bool offered = false, held = false;
        for (const auto& blob : rec->store().keptBlobs(rec->store().offeredTargets()))
            if (blob.hash == hash) { offered = true; held = blob.held(); }
        std::printf("R9 A's undo take listed=%d held=%d while B is mounted\n", offered, held);
        CHECK(held); // A's Undo needs it
        try { rec->store().releaseBlobs({ hash }, rec->store().offeredTargets(), 9999); }
        catch (const Error& e) { std::printf("R9 release refused: %s\n", e.what()); }
        rec->disconnect();
        CHECK(!rec->firstSeen(a)); // back to card A
        CHECK(rec->store().offeredTargets().undo == undoA);
        if (undoA) {
            try {
                const auto checked = history::undo::beginPress(*rec, "undo-a", false, *undoA, a);
                history::undo::apply(rec->store(), checked.plan, a,
                    history::withHistory(rec, { .opId = "undo-a" }));
                rec->finish("undo-a", "", checked.note);
                CHECK_EQ(commands::readFileBytes(volume::wavDir(a, 4) / "Original.wav"), ta);
                std::printf("R9 A's undo still possible\n");
            } catch (const Error& e) {
                std::printf("R9 A's undo refused: %s\n", e.what());
                CHECK(false);
            }
        }
    }
    // R3: clear and trim overtaking the snapshot keep their original takes.
    {
        Scratch scratch;
        const auto tmp = scratch.path;
        const auto card = cardAt(tmp);
        const auto t40 = putTake(card, 40, 44100);
        const auto t41 = putTake(card, 41, 44100, 7);
        const auto t50 = putTake(card, 50, 132300);
        const auto before = commands::readMemory(card);
        auto rec = recorderAt(tmp / "history");
        const auto baseline = *rec->firstSeen(card);
        rec->snapshotStep(baseline, 1);
        write(rec, card, "clear", [&](const auto& options) {
            commands::clear(card, { 40, 41 }, { .write = options });
        });
        write(rec, card, "trim", [&](const auto& options) {
            commands::trim(card, 50, 0, 66150, { .write = options });
        });
        complete(*rec, baseline);
        const std::map<int, std::string> originals { { 40, t40 }, { 41, t41 }, { 50, t50 } };
        for (const auto& [slot, bytes] : originals) {
            const auto entry = rec->store().slotTimeline(slot).front();
            CHECK_EQ(entry.kind, std::string("snapshot"));
            CHECK(entry.afterBody == rc0::slotBody(before, slot));
            CHECK(entry.takeHash == HistoryStore::contentHash(bytes));
            CHECK(entry.takeHash && rec->store().takeBytes(*entry.takeHash) == bytes);
        }
    }

    // R8: a vanished marker starts a new baseline before the next write.
    {
        Scratch scratch;
        const auto tmp = scratch.path;
        const auto card = cardAt(tmp);
        const auto t5 = putTake(card, 5, 2000, 3);
        auto rec = recorderAt(tmp / "history");
        complete(*rec, *rec->firstSeen(card));
        fs::remove(marker::markerPath(card));
        const auto source = tmp / "In.wav";
        commands::writeFileBytes(source, putTake(cardAt(tmp / "other"), 1, 88200));
        write(rec, card, "push", [&](const auto& options) {
            commands::push(card, source, 5, { .force = true, .write = options });
        });
        const auto again = rec->firstSeen(card);
        CHECK(again.has_value());
        if (again) complete(*rec, *again);
        const auto timeline = rec->store().slotTimeline(5);
        std::printf("R8 remint: cards = %lld, slot 5 rows:", static_cast<long long>(number(rec->store(), "SELECT count(*) FROM cards")));
        for (const auto& e : timeline) std::printf(" %s", e.kind.c_str());
        std::printf("\n");
        CHECK(!timeline.empty() && timeline.front().kind == "snapshot");
        CHECK(!timeline.empty() && timeline.front().takeHash == HistoryStore::contentHash(t5));
    }
    // An oversized slot is a durable failed row; the worker continues and reconnect does not retry.
    {
        Scratch tmp;
        const auto card = cardAt(tmp.path);
        putTake(card, 2, 20000);
        const auto good = putTake(card, 3, 1000);
        auto rec = recorderAt(tmp.path / "history");
        const auto baseline = newSighting(*rec, card);
        const auto oldLimit = sqlite3_limit(rec->store().db().raw(), SQLITE_LIMIT_LENGTH, 100000);
        auto run = std::make_shared<history::FirstSeenRun>(baseline);
        for (int slot = 1; slot <= 99; ++slot) {
            auto job = history::firstSeenJob(rec, run, slot, [slot](int count, const auto& error) {
                CHECK(error.empty()); CHECK_EQ(count, slot);
            });
            job.work(card); job.after("");
        }
        sqlite3_limit(rec->store().db().raw(), SQLITE_LIMIT_LENGTH, oldLimit);
        const auto failed = rec->store().slotTimeline(2).front();
        CHECK_EQ(failed.status, std::string("failed"));
        CHECK(failed.note.find("size limit") != std::string::npos);
        CHECK(!failed.afterBody);
        CHECK(history::rows::forSlot({failed}).front().line.detail.find("size limit") != std::string::npos);
        CHECK(!history::rows::forSlot({failed}).front().restorable);
        CHECK(history::rows::forCard(rec->store().cardTimeline()).front().state.find("slot 2 failed: take Original.wav exceeds") != std::string::npos);
        CHECK(rec->store().slotTimeline(3).front().takeHash == HistoryStore::contentHash(good));
        CHECK_EQ(rec->store().opStatus(baseline.op), std::string("done"));
        rec->disconnect();
        CHECK(!rec->firstSeen(card));
        rec.reset(); rec = recorderAt(tmp.path / "history");
        CHECK(!rec->firstSeen(card));
        CHECK_EQ(rec->store().slotTimeline(2).size(), 1u);
        CHECK_EQ(rec->store().slotTimeline(2).front().note, failed.note);
    }
    // Audio-only rewrites must preserve the slot even when no body fields change.
    for (const std::string kind : { "normalize", "downmix" }) {
        Scratch tmp;
        const auto card = cardAt(tmp.path);
        auto bytes = putTake(card, 60, 44100);
        const auto dataStart = wav::detail::dataChunkStart({ reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size() });
        for (int frame = 0; frame < 44100; ++frame) {
            const auto value = static_cast<float>(0.1 * std::sin(frame * 0.142));
            for (int channel = 0; channel < 2; ++channel) {
                const auto bits = std::bit_cast<std::uint32_t>(channel == 0 ? value : value * 0.5f);
                for (int byte = 0; byte < 4; ++byte)
                    bytes[dataStart + static_cast<std::size_t>(frame * 8 + channel * 4 + byte)]
                        = static_cast<char>((bits >> (byte * 8)) & 255);
            }
        }
        commands::writeFileBytes(volume::wavDir(card, 60) / "Original.wav", bytes);
        const auto before = rc0::slotBody(commands::readMemory(card), 60);
        auto rec = recorderAt(tmp.path / "history");
        const auto baseline = newSighting(*rec, card);
        rec->snapshotStep(baseline, 1);
        write(rec, card, kind, [&](const auto& options) {
            if (kind == "normalize") CHECK(commands::normalize(card, 60, { .targetLufs = -30, .write = options }).applied);
            else commands::downmixToMono(card, 60, { .write = options });
        });
        CHECK(commands::readFileBytes(volume::wavDir(card, 60) / "Original.wav") != bytes);
        complete(*rec, baseline);
        const auto first = rec->store().slotTimeline(60).front();
        CHECK(first.afterBody == before);
        CHECK(first.takeHash == HistoryStore::contentHash(bytes));
        CHECK(first.takeHash && rec->store().takeBytes(*first.takeHash) == bytes);
    }
    return testkit::summary("first_seen_tests");
}

int main()
{
    try { return runTests(); }
    catch (const std::exception& error) {
        testkit::fail(error.what(), __FILE__, __LINE__);
        return testkit::summary("first_seen_tests");
    }
}
