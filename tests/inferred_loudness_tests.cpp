// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What the history can say about a take it meets again without reading it
// (#141), end to end on a synthetic pedal: real core commands recorded through
// history::withHistory as the app records them, readings filed as Check
// loudness files them, and the takes then sighted the way the connect scan
// sights them (TakeFacts.h) — name, size and stamp off the directory entry,
// WavLen off the config. What must hold, and what these try to break:
//
//   - a take the history measured is recognised on sight and its reading
//     comes back as it was taken; a take it never measured comes back as
//     nothing — a dash stays a dash, never a guess
//   - asking writes nothing: no operation, no row, no reading, no take
//   - a rename leaves the take what it was: still recognised
//   - a swap moves a take that keeps its own name, and its reading moves
//     with it; it renames a take under the pedal's own name (NNN_1.WAV) to
//     its new address, and the history cannot follow that one; the slot a
//     take left knows nothing about it any more
//   - a trim leaves bytes nobody measured: nothing, and nothing for the old
//     facts either — until a read measures the new take
//   - a card the history never met, and another card, know nothing about
//     this card's takes
//   - the same bytes pushed to another slot carry their reading there
//   - a slot the history saw emptied (a clear) answers nothing for the facts
//     it had before, even met again exactly: the pedal's clock does not tell
//     two of its recordings of one length apart
//   - the limit, and why it is harmless (#142): a different take under the
//     very same name, size, stamp and WavLen fools the inference — and the
//     store still knows nothing about the bytes really there, so a step that
//     reads and hashes them must measure, and normalize, which reads them,
//     decides by them against a target the guess says is already met — and
//     hands what it measured on through the recorder, once, for its slot
//   - and a real read corrects it (review S3): after Check loudness reads the
//     new bytes under the same facts, the next inference answers their
//     reading, never the old one — nor after an operation that followed the
//     read (S3d: a rename hands the read's hash on) — and a read whose bytes
//     have no reading answers nothing
//   - reads are placed among the operations by sequence, never by clock: a
//     first sighting resumed after a read photographs the slot anew (S7); a
//     player read filed after an operation that ran during it does not
//     outrank that operation (S8); a clock set back between a read and a
//     clear does not let the read outlive the clear (S10)
//   - a migrated history (rows with no stamp) never infers through a row it
//     vouched for by name and size: a file overwritten outside the app with
//     one of the same size, then renamed, is not read as the old take
//     (review S1), while the slot stays restorable; on a v9 history the
//     same steps carry no hash at all
//   - a slot whose recorded body cannot be read answers nothing and is
//     reported by its slot; the other slots are still answered

#include "support.hpp"

#include "../app/TakeFacts.h"
#include "../app/history/InferredLoudness.h"
#include "../app/history/SlotLoudness.h"
#include "../app/history/WriteOptionsFactory.h"

#include <loopercat/Catalog.hpp>
#include <loopercat/Commands.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
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
        path = fs::temp_directory_path() / ("loopercat-inferred-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        // The store is closed by now (declared after the directory, gone
        // before it); a directory that will not go is a failure, not a leak.
        std::error_code ec;
        fs::remove_all(path, ec);
        CHECK(!ec);
    }
};

fs::path makePedal(const fs::path& root)
{
    const fs::path volume = root / "BOSS RC-5";
    fs::create_directories(volume / "ROLAND" / "WAVE");
    fs::create_directories(volume::dataDir(volume));
    const std::string text = testkit::syntheticMemoryText();
    for (const int fileNo : { 1, 2 })
        commands::writeFileBytes(volume::memoryPath(volume, fileNo),
                                 rc0::setTailMarker(text, fileNo));
    return volume;
}

// A float32 stereo 997 Hz sine at `dbfs` peak — the pedal's own shape, and
// the tone BS.1770 calibrates on, so a reading can be told from another.
void writeSine(const fs::path& file, int frames, double dbfs)
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
    commands::writeFileBytes(file, std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
}

// What the pedal worker does around every recorded job: open, run, close with
// the outcome.
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

commands::WriteOptions options(const std::shared_ptr<HistoryRecorder>& rec, const std::string& opId)
{
    return history::withHistory(rec, { .opId = opId });
}

std::int64_t count(sqlite::Db& db, const std::string& sql)
{
    sqlite::Statement read(db, sql);
    if (!read.step())
        throw Error("count returned no row: " + sql);
    return read.integer(0);
}

// Everything the store holds, counted: the proof that asking wrote nothing.
std::array<std::int64_t, 5> rows(sqlite::Db& db)
{
    return { count(db, "SELECT count(*) FROM ops"), count(db, "SELECT count(*) FROM slot_audio"),
             count(db, "SELECT count(*) FROM slot_changes"),
             count(db, "SELECT count(*) FROM loudness_readings"),
             count(db, "SELECT count(*) FROM blobs") };
}

// A slot as the connect scan sights it: the first take's entry, and the
// config's WavLen — without reading the take.
history::SlotSighting sight(const fs::path& volume, int slot)
{
    const auto files = volume::listSlotWavs(volume, slot);
    if (files.empty())
        throw Error("slot " + std::to_string(slot) + " has no take to sight");
    const auto facts = takeFacts(volume::wavDir(volume, slot) / files.front());
    if (!facts)
        throw Error("the entry of slot " + std::to_string(slot) + "'s take gave no facts");
    const catalog::SlotInfo info = catalog::readSlot(commands::readMemory(volume), slot);
    return { slot, { facts->name, facts->size, facts->modifiedMs, info.frames } };
}

// A file's stamp set the way the platform reports it back (FileTime.h).
void setStamp(const fs::path& file, std::int64_t ms)
{
    const std::u8string utf8 = file.u8string();
    const juce::File entry(juce::String::fromUTF8(reinterpret_cast<const char*>(utf8.data()),
                                                  static_cast<int>(utf8.size())));
    if (!entry.setLastModificationTime(juce::Time(ms)))
        throw Error("cannot set the modification time of " + file.string());
}

bool same(const wav::LoudnessReading& a, const wav::LoudnessReading& b)
{
    return a.integratedLufs.has_value() == b.integratedLufs.has_value()
        && (!a.integratedLufs || std::abs(*a.integratedLufs - *b.integratedLufs) <= 1.0e-12)
        && std::abs(a.samplePeak - b.samplePeak) <= 1.0e-12
        && std::abs(a.truePeakDb - b.truePeakDb) <= 1.0e-12
        && a.wildSamples == b.wildSamples;
}

} // namespace

int main()
{
    TempDir tmp;
    const fs::path volume = makePedal(tmp.path);
    auto rec = std::make_shared<HistoryRecorder>(tmp.path / "history", tick);

    // Two takes land through the app's own wiring: one is then measured the
    // way Check loudness measures it, the other never is.
    const fs::path loud = tmp.path / "loud.wav";
    const fs::path quiet = tmp.path / "quiet.wav";
    writeSine(loud, 88200, -20.0);
    writeSine(quiet, 88200, -28.0);
    CHECK_EQ(run(*rec, "op-push-3", "push", volume, [&] {
                 commands::push(volume, loud, 3, { .write = options(rec, "op-push-3") });
             }),
             std::string());
    CHECK_EQ(run(*rec, "op-push-4", "push", volume, [&] {
                 commands::push(volume, quiet, 4, { .write = options(rec, "op-push-4") });
             }),
             std::string());
    HistoryStore& store = rec->store();
    const std::int64_t card = *store.selectedCard();
    const history::SlotLoudness measured = history::readSlotLoudness(volume, 3, *rec);
    CHECK(measured.kept);
    CHECK(measured.reading.integratedLufs.has_value());

    // --- the measured take is recognised on sight; the other is nothing ---
    {
        const auto before = rows(store.db());
        const auto found = history::inferLoudness(store, card, { sight(volume, 3), sight(volume, 4) }).found;
        CHECK_EQ(found.size(), 1u);
        if (!found.empty()) {
            CHECK_EQ(found.front().sighted.slot, 3);
            CHECK_EQ(found.front().sighted.take.name, std::string("loud.wav")); // the sighting it answers
            CHECK(same(found.front().reading, measured.reading));
            CHECK(found.front().measuredMs > 1'000'000); // the clock's own time, not a default
        }
        // asking wrote nothing anywhere
        CHECK(rows(store.db()) == before);
        // and the sighting itself read no audio: the facts are the entry's
        const auto sighted = sight(volume, 3);
        CHECK_EQ(sighted.take.name, std::string("loud.wav")); // a pushed take keeps its own name
        CHECK_EQ(sighted.take.size,
                 static_cast<std::int64_t>(fs::file_size(volume::wavDir(volume, 3) / "loud.wav")));
        CHECK_EQ(sighted.take.frames, 88200);
    }

    // --- a card never met, and another card: nothing about this one's takes ---
    {
        CHECK(!store.cardFor("never-met").has_value());
        const auto other = store.card("other-card", "RC-5", "Other", 5);
        CHECK(history::inferLoudness(store, other, { sight(volume, 3) }).found.empty());
        store.selectCard(card);
    }

    // --- a rename leaves the take what it was ---
    {
        CHECK_EQ(run(*rec, "op-rename", "rename", volume, [&] {
                     commands::rename(volume, 3, "Intro", options(rec, "op-rename"));
                 }),
                 std::string());
        const auto found = history::inferLoudness(store, card, { sight(volume, 3) }).found;
        CHECK_EQ(found.size(), 1u);
        CHECK(!found.empty() && same(found.front().reading, measured.reading));
    }

    // --- a swap: a take under its own name moves with its reading ---
    {
        CHECK_EQ(run(*rec, "op-swap", "swap", volume, [&] {
                     commands::swap(volume, 3, 7, options(rec, "op-swap"));
                 }),
                 std::string());
        CHECK(volume::listSlotWavs(volume, 3).empty()); // the take went to 7
        const auto moved = sight(volume, 7);
        CHECK_EQ(moved.take.name, std::string("loud.wav"));
        const auto found = history::inferLoudness(store, card, { moved }).found;
        CHECK_EQ(found.size(), 1u);
        CHECK(!found.empty() && same(found.front().reading, measured.reading));
        // the slot it left: the swap is that slot's last word, whatever the facts
        history::SlotSighting left = moved;
        left.slot = 3;
        CHECK(history::inferLoudness(store, card, { left }).found.empty());
    }

    // --- a swap renames a take under the pedal's own name: nothing follows ---
    {
        const fs::path pedalNamed = tmp.path / "005_1.WAV"; // the name the pedal gives slot 5's take
        writeSine(pedalNamed, 88200, -24.0);
        CHECK_EQ(run(*rec, "op-push-5", "push", volume, [&] {
                     commands::push(volume, pedalNamed, 5, { .write = options(rec, "op-push-5") });
                 }),
                 std::string());
        const history::SlotLoudness five = history::readSlotLoudness(volume, 5, *rec);
        CHECK(five.kept);
        CHECK_EQ(history::inferLoudness(store, card, { sight(volume, 5) }).found.size(), 1u);
        CHECK_EQ(run(*rec, "op-swap-5-8", "swap", volume, [&] {
                     commands::swap(volume, 5, 8, options(rec, "op-swap-5-8"));
                 }),
                 std::string());
        const auto renamed = sight(volume, 8);
        CHECK_EQ(renamed.take.name, std::string("008_1.WAV")); // retitled to its new address
        CHECK(history::inferLoudness(store, card, { renamed }).found.empty());
        // the same bytes under the old name, asked of either slot: nothing
        history::SlotSighting asBefore = renamed;
        asBefore.take.name = "005_1.WAV";
        CHECK(history::inferLoudness(store, card, { asBefore }).found.empty());
        asBefore.slot = 5;
        CHECK(history::inferLoudness(store, card, { asBefore }).found.empty());
        // the reading itself is still in the history, under the bytes
        CHECK(store.readingFor(five.hash).has_value());
    }

    // --- a trim leaves bytes nobody measured: nothing, until a read measures them ---
    {
        const history::SlotSighting whole = sight(volume, 7);
        CHECK_EQ(run(*rec, "op-trim", "trim", volume, [&] {
                     commands::trim(volume, 7, 0, 44100, { .write = options(rec, "op-trim") });
                 }),
                 std::string());
        const history::SlotSighting trimmed = sight(volume, 7);
        CHECK_EQ(trimmed.take.frames, 44100);
        CHECK(trimmed.take.size < whole.take.size);
        CHECK(history::inferLoudness(store, card, { trimmed }).found.empty());
        CHECK(history::inferLoudness(store, card, { whole }).found.empty()); // the old facts: the newest row is the last word
        // a read measures the new take and files it: recognised from then on
        const history::SlotLoudness again = history::readSlotLoudness(volume, 7, *rec);
        CHECK(again.kept);
        const auto found = history::inferLoudness(store, card, { trimmed }).found;
        CHECK_EQ(found.size(), 1u);
        CHECK(!found.empty() && same(found.front().reading, again.reading));
        // and a stamp one step off is another file: nothing
        history::SlotSighting later = trimmed;
        later.take.modifiedMs += 2000; // FAT's step
        CHECK(history::inferLoudness(store, card, { later }).found.empty());
    }

    // --- the same bytes in another slot: their reading travels with them ---
    // --- then a clear, and the old facts met again: nothing ---
    {
        CHECK_EQ(run(*rec, "op-push-10", "push", volume, [&] {
                     commands::push(volume, loud, 10, { .write = options(rec, "op-push-10") });
                 }),
                 std::string());
        const history::SlotSighting pushed = sight(volume, 10);
        const auto found = history::inferLoudness(store, card, { pushed }).found;
        CHECK_EQ(found.size(), 1u); // never measured in slot 10, measured as bytes in slot 3
        CHECK(!found.empty() && same(found.front().reading, measured.reading));
        CHECK_EQ(run(*rec, "op-clear-10", "clear", volume, [&] {
                     commands::clear(volume, { 10 }, { .write = options(rec, "op-clear-10") });
                 }),
                 std::string());
        CHECK(volume::listSlotWavs(volume, 10).empty());
        // The slot is met again showing the very facts the row had — a copy
        // put back with its stamp, or the pedal recording one loop of the
        // same length off a clock that does not keep the date. The history
        // saw the slot emptied after its row: that row is not the slot's
        // word any more.
        CHECK(history::inferLoudness(store, card, { pushed }).found.empty());
    }

    // --- fooled, and harmless: the decision reads the bytes (#142) ---
    {
        CHECK_EQ(run(*rec, "op-push-11", "push", volume, [&] {
                     commands::push(volume, loud, 11, { .write = options(rec, "op-push-11") });
                 }),
                 std::string());
        const history::SlotSighting before = sight(volume, 11);
        // Another take under the very same facts: slot 4's quiet loop, the
        // same length and so the same size, written over slot 11's file
        // outside the app and stamped as the old one was. WavLen untouched.
        const fs::path take11 = volume::wavDir(volume, 11) / before.take.name;
        const std::string quietBytes = commands::readFileBytes(
            volume::wavDir(volume, 4) / volume::listSlotWavs(volume, 4).front());
        CHECK_EQ(static_cast<std::int64_t>(quietBytes.size()), before.take.size);
        commands::writeFileBytes(take11, quietBytes);
        setStamp(take11, before.take.modifiedMs);
        const history::SlotSighting after = sight(volume, 11);
        CHECK_EQ(after.take.name, before.take.name);
        CHECK_EQ(after.take.size, before.take.size);
        CHECK_EQ(after.take.modifiedMs, before.take.modifiedMs);
        CHECK_EQ(after.take.frames, before.take.frames);
        // The limit the issue names: the inference cannot tell, and says the
        // loud take's number for the quiet take's bytes.
        const auto fooled = history::inferLoudness(store, card, { after }).found;
        CHECK_EQ(fooled.size(), 1u);
        CHECK(!fooled.empty() && same(fooled.front().reading, measured.reading));
        // What a decision asks instead: the store, by the hash of the bytes
        // really in the slot — and it knows nothing about them.
        const std::string onCard = commands::readFileBytes(take11);
        const std::string onCardHash = HistoryStore::contentHash(onCard);
        CHECK(onCardHash != measured.hash);
        CHECK(!store.readingFor(onCardHash).has_value());
        // Normalize to the very loudness the guess shows: by the guess there
        // is nothing to do. It reads the bytes, finds the quiet loop, and
        // raises it.
        const double guessed = *measured.reading.integratedLufs;
        // and whoever shows the column is told what the command measured
        std::vector<std::pair<int, wav::LoudnessReading>> told;
        rec->onMeasured = [&told](int slot, const wav::LoudnessReading& reading) {
            told.emplace_back(slot, reading);
        };
        commands::NormalizeResult result;
        CHECK_EQ(run(*rec, "op-normalize-11", "normalize", volume, [&] {
                     result = commands::normalize(volume, 11,
                                                  { .targetLufs = guessed,
                                                    .write = options(rec, "op-normalize-11") });
                 }),
                 std::string());
        rec->onMeasured = nullptr;
        CHECK(result.applied);
        CHECK_EQ(told.size(), 1u);
        CHECK(!told.empty() && told.front().first == 11 && told.front().second.integratedLufs
              && std::abs(*told.front().second.integratedLufs - result.measuredLufs) <= 1.0e-9);
        CHECK(result.measuredLufs < guessed - 6.0); // -28 dBFS against -20: about 8 LU apart
        CHECK(result.gainDb > 6.0);
        // and the history learned the truth about those bytes from the bytes
        const auto learned = store.readingFor(onCardHash);
        CHECK(learned.has_value());
        CHECK(learned && learned->reading.integratedLufs
              && std::abs(*learned->reading.integratedLufs - result.measuredLufs) <= 1.0e-9);
        // while the loud take's reading stands as it was
        const auto loudStill = store.readingFor(measured.hash);
        CHECK(loudStill && same(loudStill->reading, measured.reading));
    }

    // --- a real read corrects the guess (review S3) ---
    {
        CHECK_EQ(run(*rec, "op-push-12", "push", volume, [&] {
                     commands::push(volume, loud, 12, { .write = options(rec, "op-push-12") });
                 }),
                 std::string());
        const history::SlotSighting before = sight(volume, 12);
        // the pedal records the slot again, the same length, off a clock that
        // does not keep the date: the same name, size, stamp and WavLen
        const fs::path take12 = volume::wavDir(volume, 12) / before.take.name;
        commands::writeFileBytes(take12, commands::readFileBytes(
                                             volume::wavDir(volume, 4) / volume::listSlotWavs(volume, 4).front()));
        setStamp(take12, before.take.modifiedMs);
        const history::SlotSighting again = sight(volume, 12);
        CHECK_EQ(again.take.modifiedMs, before.take.modifiedMs);
        CHECK_EQ(again.take.size, before.take.size);
        const auto guessed = history::inferLoudness(store, card, { again }).found;
        CHECK(guessed.size() == 1u && same(guessed.front().reading, measured.reading)); // fooled, as before
        // Check loudness reads the bytes: the new reading, and what the read saw
        const history::SlotLoudness checked = history::readSlotLoudness(volume, 12, *rec);
        CHECK(checked.kept);
        CHECK(checked.failure.empty());
        CHECK(checked.hash != measured.hash);
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM take_sightings WHERE slot = 12"), 1);
        // the next connect: the read's word, never the old take's
        const auto corrected = history::inferLoudness(store, card, { again }).found;
        CHECK_EQ(corrected.size(), 1u);
        CHECK(!corrected.empty() && same(corrected.front().reading, checked.reading));
        CHECK(!corrected.empty() && !same(corrected.front().reading, measured.reading));
        // S3d: an operation after the read — a rename, twice — hands the
        // read's word on; the old take does not come back
        for (const char* name : { "Renamed", "Again" }) {
            const std::string opId = std::string("op-rename-12-") + name;
            CHECK_EQ(run(*rec, opId, "rename", volume, [&] {
                         commands::rename(volume, 12, name, options(rec, opId));
                     }),
                     std::string());
            const auto renamed = history::inferLoudness(store, card, { sight(volume, 12) }).found;
            CHECK_EQ(renamed.size(), 1u);
            CHECK(!renamed.empty() && same(renamed.front().reading, checked.reading));
        }
        // a read whose bytes have no reading on file answers nothing — not the old take
        store.recordSighting(card, 12, again.take.name, again.take.size, again.take.modifiedMs,
                             HistoryStore::contentHash("bytes nobody measured"), store.newestOp(), tick());
        CHECK(history::inferLoudness(store, card, { again }).found.empty());
    }

    // --- a migrated history never infers through a row it vouched for (review S1) ---
    {
        CHECK_EQ(run(*rec, "op-push-13", "push", volume, [&] {
                     commands::push(volume, loud, 13, { .write = options(rec, "op-push-13") });
                 }),
                 std::string());
        // the push's row as a store older than version 9 left it: no stamp
        store.db().exec("UPDATE slot_audio SET modified = NULL WHERE slot = 13");
        // overwritten outside the app: another loop, the same size, a new stamp
        const history::SlotSighting pushed = sight(volume, 13);
        const fs::path take13 = volume::wavDir(volume, 13) / pushed.take.name;
        const std::string quietBytes =
            commands::readFileBytes(volume::wavDir(volume, 4) / volume::listSlotWavs(volume, 4).front());
        commands::writeFileBytes(take13, quietBytes);
        setStamp(take13, pushed.take.modifiedMs + 4000);
        CHECK_EQ(run(*rec, "op-rename-13", "rename", volume, [&] {
                     commands::rename(volume, 13, "Thirteen", options(rec, "op-rename-13"));
                 }),
                 std::string());
        // the rename's row: the old hash, vouched for by name and size so the
        // slot stays restorable — and no stamp beside it
        {
            sqlite::Statement row(store.db(), "SELECT a.hash, a.modified FROM slot_audio a JOIN ops o ON o.seq = a.op "
                                              "WHERE o.id = 'op-rename-13' AND a.slot = 13 AND a.side = 'after'");
            CHECK(row.step());
            CHECK(!row.isNull(0) && row.blob(0) == measured.hash);
            CHECK(row.isNull(1));
        }
        // and the connect does not read the quiet loop as the loud one
        CHECK(history::inferLoudness(store, card, { sight(volume, 13) }).found.empty());
        // the v9 control: the same steps on a stamped row carry no hash at all
        CHECK_EQ(run(*rec, "op-push-14", "push", volume, [&] {
                     commands::push(volume, loud, 14, { .write = options(rec, "op-push-14") });
                 }),
                 std::string());
        const history::SlotSighting stamped = sight(volume, 14);
        const fs::path take14 = volume::wavDir(volume, 14) / stamped.take.name;
        commands::writeFileBytes(take14, quietBytes);
        setStamp(take14, stamped.take.modifiedMs + 4000);
        CHECK_EQ(run(*rec, "op-rename-14", "rename", volume, [&] {
                     commands::rename(volume, 14, "Fourteen", options(rec, "op-rename-14"));
                 }),
                 std::string());
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM slot_audio a JOIN ops o ON o.seq = a.op "
                                   "WHERE o.id = 'op-rename-14' AND a.slot = 14 AND a.hash IS NULL "
                                   "AND a.modified IS NOT NULL"),
                 1);
        CHECK(history::inferLoudness(store, card, { sight(volume, 14) }).found.empty());
    }

    // --- one slot's unreadable record: that slot answers nothing, the others still answer ---
    {
        CHECK_EQ(run(*rec, "op-push-15", "push", volume, [&] {
                     commands::push(volume, loud, 15, { .write = options(rec, "op-push-15") });
                 }),
                 std::string());
        const history::SlotSighting fifteen = sight(volume, 15);
        CHECK_EQ(history::inferLoudness(store, card, { fifteen }).found.size(), 1u);
        store.db().exec("UPDATE slot_changes SET after_body = x'6e6f742061206d656d6f7279' WHERE slot = 15");
        const history::Inference both = history::inferLoudness(store, card, { fifteen, sight(volume, 7) });
        CHECK_EQ(both.problems.size(), 1u);
        CHECK(!both.problems.empty() && both.problems.front().slot == 15 && !both.problems.front().what.empty());
        CHECK_EQ(both.found.size(), 1u); // slot 7, read and measured after its trim
        CHECK(!both.found.empty() && both.found.front().sighted.slot == 7);
    }

    // The quiet loop as the card holds it, and what the history measured it at.
    const std::string quietOnCard =
        commands::readFileBytes(volume::wavDir(volume, 4) / volume::listSlotWavs(volume, 4).front());
    const std::optional<HistoryStore::StoredReading> quietReading =
        store.readingFor(HistoryStore::contentHash(quietOnCard));
    CHECK(quietReading.has_value());

    // --- a read before an interrupted first sighting resumes (review S7) ---
    // The baseline's operation keeps the sequence of the connect it began on;
    // a slot it photographs on a later connect is newer than a read between.
    {
        const fs::path src = tmp.path / "060_1.WAV";
        writeSine(src, 88200, -20.0);
        commands::WriteOptions bare; // the pedal recorded it: nothing told the history
        bare.journal.bodiesChanging = [](const std::vector<commands::SlotChange>&) {};
        commands::push(volume, src, 60, { .write = bare });
        const auto baseline = rec->firstSeen(volume); // still running, slot 60 not reached
        CHECK(baseline.has_value());
        const history::SlotSighting seen = sight(volume, 60);
        const history::SlotLoudness read60 = history::readSlotLoudness(volume, 60, *rec);
        CHECK(read60.kept);
        const auto before = history::inferLoudness(store, card, { seen }).found;
        CHECK(before.size() == 1u && same(before.front().reading, read60.reading)); // the read's word
        rec->disconnect(); // the baseline is interrupted before slot 60
        // the pedal records slot 60 again: the same length, name and frozen stamp
        const fs::path take60 = volume::wavDir(volume, 60) / "060_1.WAV";
        commands::writeFileBytes(take60, quietOnCard);
        setStamp(take60, seen.take.modifiedMs);
        CHECK_EQ(sight(volume, 60).take.modifiedMs, seen.take.modifiedMs);
        const auto resumed = rec->firstSeen(volume); // the next connect resumes it
        CHECK(resumed.has_value());
        if (resumed)
            rec->snapshotStep(*resumed, 60); // and photographs slot 60's bytes now
        CHECK_EQ(count(store.db(), "SELECT count(*) FROM take_sightings WHERE slot = 60"), 0);
        const auto after = history::inferLoudness(store, card, { sight(volume, 60) }).found;
        CHECK_EQ(after.size(), 1u);
        CHECK(!after.empty() && quietReading && same(after.front().reading, quietReading->reading));
        CHECK(!after.empty() && !same(after.front().reading, read60.reading));
    }

    // --- a read filed after an operation that ran during it (review S8) ---
    // The player's pass reads on its own thread; its filing is a background
    // job that a foreground operation jumps ahead of. Facts equal across the
    // operation (FAT's two-second step) are simulated by restamping.
    {
        const fs::path src = tmp.path / "s70.wav";
        writeSine(src, 88200, -20.0);
        CHECK_EQ(run(*rec, "op-push-70", "push", volume, [&] {
                     commands::push(volume, src, 70, { .write = options(rec, "op-push-70") });
                 }),
                 std::string());
        const fs::path take70 = volume::wavDir(volume, 70) / "s70.wav";
        const history::SlotSighting read = sight(volume, 70); // the pass's facts
        const std::string readHash = HistoryStore::contentHash(commands::readFileBytes(take70));
        const std::optional<std::int64_t> began = rec->newestOp(); // where the pass's read began
        CHECK(began.has_value());
        // the foreground operation: another take under the same name and size
        const fs::path srcQuiet = tmp.path / "q70" / "s70.wav";
        fs::create_directories(srcQuiet.parent_path());
        writeSine(srcQuiet, 88200, -28.0);
        CHECK_EQ(run(*rec, "op-push-70b", "push", volume, [&] {
                     commands::push(volume, srcQuiet, 70, { .force = true, .write = options(rec, "op-push-70b") });
                 }),
                 std::string());
        setStamp(take70, read.take.modifiedMs); // the write landed in the same two-second step
        sqlite::Statement same70(store.db(), "UPDATE slot_audio SET modified = ?1 WHERE slot = 70 AND side = 'after' "
                                             "AND op = (SELECT max(op) FROM slot_audio WHERE slot = 70)");
        same70.bind(1, read.take.modifiedMs).run();
        const history::SlotSighting now70 = sight(volume, 70);
        CHECK(now70.take.name == read.take.name && now70.take.size == read.take.size
              && now70.take.modifiedMs == read.take.modifiedMs);
        // the pass's filing job runs now, after the operation
        CHECK(began && rec->sighted(volume, 70, read.take.name, read.take.size, read.take.modifiedMs,
                                    readHash, *began));
        const auto found = history::inferLoudness(store, card, { sight(volume, 70) }).found;
        CHECK_EQ(found.size(), 1u);
        CHECK(!found.empty() && quietReading && same(found.front().reading, quietReading->reading));
        CHECK(!found.empty() && !same(found.front().reading, measured.reading)); // never the take that is gone
    }

    // --- the wall clock set back between a read and a later clear (review S10) ---
    {
        const fs::path src = tmp.path / "s80.wav";
        writeSine(src, 88200, -20.0);
        CHECK_EQ(run(*rec, "op-push-80", "push", volume, [&] {
                     commands::push(volume, src, 80, { .write = options(rec, "op-push-80") });
                 }),
                 std::string());
        const history::SlotSighting seen = sight(volume, 80);
        CHECK(history::readSlotLoudness(volume, 80, *rec).kept); // a read of the loud loop
        CHECK_EQ(history::inferLoudness(store, card, { seen }).found.size(), 1u);
        clockAt -= 3'600'000; // the computer's clock goes back an hour
        CHECK_EQ(run(*rec, "op-clear-80", "clear", volume, [&] {
                     commands::clear(volume, { 80 }, { .write = options(rec, "op-clear-80") });
                 }),
                 std::string());
        // the pedal records a new loop into slot 80 under the old facts
        fs::create_directories(volume::wavDir(volume, 80));
        const fs::path take80 = volume::wavDir(volume, 80) / "s80.wav";
        commands::writeFileBytes(take80, quietOnCard);
        setStamp(take80, seen.take.modifiedMs);
        CHECK(history::inferLoudness(store, card, { sight(volume, 80) }).found.empty());
        CHECK(history::inferLoudness(store, card, { seen }).found.empty());
    }

    return testkit::summary("inferred_loudness_tests");
}
