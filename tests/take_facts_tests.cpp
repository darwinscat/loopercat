// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A take's directory entry as the connect scan reads it (#141): TakeFacts.h,
// and PedalWorker::scanOnce carrying the facts slot by slot, against a
// scratch volume. What must hold, and what these try to break:
//
//   - a regular file: its name, its size, and the platform's own stamp,
//     exactly — the number the history stores for the same file
//   - one stat gives the size and the stamp together (history::statFile),
//     the stamp JUCE gives for the same file, to the millisecond; a stamp of
//     zero is a stamp, told apart from no answer at all — JUCE's getter
//     cannot tell the two apart, which is why the facts no longer ask it
//   - an entry the file system will not describe as a take — a folder under
//     the take's name, a name with no file behind it, a link to nothing —
//     has no facts, and no throw
//   - the scan reads no audio for any of this: a take that is not a WAV at
//     all is sighted like any other, and a slot whose entry gives no facts
//     keeps its name on screen while the scan carries on, no error
//   - an empty slot carries no facts

#include "support.hpp"

#include "../app/PedalWorker.h"
#include "../app/TakeFacts.h"

#include <loopercat/Commands.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/Volume.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#if !defined(_WIN32)
#include <utime.h>
#endif

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-take-facts-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        CHECK(!ec); // a directory that will not go is a failure, not a leak
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

juce::File juceFile(const fs::path& file)
{
    const std::u8string utf8 = file.u8string();
    return juce::File(juce::String::fromUTF8(reinterpret_cast<const char*>(utf8.data()),
                                             static_cast<int>(utf8.size())));
}

// The platform's own stamp for a file, as the history stores it.
std::int64_t stampOf(const fs::path& file)
{
    return juceFile(file).getLastModificationTime().toMilliseconds();
}

#if !defined(_WIN32)
// A file stamped at the epoch, set the POSIX way: JUCE's own setter reads a
// zero time as "leave it as it is", and its getter answers zero for a file
// it cannot stat.
bool stampAtEpoch(const fs::path& file)
{
    const struct utimbuf epoch { 0, 0 };
    return ::utime(file.c_str(), &epoch) == 0;
}
#endif

} // namespace

int main()
{
    TempDir tmp;

    // --- a regular file: name, size and the exact stamp ---
    {
        const fs::path file = tmp.path / "001_1.WAV";
        commands::writeFileBytes(file, "not audio at all"); // 16 bytes: the facts do not care
        const auto facts = takeFacts(file);
        CHECK(facts.has_value());
        if (facts) {
            CHECK_EQ(facts->name, std::string("001_1.WAV"));
            CHECK_EQ(facts->size, 16);
            CHECK_EQ(facts->modifiedMs, stampOf(file)); // equal, not close
            CHECK(facts->modifiedMs > 0);
            CHECK(*facts == (TakeFacts { "001_1.WAV", 16, stampOf(file) }));
        }
        // the one stat behind them says the same, size and stamp together
        CHECK(history::statFile(file) == (history::FileStat { 16, stampOf(file) }));
        CHECK_EQ(history::modifiedMs(file), stampOf(file));
        // asked again of the unchanged file: the very same facts, to the
        // millisecond — the history compares them for equality
        CHECK(takeFacts(file) == facts);
    }

    // --- an entry that is not a take: no facts, no throw ---
    {
        const fs::path folder = tmp.path / "002_1.WAV";
        fs::create_directories(folder);
        CHECK(!takeFacts(folder).has_value());
        CHECK(!takeFacts(tmp.path / "003_1.WAV").has_value()); // nothing behind the name
        CHECK(!takeFacts(tmp.path / "nowhere" / "004_1.WAV").has_value()); // nor a folder for it
        CHECK_THROWS(history::modifiedMs(tmp.path / "003_1.WAV"), "cannot read the modification time");
#if !defined(_WIN32) // the epoch stamp is set through POSIX, and a symbolic
                      // link needs a privilege a Windows test run lacks
        // a file stamped at the epoch is described as the stat has it: zero
        // is the file's stamp, not "could not tell"
        const fs::path epochal = tmp.path / "005_1.WAV";
        commands::writeFileBytes(epochal, "sixteen bytes!!!");
        CHECK(stampAtEpoch(epochal));
        CHECK(takeFacts(epochal) == (std::optional<TakeFacts>(TakeFacts { "005_1.WAV", 16, 0 })));
        // a link whose target is gone: no regular file behind the name
        const fs::path dangling = tmp.path / "006_1.WAV";
        fs::create_symlink(tmp.path / "gone.wav", dangling);
        CHECK(!takeFacts(dangling).has_value());
#endif
    }

    // --- the scan carries the facts, and carries on where there are none ---
    {
        const fs::path volume = makePedal(tmp.path);
        fs::create_directories(volume::wavDir(volume, 1));
        commands::writeFileBytes(volume::wavDir(volume, 1) / "001_1.WAV", "RIFF but not a WAV");
        fs::create_directories(volume::wavDir(volume, 2) / "002_1.WAV"); // a folder under the take's name
#if !defined(_WIN32)
        fs::create_directories(volume::wavDir(volume, 4));
        fs::create_symlink(tmp.path / "gone.wav", volume::wavDir(volume, 4) / "004_1.WAV"); // a link to nothing
#endif
        PedalWorker scanner(volume.string(), [](const PedalSnapshot&) {});
        const PedalSnapshot seen = scanner.scanOnce();
        CHECK_EQ(seen.error, std::string());
        CHECK_EQ(seen.slots.size(), 99u);
        if (seen.slots.size() == 99) {
            const SlotRow& first = seen.slots[0];
            CHECK_EQ(first.wavFile, std::string("001_1.WAV"));
            CHECK(first.take.has_value());
            CHECK(first.take
                  == std::optional<TakeFacts>(TakeFacts {
                      "001_1.WAV", 18, stampOf(volume::wavDir(volume, 1) / "001_1.WAV") }));
            const SlotRow& second = seen.slots[1];
            CHECK_EQ(second.wavFile, std::string("002_1.WAV")); // still listed by name
            CHECK(!second.take.has_value());                    // but vouched for by nothing
            CHECK(!second.wavPath.empty());
            const SlotRow& third = seen.slots[2];
            CHECK(third.wavFile.empty());
            CHECK(!third.take.has_value());
#if !defined(_WIN32)
            const SlotRow& fourth = seen.slots[3];
            CHECK_EQ(fourth.wavFile, std::string("004_1.WAV")); // listed, playable by path
            CHECK(!fourth.wavPath.empty());
            CHECK(!fourth.take.has_value());                     // and vouched for by nothing
#endif
        }
        // the facts follow the file: a rewrite is another sighting
        commands::writeFileBytes(volume::wavDir(volume, 1) / "001_1.WAV", "RIFF but not a WAV, longer");
        const PedalSnapshot after = scanner.scanOnce();
        CHECK(after.slots.size() == 99 && after.slots[0].take.has_value()
              && after.slots[0].take->size == 26);
        CHECK(!(after == seen)); // a changed take is a changed snapshot
    }

    return testkit::summary("take_facts_tests");
}
