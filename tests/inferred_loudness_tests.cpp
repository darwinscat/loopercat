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
//     decides by them against a target the guess says is already met

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
        const auto found = history::inferLoudness(store, card, { sight(volume, 3), sight(volume, 4) });
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
        CHECK(history::inferLoudness(store, other, { sight(volume, 3) }).empty());
        store.selectCard(card);
    }

    // --- a rename leaves the take what it was ---
    {
        CHECK_EQ(run(*rec, "op-rename", "rename", volume, [&] {
                     commands::rename(volume, 3, "Intro", options(rec, "op-rename"));
                 }),
                 std::string());
        const auto found = history::inferLoudness(store, card, { sight(volume, 3) });
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
        const auto found = history::inferLoudness(store, card, { moved });
        CHECK_EQ(found.size(), 1u);
        CHECK(!found.empty() && same(found.front().reading, measured.reading));
        // the slot it left: the swap is that slot's last word, whatever the facts
        history::SlotSighting left = moved;
        left.slot = 3;
        CHECK(history::inferLoudness(store, card, { left }).empty());
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
        CHECK_EQ(history::inferLoudness(store, card, { sight(volume, 5) }).size(), 1u);
        CHECK_EQ(run(*rec, "op-swap-5-8", "swap", volume, [&] {
                     commands::swap(volume, 5, 8, options(rec, "op-swap-5-8"));
                 }),
                 std::string());
        const auto renamed = sight(volume, 8);
        CHECK_EQ(renamed.take.name, std::string("008_1.WAV")); // retitled to its new address
        CHECK(history::inferLoudness(store, card, { renamed }).empty());
        // the same bytes under the old name, asked of either slot: nothing
        history::SlotSighting asBefore = renamed;
        asBefore.take.name = "005_1.WAV";
        CHECK(history::inferLoudness(store, card, { asBefore }).empty());
        asBefore.slot = 5;
        CHECK(history::inferLoudness(store, card, { asBefore }).empty());
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
        CHECK(history::inferLoudness(store, card, { trimmed }).empty());
        CHECK(history::inferLoudness(store, card, { whole }).empty()); // the old facts: the newest row is the last word
        // a read measures the new take and files it: recognised from then on
        const history::SlotLoudness again = history::readSlotLoudness(volume, 7, *rec);
        CHECK(again.kept);
        const auto found = history::inferLoudness(store, card, { trimmed });
        CHECK_EQ(found.size(), 1u);
        CHECK(!found.empty() && same(found.front().reading, again.reading));
        // and a stamp one step off is another file: nothing
        history::SlotSighting later = trimmed;
        later.take.modifiedMs += 2000; // FAT's step
        CHECK(history::inferLoudness(store, card, { later }).empty());
    }

    // --- the same bytes in another slot: their reading travels with them ---
    // --- then a clear, and the old facts met again: nothing ---
    {
        CHECK_EQ(run(*rec, "op-push-10", "push", volume, [&] {
                     commands::push(volume, loud, 10, { .write = options(rec, "op-push-10") });
                 }),
                 std::string());
        const history::SlotSighting pushed = sight(volume, 10);
        const auto found = history::inferLoudness(store, card, { pushed });
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
        CHECK(history::inferLoudness(store, card, { pushed }).empty());
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
        const auto fooled = history::inferLoudness(store, card, { after });
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
        commands::NormalizeResult result;
        CHECK_EQ(run(*rec, "op-normalize-11", "normalize", volume, [&] {
                     result = commands::normalize(volume, 11,
                                                  { .targetLufs = guessed,
                                                    .write = options(rec, "op-normalize-11") });
                 }),
                 std::string());
        CHECK(result.applied);
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

    return testkit::summary("inferred_loudness_tests");
}
