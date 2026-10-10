// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The single-slot Normalize around its plan (#142), from the THEORY of the
// two steps, never from their code. What must hold, and what these try to
// break:
//
//   1. the target the player types is a number in the Settings window or
//      nothing: NaN and the infinities JUCE reads out of "nan" and "inf" are
//      nothing, like every typo
//   2. the first step makes the plan of the take's bytes — the history's
//      reading when it holds one for exactly them, a fresh one otherwise — and
//      opens no operation; a target that is not one is refused before a byte
//      is read; a slot with no take is an error
//   3. the write's guard: the very bytes the step measured go on to the
//      history and the command; anything else — another sound under the same
//      name, size and date, a slot emptied since — is refused in one sentence
//      ahead of the operation: no row, no archive copy, the take untouched
//   4. one step per slot: a second request while the first is in flight is
//      dropped, other slots go ahead, and an end without a start is a bug
//   5. a stored target that is not one (a hand-edited "inf", a "nan" from a
//      build whose field let it through) is reported as what the file holds,
//      with no target put in its place; readings then carry no verdict —
//      the number and the peak, nothing coloured, no word about a target

#include "support.hpp"

#include "../app/LoudnessReport.h"
#include "../app/NormalizeStep.h"
#include "../app/TargetLufs.h"
#include "../app/history/WriteOptionsFactory.h"

#include <loopercat/Commands.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

using namespace loopercat;
using history::HistoryRecorder;
using history::HistoryStore;
using normalizeplan::Plan;
namespace fs = std::filesystem;

namespace {

constexpr double kTarget = -18.0;

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-step-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
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

// A float32 stereo take of a 997 Hz sine at `dbfs` peak in both channels —
// the tone BS.1770 calibrates on, so a -28 dBFS take reads -28 LUFS. `spike`
// plants one sample at frame 0 of channel 0: 1e20 is bytes that are not audio.
void putSineWav(const fs::path& volume, int slot, const std::string& name, int frames, double dbfs,
                float spike = 0.0f)
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
        sample(frame == 0 && spike > 0.0f ? spike : v);
        sample(v);
    }
    fs::create_directories(volume::wavDir(volume, slot));
    commands::writeFileBytes(volume::wavDir(volume, slot) / name,
                             std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
}

std::int64_t count(sqlite::Db& db, const std::string& sql)
{
    sqlite::Statement read(db, sql);
    if (!read.step())
        throw Error("count returned no row: " + sql);
    return read.integer(0);
}

std::int64_t clockAt = 1'000'000;
std::int64_t tick() { return ++clockAt; }

std::shared_ptr<HistoryRecorder> recorderAt(const fs::path& dir)
{
    return std::make_shared<HistoryRecorder>(dir, tick);
}

using Before = std::function<void(const fs::path&)>;
using Work = std::function<void(const fs::path&)>;
using After = std::function<void(const std::string&)>;

// What the worker does with a job (PedalWorker::run): `before`, then
// `work`, a throw from either stopping the job; `after` always, with the
// job's error. The worker's lifecycle gate is not under test here.
std::string runJob(const fs::path& volume, const Before& before, const Work& work, const After& after)
{
    std::string error;
    try {
        before(volume);
        work(volume);
    } catch (const std::exception& e) {
        error = e.what();
    }
    after(error);
    return error;
}

// The history's half of a recorded normalize, as MainComponent::recorded
// wires it: the operation opens and names its slot before the card is
// touched, and closes with the job's outcome.
Before opens(const std::shared_ptr<HistoryRecorder>& rec, const std::string& opId, int slot)
{
    return [rec, opId, slot](const fs::path& volume) {
        rec->begin(opId, "normalize", volume);
        rec->subject(opId, slot);
    };
}

After closes(const std::shared_ptr<HistoryRecorder>& rec, const std::string& opId)
{
    return [rec, opId](const std::string& error) { rec->finish(opId, error); };
}

Work normalizes(const std::shared_ptr<HistoryRecorder>& rec, const std::string& opId, int slot)
{
    return [rec, opId, slot](const fs::path& volume) {
        commands::normalize(volume, slot,
                            { .targetLufs = kTarget,
                              .write = history::withHistory(rec, { .opId = opId }) });
    };
}

bool closeTo(double a, double b, double slack) { return std::abs(a - b) <= slack; }

} // namespace

int main()
{
    // --- 1. the target the player types ---
    {
        const auto parsed = [](const char* text) { return targetlufs::parse(juce::String(text)); };
        CHECK(parsed("-18").has_value() && closeTo(*parsed("-18"), -18.0, 1.0e-12));
        CHECK(parsed("  -14.5 ").has_value() && closeTo(*parsed("  -14.5 "), -14.5, 1.0e-12));
        CHECK(parsed("-30").has_value()); // the window's edges are targets
        CHECK(parsed("-8").has_value());
        CHECK(!parsed("-30.1").has_value());
        CHECK(!parsed("-7.9").has_value());
        CHECK(!parsed("").has_value());    // reads as 0.0
        CHECK(!parsed("loud").has_value()); // reads as 0.0
        // JUCE reads these as numbers that are not numbers: none is a target.
        CHECK(std::isnan(juce::String("nan").getDoubleValue())); // the premise
        for (const char* text : { "nan", "NaN", "-nan", "inf", "-inf", "Inf", "1e999", "-1e999" })
            CHECK(!parsed(text).has_value());
        // The whole text is the number, or there is no target: JUCE reads a
        // number off the front of anything, and a hand-edited "-18,5" meant
        // -18.5, not -18 (integration review of 0.9.6).
        for (const char* text : { "-18,5", "-18 LUFS", "-18abc", "--18", "- 18", "-", ".", "-.",
                                  "-18.5.0", "-1e1", "-0x12", "\xe2\x88\x92" "18" /* U+2212 minus */ })
            CHECK(!parsed(text).has_value());
        CHECK(parsed("-18.").has_value() && closeTo(*parsed("-18."), -18.0, 1.0e-12)); // a point with nothing after
        CHECK(parsed("-9.75").has_value() && closeTo(*parsed("-9.75"), -9.75, 1.0e-12));
    }

    // --- 2. the first step: a plan of the take's bytes, and no operation ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putSineWav(volume, 1, "001_1.WAV", 44100, kTarget);
        putSineWav(volume, 2, "002_1.WAV", 44100, -28.0);
        putSineWav(volume, 3, "003_1.WAV", 44100, -23.0, 1.0e20f);
        auto rec = recorderAt(tmp.path / "history");
        const auto first = rec->firstSeen(volume);
        CHECK(first.has_value());
        for (int slot = 1; first.has_value() && slot <= 99; ++slot)
            rec->snapshotStep(*first, slot);
        sqlite::Db& db = rec->store().db();
        const std::int64_t ops = count(db, "SELECT count(*) FROM ops");
        const std::size_t rows = rec->store().cardTimeline().size();

        const normalizestep::Step atTarget = normalizestep::run(volume, 1, *rec, kTarget);
        CHECK(atTarget.plan.outcome == Plan::Outcome::nothingToDo);
        CHECK(atTarget.read.kept);

        const normalizestep::Step quiet = normalizestep::run(volume, 2, *rec, kTarget);
        CHECK(quiet.plan.outcome == Plan::Outcome::apply);
        CHECK(closeTo(quiet.plan.gainDb, 10.0, 0.1));
        CHECK(quiet.plan.words.rfind("Measured -28.0 LUFS, this adds +", 0) == 0);
        CHECK(!quiet.read.recalled);
        CHECK(quiet.read.hash == HistoryStore::contentHash(
                                     commands::readFileBytes(volume::wavDir(volume, 2) / "002_1.WAV")));
        // The same bytes again: the history's reading, the same plan.
        const normalizestep::Step again = normalizestep::run(volume, 2, *rec, kTarget);
        CHECK(again.read.recalled);
        CHECK(again.plan.outcome == Plan::Outcome::apply);
        CHECK(again.plan.words == quiet.plan.words);

        const normalizestep::Step damaged = normalizestep::run(volume, 3, *rec, kTarget);
        CHECK(damaged.plan.outcome == Plan::Outcome::refuse);
        CHECK_EQ(damaged.plan.words, commands::normalizeDamagedRefusal(3, 1));

        // A target that is not one is refused before a byte is read: slot 4
        // has no take, and the refusal is the target's, not the slot's.
        for (const double target : { std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::infinity(),
                                     -std::numeric_limits<double>::infinity(), 0.0 })
            CHECK_THROWS(normalizestep::run(volume, 4, *rec, target), "between -70 and -1");
        CHECK_THROWS(normalizestep::run(volume, 4, *rec, kTarget), "no audio to measure");

        // Nothing the step did opened an operation.
        CHECK_EQ(count(db, "SELECT count(*) FROM ops"), ops);
        CHECK_EQ(rec->store().cardTimeline().size(), rows);
    }

    // --- 3. the write's guard: only the bytes the step measured ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        for (const int slot : { 5, 6, 7 })
            putSineWav(volume, slot, "00" + std::to_string(slot) + "_1.WAV", 44100, -28.0);
        auto rec = recorderAt(tmp.path / "history");
        const auto first = rec->firstSeen(volume);
        CHECK(first.has_value());
        for (int slot = 1; first.has_value() && slot <= 99; ++slot)
            rec->snapshotStep(*first, slot);
        sqlite::Db& db = rec->store().db();
        const auto opsNow = [&db] { return count(db, "SELECT count(*) FROM ops"); };
        const auto blobsNow = [&db] { return count(db, "SELECT count(*) FROM blobs_meta"); };

        // Unchanged: the guard steps aside, the operation opens, the gain lands.
        const fs::path take5 = volume::wavDir(volume, 5) / "005_1.WAV";
        const std::string measured5 = normalizestep::run(volume, 5, *rec, kTarget).read.hash;
        const std::int64_t opsBefore = opsNow();
        CHECK_EQ(runJob(volume, normalizestep::guardedBefore(5, measured5, opens(rec, "op-5", 5)),
                        normalizes(rec, "op-5", 5), closes(rec, "op-5")),
                 std::string());
        CHECK_EQ(opsNow(), opsBefore + 1);
        CHECK(HistoryStore::contentHash(commands::readFileBytes(take5)) != measured5); // it was written

        // Another sound under the same name, size and date: refused in one
        // sentence, before the operation opens — no row, no archive copy,
        // and the take as the replacement left it.
        const fs::path take6 = volume::wavDir(volume, 6) / "006_1.WAV";
        const std::string measured6 = normalizestep::run(volume, 6, *rec, kTarget).read.hash;
        const auto size6 = fs::file_size(take6);
        const auto stamp6 = fs::last_write_time(take6);
        putSineWav(volume, 6, "006_1.WAV", 44100, -31.0);
        fs::last_write_time(take6, stamp6);
        CHECK(fs::file_size(take6) == size6);
        const std::string replaced = commands::readFileBytes(take6);
        const std::int64_t opsMid = opsNow();
        const std::int64_t blobsMid = blobsNow();
        CHECK_EQ(runJob(volume, normalizestep::guardedBefore(6, measured6, opens(rec, "op-6", 6)),
                        normalizes(rec, "op-6", 6), closes(rec, "op-6")),
                 normalizestep::changedSinceMeasured(6));
        CHECK_EQ(normalizestep::changedSinceMeasured(6),
                 std::string("slot 6 changed since it was measured — Normalize again"));
        CHECK_EQ(opsNow(), opsMid);
        CHECK_EQ(blobsNow(), blobsMid);
        CHECK(commands::readFileBytes(take6) == replaced);

        // A slot emptied since the step: the same sentence, the same nothing.
        const std::string measured7 = normalizestep::run(volume, 7, *rec, kTarget).read.hash;
        CHECK(fs::remove(volume::wavDir(volume, 7) / "007_1.WAV"));
        CHECK_EQ(runJob(volume, normalizestep::guardedBefore(7, measured7, opens(rec, "op-7", 7)),
                        normalizes(rec, "op-7", 7), closes(rec, "op-7")),
                 normalizestep::changedSinceMeasured(7));
        CHECK_EQ(opsNow(), opsMid);

        // The guard goes ahead of a history step; a job without one is a wiring bug.
        CHECK_THROWS(normalizestep::guardedBefore(6, measured6, nullptr), "has none");
    }

    // --- 4. one step per slot ---
    {
        normalizestep::StepsInFlight steps;
        CHECK(steps.start(5));
        CHECK(!steps.start(5)); // the second request for slot 5 is dropped
        CHECK(steps.contains(5));
        CHECK(steps.start(6));  // another slot goes ahead
        steps.end(5);
        CHECK(!steps.contains(5));
        CHECK(steps.contains(6));
        CHECK(steps.start(5)); // once ended, the slot takes a request again
        steps.end(5);
        steps.end(6);
        CHECK_THROWS(steps.end(6), "no Normalize step for slot 6");
        CHECK_THROWS(steps.end(9), "no Normalize step for slot 9");
    }

    // --- 5. a stored target that is not one, and readings without a verdict ---
    {
        // What a settings file holds is text; read back through the field's
        // rule, the text that is not a target is no target — none in its place.
        const auto stored = [](const char* text) { return targetlufs::parse(juce::String(text)); };
        CHECK(stored("-18").has_value() && closeTo(*stored("-18"), -18.0, 1.0e-12));
        CHECK(stored("-18.0").has_value() && closeTo(*stored("-18.0"), -18.0, 1.0e-12));
        for (const char* text : { "nan", "inf", "-inf", "-40", "0", "" })
            CHECK(!stored(text).has_value());
        CHECK_EQ(targetlufs::unusableStored("inf"),
                 juce::String::fromUTF8("The normalize target in Settings is not a number LooperCat "
                                        "can use (inf): set it again in Settings \xe2\x86\x92 Import"));
        // an empty stored value is said as empty, not as "()"
        CHECK_EQ(targetlufs::unusableStored(""),
                 juce::String::fromUTF8("The normalize target in Settings is empty: "
                                        "set it again in Settings \xe2\x86\x92 Import"));
        CHECK_EQ(targetlufs::unusableStored("  "), targetlufs::unusableStored(""));

        const wav::LoudnessReading quiet { -22.8, 0.1f, -20.0, 0 };
        const wav::LoudnessReading peaky { -25.0, 0.89f, -1.0, 0 }; // nothing to gain against -18
        const wav::LoudnessReading atTarget { -18.1, 0.3f, -9.0, 0 };

        // The control: with a target, the verdict and its colour.
        const loudnessreport::Report judged = loudnessreport::describe(quiet, -18.0);
        CHECK(judged.attention);
        CHECK(judged.rowText.contains("below target -18"));

        // Without one: the number and the peak, no verdict, nothing coloured.
        for (const auto& reading : { quiet, peaky, atTarget }) {
            const loudnessreport::Report bare = loudnessreport::describe(reading, std::nullopt);
            CHECK_EQ(bare.cellText, juce::String(*reading.integratedLufs, 1));
            CHECK_EQ(bare.rowText, juce::String(*reading.integratedLufs, 1) + " LUFS");
            CHECK(bare.noteText.contains(juce::String(reading.truePeakDb, 1) + " dBTP"));
            CHECK(!bare.attention);
            CHECK(!bare.damaged);
            CHECK(!bare.rowText.contains("target"));
            CHECK(!bare.noteText.contains("target"));
            CHECK(!bare.rowText.contains("peak-limited"));
            CHECK(bare.tooltipText.contains("Settings"));
        }
        // What is not a number stays what it was: a fact about the bytes.
        const loudnessreport::Report silent =
            loudnessreport::describe({ std::nullopt, 0.0f, -std::numeric_limits<double>::infinity(), 0 },
                                     std::nullopt);
        CHECK_EQ(silent.cellText, juce::String("n/a"));
        CHECK(!silent.attention);
        const loudnessreport::Report broken =
            loudnessreport::describe({ 767.0, 2.4e38f, 400.0, 1234 }, std::nullopt);
        CHECK_EQ(broken.cellText, juce::String("damaged"));
        CHECK(broken.damaged);
    }

    return testkit::summary("normalize_step_tests");
}
