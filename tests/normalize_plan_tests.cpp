// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The decision the single-slot Normalize makes before it asks (#142), from
// the THEORY of what commands::normalize does with a take — never from the
// plan's code. Each case is one the window must stay shut for where the
// command would write nothing or refuse, or open for where it would write:
//
//   1. a take within loudness::kAlreadyAtTargetLu of the target is nothing
//      to do, and a fraction of an LU outside it is something — the boundary
//      is the command's constant, not a round number (0.9 LU off is a plan)
//   2. a boost with headroom is the whole wanted gain, worded as the window
//      says it; a cut is the whole wanted gain and is never capped, even on a
//      take already over the ceiling
//   3. a quiet take already peaking at the ceiling is nothing to do, with the
//      peak as the reason; so is one whose headroom is under the smallest gain
//      worth writing (loudness::kSmallestGainDb); one with more headroom gets
//      exactly that much, and the words say so
//   4. bytes that are not audio are refused with their count, before anything
//      is computed from them; silence and a take under one gating block are
//      refused as unmeasurable; a target outside the command's window is a
//      bug and throws, as the command does; a measurable take with no peak
//      is the impossible reading the gain rule throws on
//   5. one to one with the command itself, on a synthetic card: wherever the
//      command writes, the plan said apply with the same gain and the same
//      cap; wherever it answers "nothing", the plan said so; wherever it
//      throws, the plan refused in the very same sentence. And a take the
//      command has just capped is nothing to do from then on, for both.

#include "support.hpp"
#include "archive_support.hpp"

#include "../app/NormalizePlan.h"

#include <loopercat/Commands.hpp>
#include <loopercat/Normalize.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace loopercat;
using normalizeplan::Plan;
namespace fs = std::filesystem;

namespace {

constexpr double kTarget = -18.0;
constexpr double kTiny = 1.0e-9; // the slack a double needs, far under anything audible

// A reading of audio: integrated loudness and a true peak, the sample peak
// following the latter — the shape the meter hands over for any take that is
// sound (tests/loudness_tests.cpp).
wav::LoudnessReading audio(double lufs, double truePeakDb)
{
    return { lufs, static_cast<float>(std::pow(10.0, truePeakDb / 20.0)), truePeakDb, 0 };
}

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-plan-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

fs::path makePedal(const fs::path& root)
{
    const fs::path volume = root / "PEDAL";
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
// plants one sample at frame 0 of channel 0: the quiet-but-peaky shape at
// 0.5..0.95, and bytes that are not audio at 1e20.
void putSineFloatWav(const fs::path& volume, int slot, const std::string& name, int frames,
                     double dbfs, float spike = 0.0f)
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

// The take's bytes as the worker reads them, measured as the worker measures them.
wav::LoudnessReading measureSlot(const fs::path& volume, int slot)
{
    const auto files = volume::listSlotWavs(volume, slot);
    const std::string raw = commands::readFileBytes(volume::wavDir(volume, slot) / files.front());
    return wav::measureLoudness(
        wav::BytesView(reinterpret_cast<const unsigned char*>(raw.data()), raw.size()));
}

commands::WriteOptions writeOpts(const fs::path& root, const std::string& opId)
{
    return { .opId = opId, .archive = testkit::fileArchive(root / "archive", opId),
             .journal = testkit::noOpJournal() };
}

// What the command answered, or the sentence it refused with.
struct CommandOutcome {
    std::optional<commands::NormalizeResult> result;
    std::string refusal;
};

CommandOutcome runCommand(const fs::path& volume, int slot, const commands::WriteOptions& write,
                          double target = kTarget)
{
    try {
        return { commands::normalize(volume, slot, { .targetLufs = target, .write = write }), {} };
    } catch (const Error& e) {
        return { std::nullopt, e.what() };
    }
}

bool near(double a, double b, double slack = kTiny) { return std::abs(a - b) <= slack; }

} // namespace

int main()
{
    // --- 1. the tolerance is the command's constant ---
    {
        const Plan at = normalizeplan::decide(7, audio(kTarget, -6.0), kTarget);
        CHECK(at.outcome == Plan::Outcome::nothingToDo);
        CHECK(near(at.measuredLufs, kTarget));
        CHECK(near(at.gainDb, 0.0));
        CHECK(!at.cappedByPeak);
        CHECK(at.words.empty());

        // A hair inside the tolerance either way: nothing. A hair outside: a
        // plan — the command's rule is `< kAlreadyAtTargetLu`, so the margin
        // here is a thousandth of an LU, not a float's rounding.
        const double inside = loudness::kAlreadyAtTargetLu - 1.0e-3;
        const double outside = loudness::kAlreadyAtTargetLu + 1.0e-3;
        CHECK(normalizeplan::decide(7, audio(kTarget - inside, -6.0), kTarget).outcome
              == Plan::Outcome::nothingToDo);
        CHECK(normalizeplan::decide(7, audio(kTarget + inside, -6.0), kTarget).outcome
              == Plan::Outcome::nothingToDo);
        const Plan below = normalizeplan::decide(7, audio(kTarget - outside, -6.0), kTarget);
        CHECK(below.outcome == Plan::Outcome::apply);
        CHECK(near(below.gainDb, outside));
        const Plan above = normalizeplan::decide(7, audio(kTarget + outside, -6.0), kTarget);
        CHECK(above.outcome == Plan::Outcome::apply);
        CHECK(near(above.gainDb, -outside));

        // The issue's own case: measured -14.1 against a -14.0 target is the
        // take the old flow asked about and then left alone. No window now.
        const Plan issue = normalizeplan::decide(7, audio(-14.1, -6.0), -14.0);
        CHECK(issue.outcome == Plan::Outcome::nothingToDo);
        CHECK(near(issue.measuredLufs, -14.1));
        CHECK(!issue.cappedByPeak);

        // 0.9 LU off is outside a 0.2 LU tolerance: the command would write
        // +0.9 dB, so the window must open for it.
        const Plan nearly = normalizeplan::decide(7, audio(-18.9, -6.0), kTarget);
        CHECK(nearly.outcome == Plan::Outcome::apply);
        CHECK(near(nearly.gainDb, 0.9));
        CHECK(!nearly.cappedByPeak);
        CHECK_EQ(nearly.words, std::string("Measured -18.9 LUFS, this adds +0.9 dB"));
    }

    // --- 2. a boost with headroom, a cut ---
    {
        const Plan boost = normalizeplan::decide(7, audio(-22.7, -8.0), kTarget);
        CHECK(boost.outcome == Plan::Outcome::apply);
        CHECK(near(boost.measuredLufs, -22.7));
        CHECK(near(boost.gainDb, 4.7));
        CHECK(!boost.cappedByPeak);
        CHECK_EQ(boost.words, std::string("Measured -22.7 LUFS, this adds +4.7 dB"));

        const Plan cut = normalizeplan::decide(7, audio(-12.3, -1.5), kTarget);
        CHECK(cut.outcome == Plan::Outcome::apply);
        CHECK(near(cut.gainDb, -5.7));
        CHECK(!cut.cappedByPeak);
        CHECK_EQ(cut.words, std::string("Measured -12.3 LUFS, this cuts 5.7 dB"));

        // The ceiling is for boosts: a take already over it is cut in full.
        const Plan over = normalizeplan::decide(7, audio(-10.0, 0.3), kTarget);
        CHECK(over.outcome == Plan::Outcome::apply);
        CHECK(near(over.gainDb, -8.0));
        CHECK(!over.cappedByPeak);
    }

    // --- 3. the peak ceiling: nothing to give, or only so much ---
    {
        const Plan limited = normalizeplan::decide(7, audio(-25.0, loudness::kPeakCeilingDb), kTarget);
        CHECK(limited.outcome == Plan::Outcome::nothingToDo);
        CHECK(near(limited.measuredLufs, -25.0));
        CHECK(near(limited.gainDb, 0.0));
        CHECK(limited.cappedByPeak); // the reason: peak-limited, not at target
        CHECK(limited.words.empty());

        const Plan hot = normalizeplan::decide(7, audio(-25.0, -0.2), kTarget); // over the ceiling
        CHECK(hot.outcome == Plan::Outcome::nothingToDo);
        CHECK(hot.cappedByPeak);

        const Plan partial = normalizeplan::decide(7, audio(-25.0, -3.1), kTarget);
        CHECK(partial.outcome == Plan::Outcome::apply);
        CHECK(near(partial.gainDb, 2.1));
        CHECK(partial.cappedByPeak);
        CHECK_EQ(partial.words,
                 std::string("Measured -25.0 LUFS, +2.1 dB possible: the -1 dBTP ceiling stops the rest"));

        // Headroom under an audible step is no headroom: the smallest gain
        // worth writing is the step "already at target" stands for. The
        // review's case — 0.049 dB under the ceiling — is nothing to do, with
        // the ceiling as the reason; a hair over the step is a boost.
        const Plan sliver = normalizeplan::decide(7, audio(-25.0, loudness::kPeakCeilingDb - 0.049),
                                                  kTarget);
        CHECK(sliver.outcome == Plan::Outcome::nothingToDo);
        CHECK(sliver.cappedByPeak);
        CHECK(near(sliver.gainDb, 0.0));
        const double step = loudness::kSmallestGainDb;
        CHECK(normalizeplan::decide(7, audio(-25.0, loudness::kPeakCeilingDb - (step - 1.0e-3)),
                                    kTarget).outcome
              == Plan::Outcome::nothingToDo);
        const Plan justEnough = normalizeplan::decide(
            7, audio(-25.0, loudness::kPeakCeilingDb - (step + 1.0e-3)), kTarget);
        CHECK(justEnough.outcome == Plan::Outcome::apply);
        CHECK(justEnough.cappedByPeak);
        CHECK(near(justEnough.gainDb, step + 1.0e-3, 1.0e-9));
    }

    // --- 4. refusals, in the command's sentences; bugs throw ---
    {
        const double inf = std::numeric_limits<double>::infinity();
        const Plan damaged = normalizeplan::decide(42, { -23.0, 1.0e20f, inf, 3 }, kTarget);
        CHECK(damaged.outcome == Plan::Outcome::refuse);
        CHECK_EQ(damaged.words, commands::normalizeDamagedRefusal(42, 3));
        CHECK(damaged.words.find("slot 42 contains 3 impossible sample value(s)") != std::string::npos);
        CHECK(near(damaged.gainDb, 0.0));
        // Garbage outranks silence: a reading with wild samples is refused
        // for them whatever else it says.
        const Plan garbage = normalizeplan::decide(42, { std::nullopt, 0.0f, -inf, 2 }, kTarget);
        CHECK(garbage.outcome == Plan::Outcome::refuse);
        CHECK(garbage.words.find("impossible sample") != std::string::npos);

        const Plan silent = normalizeplan::decide(42, { std::nullopt, 0.0f, -inf, 0 }, kTarget);
        CHECK(silent.outcome == Plan::Outcome::refuse);
        CHECK_EQ(silent.words, commands::normalizeUnmeasurableRefusal(42));
        CHECK(silent.words.find("slot 42 is silent or shorter") != std::string::npos);

        // The command's target window, edges excluded: 0.0 is an unset field.
        for (const double target : { 0.0, loudness::kPeakCeilingDb, loudness::kAbsoluteGateLufs, 5.0 })
            CHECK_THROWS(normalizeplan::decide(7, audio(-23.0, -6.0), target), "between -70 and -1");
        CHECK(normalizeplan::decide(7, audio(-23.0, -6.0), loudness::kPeakCeilingDb - 0.1).outcome
              == Plan::Outcome::apply);
        CHECK(normalizeplan::decide(7, audio(-23.0, -6.0), loudness::kAbsoluteGateLufs + 0.1).outcome
              == Plan::Outcome::apply);
        // A measurable take with no peak cannot come off one meter pass; the
        // gain rule says so, and the plan lets it — the command would too.
        CHECK_THROWS(normalizeplan::decide(7, { -30.0, 0.0f, -inf, 0 }, kTarget), "finite peak");
    }

    // --- 5. one to one with the command, on a card ---
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        putSineFloatWav(volume, 1, "attarget.wav", 44100, kTarget);
        putSineFloatWav(volume, 2, "peaky.wav", 44100, -25.0, 0.95f);   // over the ceiling already
        putSineFloatWav(volume, 3, "faint.wav", 44100, -100.0);        // under the -70 gate
        putSineFloatWav(volume, 4, "short.wav", 4410, -20.0);          // 100 ms < one block
        putSineFloatWav(volume, 5, "damaged.wav", 44100, -23.0, 1.0e20f);
        putSineFloatWav(volume, 6, "quiet.wav", 44100, -28.0);         // +10 dB, headroom to spare
        putSineFloatWav(volume, 7, "capped.wav", 44100, -25.0, 0.5f);  // wants +7, has about +5
        putSineFloatWav(volume, 8, "loud.wav", 44100, -10.0);          // -8 dB cut

        int applied = 0, untouched = 0, refused = 0;
        for (int slot = 1; slot <= 8; ++slot) {
            // The plan from the take as it is, then the command on the same
            // take — in that order, since an applied command rewrites it.
            const wav::LoudnessReading reading = measureSlot(volume, slot);
            const Plan plan = normalizeplan::decide(slot, reading, kTarget);
            const CommandOutcome command =
                runCommand(volume, slot, writeOpts(tmp.path, "op-" + std::to_string(slot)));

            switch (plan.outcome) {
            case Plan::Outcome::apply:
                ++applied;
                CHECK(command.result.has_value());
                CHECK(command.result.has_value() && command.result->applied);
                CHECK(command.result.has_value() && near(command.result->gainDb, plan.gainDb));
                CHECK(command.result.has_value()
                      && near(command.result->measuredLufs, plan.measuredLufs));
                CHECK(command.result.has_value() && command.result->cappedByPeak == plan.cappedByPeak);
                CHECK(plan.words.rfind("Measured ", 0) == 0u);
                break;
            case Plan::Outcome::nothingToDo:
                ++untouched;
                CHECK(command.result.has_value());
                CHECK(command.result.has_value() && !command.result->applied);
                CHECK(command.result.has_value() && command.result->cappedByPeak == plan.cappedByPeak);
                CHECK(command.result.has_value()
                      && near(command.result->measuredLufs, plan.measuredLufs));
                break;
            case Plan::Outcome::refuse:
                ++refused;
                CHECK(!command.result.has_value());
                CHECK_EQ(command.refusal, plan.words);
                break;
            }
        }
        // Every shape above went where the card says it should.
        CHECK_EQ(applied, 3);   // quiet, capped, loud
        CHECK_EQ(untouched, 2); // at target, peaky
        CHECK_EQ(refused, 3);   // faint, short, damaged
        // The capped take got its gain and now sits at the ceiling, give or
        // take what float32 rounding leaves: the next Normalize has nothing to
        // offer and the command nothing to write — not once, not on the third
        // try (review of #142: a "+0.0 dB" window, the same bytes rewritten
        // and a history row added each time).
        const std::string cappedOnce = commands::readFileBytes(volume::wavDir(volume, 7) / "capped.wav");
        for (int again = 1; again <= 3; ++again) {
            const Plan next = normalizeplan::decide(7, measureSlot(volume, 7), kTarget);
            CHECK(next.outcome == Plan::Outcome::nothingToDo);
            CHECK(next.cappedByPeak);
            const CommandOutcome rerun =
                runCommand(volume, 7, writeOpts(tmp.path, "op-again-" + std::to_string(again)));
            CHECK(rerun.result.has_value() && !rerun.result->applied);
            CHECK(rerun.result.has_value() && rerun.result->cappedByPeak);
            CHECK(rerun.result.has_value() && rerun.result->archivedOriginal.empty());
        }
        CHECK(commands::readFileBytes(volume::wavDir(volume, 7) / "capped.wav") == cappedOnce);
        const Plan peakyNext = normalizeplan::decide(2, measureSlot(volume, 2), kTarget);
        CHECK(peakyNext.outcome == Plan::Outcome::nothingToDo);
        CHECK(peakyNext.cappedByPeak);

        // A target the command refuses, the plan refuses in the same words —
        // and before any reading enters into it.
        const CommandOutcome badTarget = runCommand(volume, 1, writeOpts(tmp.path, "op-bad"), 0.0);
        CHECK(!badTarget.result.has_value());
        std::string planRefusal;
        try {
            (void) normalizeplan::decide(1, measureSlot(volume, 1), 0.0);
        } catch (const Error& e) {
            planRefusal = e.what();
        }
        CHECK_EQ(planRefusal, badTarget.refusal);
    }

    return testkit::summary("normalize_plan_tests");
}
