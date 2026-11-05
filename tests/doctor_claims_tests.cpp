// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat - see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The doctor promises nothing of a reboot that a reboot was not seen to do
// (issue #171).
//
// The theory, from the hardware: the pedal saves into one bank, one write
// generation past the other, and a reboot leaves the pair that way - an RC-5
// powered on seconds before Connect read 0x25a / 0x259 with differing
// documents (2026-11-05). A hand-copied PCM file sat in a slot folder through
// that reboot, still there and still unindexed. The app reads the bank the
// counters name as newest (issue #37), so a healthy pair one step apart asks
// nothing of the player. Hence: no finding for such a pair, either order; the
// warning for a pair two or more steps apart stays, and so does one for two
// documents under one counter, which no writer leaves; the generations and
// the bank read go to operations.log at connect; the non-float finding keeps
// its facts and loses its reboot clause; and every doctor sentence that still
// says what the pedal does at boot is one this suite lists with the dated
// hardware reading behind it - and the source names that reading beside the
// sentence.

#include "support.hpp"

#include "../app/PedalWorker.h"

#include <loopercat/Catalog.hpp>
#include <loopercat/Commands.hpp>
#include <loopercat/DeviceProfile.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/Volume.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path()
        / ("loopercat-doctor-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { fs::create_directories(path); }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec)
            testkit::fail("scratch folder left behind: " + ec.message(), __FILE__, __LINE__);
    }
};

// A card as the pedal leaves it: the memory pair at the given generations,
// each bank its own document, so a test can make the two disagree.
fs::path makeCard(const fs::path& root, std::uint32_t generation1, std::uint32_t generation2,
                  const std::string& document1 = testkit::syntheticMemoryText(),
                  const std::string& document2 = testkit::syntheticMemoryText())
{
    const fs::path volume = root / "PEDAL";
    fs::create_directories(volume / "ROLAND" / "WAVE");
    fs::create_directories(volume::dataDir(volume));
    commands::writeFileBytes(volume::memoryPath(volume, 1),
                             rc0::setTailGeneration(document1, generation1));
    commands::writeFileBytes(volume::memoryPath(volume, 2),
                             rc0::setTailGeneration(document2, generation2));
    return volume;
}

// The synthetic document with one slot renamed: a different document, as a
// save on the pedal leaves the bank it wrote.
std::string renamed(int slot, const std::string& name)
{
    const std::string text = testkit::syntheticMemoryText();
    return rc0::replaceSlotBody(text, slot, rc0::setName(rc0::slotBody(text, slot), name));
}

// ...and with one slot claiming a take of the given length.
std::string withTake(int slot, long long frames)
{
    const std::string text = testkit::syntheticMemoryText();
    std::string body = rc0::slotBody(text, slot);
    body = rc0::setField(body, "WavStat", 1);
    body = rc0::setField(body, "WavLen", frames);
    return rc0::replaceSlotBody(text, slot, body);
}

// The document alone: a bank that reads but carries no write generation.
void dropTrailer(const fs::path& bank)
{
    commands::writeFileBytes(bank, rc0::splitFile(commands::readFileBytes(bank)).document);
}

void putWav(const fs::path& volume, int slot, const std::string& name,
            const testkit::WavSpec& spec)
{
    const auto bytes = testkit::syntheticWav(spec);
    fs::create_directories(volume::wavDir(volume, slot));
    commands::writeFileBytes(volume::wavDir(volume, slot) / name,
                             std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                              bytes.size()));
}

constexpr testkit::WavSpec kFloat32 { .tag = 3, .channels = 2, .bits = 32, .frames = 4410 };
constexpr testkit::WavSpec kPcm16 { .tag = 1, .channels = 2, .bits = 16, .frames = 4410 };

std::string lowercase(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool mentions(const std::string& message, std::string_view needle)
{
    return lowercase(message).find(needle) != std::string::npos;
}

bool says(const std::vector<std::string>& messages, std::string_view sentence)
{
    return std::any_of(messages.begin(), messages.end(), [sentence](const std::string& m) {
        return m.find(sentence) != std::string::npos;
    });
}

std::vector<std::string> messagesOf(const fs::path& volume)
{
    std::vector<std::string> out;
    for (const auto& finding : commands::doctor(volume))
        out.push_back(finding.message);
    return out;
}

std::string banksLine(const fs::path& volume)
{
    return commands::describeBanks(commands::inspectBanks(volume, volume::Bank::memory),
                                   volume::Bank::memory);
}

std::string trimmed(std::string name)
{
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    return name;
}

// The doctor's source, line by line, for the check that a sentence about
// boot has its reading written beside it.
std::vector<std::string> doctorSource()
{
    std::ifstream in(LOOPERCAT_COMMANDS_HPP);
    if (!in)
        testkit::fail("cannot open the doctor's source: " LOOPERCAT_COMMANDS_HPP, __FILE__, __LINE__);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

} // namespace

int main()
{
    // --- a healthy pair one step apart is no finding, either order ---

    {
        // The reading of 2026-11-05: 0x25a / 0x259 right after a reboot, the
        // documents differing. The doctor used to call this "normal right
        // after a save; a reboot reconciles the pair" - the reboot had just
        // happened.
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x25a, 0x259, renamed(5, "AFTER SAVE"));
        CHECK(commands::doctor(volume).empty());
    }
    {
        // The reading of 2026-08-10, the other way round: a save landed in
        // MEMORY2 at 237 while MEMORY1 sat at 236.
        TempDir tmp;
        const fs::path volume =
            makeCard(tmp.path, 236, 237, testkit::syntheticMemoryText(), renamed(5, "AFTER SAVE"));
        CHECK(commands::doctor(volume).empty());
    }
    {
        // Across the counter wrap too: 0xffffffff beside 0x0 is one step, and
        // 0x0 is the newer bank.
        TempDir tmp;
        const fs::path volume =
            makeCard(tmp.path, 0xffffffffu, 0u, renamed(5, "OLDER"), renamed(5, "NEWER"));
        CHECK(commands::doctor(volume).empty());
        CHECK_EQ(banksLine(volume), std::string("memory banks 0xffffffff / 0x0, reading MEMORY2"));
    }

    // --- two or more steps apart is still the warning, and nothing softer ---

    {
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x25b, 0x259);
        const auto findings = commands::doctor(volume);
        CHECK_EQ(findings.size(), 1u);
        if (!findings.empty()) {
            CHECK(findings.front().level == commands::Level::warn);
            CHECK(findings.front().message.find("0x25b / 0x259") != std::string::npos);
            CHECK(findings.front().message.find("more than one step apart") != std::string::npos);
            CHECK(!mentions(findings.front().message, "reboot"));
        }
    }
    {
        // Either order.
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x259, 0x25b);
        const auto findings = commands::doctor(volume);
        CHECK_EQ(findings.size(), 1u);
        if (!findings.empty())
            CHECK(findings.front().message.find("0x259 / 0x25b") != std::string::npos);
    }

    // --- the fact goes to the log line: both generations and the bank read ---

    {
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x25a, 0x259);
        CHECK_EQ(banksLine(volume), std::string("memory banks 0x25a / 0x259, reading MEMORY1"));
    }
    {
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x259, 0x25a);
        CHECK_EQ(banksLine(volume), std::string("memory banks 0x259 / 0x25a, reading MEMORY2"));
    }
    {
        // Equal counters - the app's own writes stamp both banks alike - read
        // the first bank, and the line says so; one document under one
        // counter is nothing to report.
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x25b, 0x25b);
        CHECK_EQ(banksLine(volume), std::string("memory banks 0x25b / 0x25b, reading MEMORY1"));
        CHECK(commands::doctor(volume).empty());
    }
    {
        // Two documents under one counter: no writer leaves this - the app
        // writes the pair from one text, the pedal stamps the bank it writes
        // past the other - and the reader would take MEMORY1 without a word.
        // Still a warning, with the counter, and still no reboot in it.
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x25b, 0x25b, renamed(5, "ONE"), renamed(5, "OTHER"));
        const auto findings = commands::doctor(volume);
        CHECK_EQ(findings.size(), 1u);
        if (!findings.empty()) {
            CHECK(findings.front().level == commands::Level::warn);
            CHECK(findings.front().message.find("differ under the same write generation 0x25b")
                  != std::string::npos);
            CHECK(!mentions(findings.front().message, "reboot"));
        }
    }
    {
        // A bank that cannot vote is named as such, never given a number it
        // does not have.
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x25a, 0x259);
        fs::remove(volume::memoryPath(volume, 2));
        CHECK_EQ(banksLine(volume), std::string("memory banks 0x25a / unreadable, reading MEMORY1"));
    }
    {
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x259, 0x25a);
        dropTrailer(volume::memoryPath(volume, 1));
        CHECK_EQ(banksLine(volume), std::string("memory banks no trailer / 0x25a, reading MEMORY2"));
    }

    // --- the scan reads the bank the line names, and carries the line ---

    {
        // MEMORY1 newer and renamed: the slots come from it, and the snapshot
        // carries the words operations.log gets at connect.
        TempDir tmp;
        const fs::path volume =
            makeCard(tmp.path, 0x25a, 0x259, renamed(5, "NEWER ONE"), renamed(5, "OLDER ONE"));
        PedalWorker scanner(volume.string(), [](const PedalSnapshot&) {});
        const PedalSnapshot seen = scanner.scanOnce();
        CHECK_EQ(seen.error, std::string());
        CHECK_EQ(seen.banks, std::string("memory banks 0x25a / 0x259, reading MEMORY1"));
        CHECK_EQ(seen.slots.size(), 99u);
        if (seen.slots.size() == 99)
            CHECK_EQ(trimmed(seen.slots[4].info.name), std::string("NEWER ONE"));
        CHECK(seen.findings.empty()); // the one-step pair is no banner either
    }
    {
        // MEMORY2 newer: the other bank, the other name, the line agreeing.
        TempDir tmp;
        const fs::path volume =
            makeCard(tmp.path, 0x259, 0x25a, renamed(5, "OLDER ONE"), renamed(5, "NEWER ONE"));
        PedalWorker scanner(volume.string(), [](const PedalSnapshot&) {});
        const PedalSnapshot seen = scanner.scanOnce();
        CHECK_EQ(seen.error, std::string());
        CHECK_EQ(seen.banks, std::string("memory banks 0x259 / 0x25a, reading MEMORY2"));
        CHECK_EQ(seen.slots.size(), 99u);
        if (seen.slots.size() == 99)
            CHECK_EQ(trimmed(seen.slots[4].info.name), std::string("NEWER ONE"));
        CHECK(seen.findings.empty());
    }
    {
        // A card the app cannot read at all still says what it saw of the
        // pair; the error is the reader's, as before.
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x25a, 0x259);
        commands::writeFileBytes(volume::memoryPath(volume, 1), "not a memory file");
        commands::writeFileBytes(volume::memoryPath(volume, 2), "not a memory file");
        PedalWorker scanner(volume.string(), [](const PedalSnapshot&) {});
        const PedalSnapshot seen = scanner.scanOnce();
        CHECK(!seen.error.empty());
        CHECK_EQ(seen.banks,
                 std::string("memory banks unreadable / unreadable, reading MEMORY1"));
        CHECK_THROWS(commands::readMemory(volume), "not an RC0 memory file");
    }

    // --- the gate falls on the bank the vote names, never on a stand-in ---

    {
        // A card with an RC-5 bank beside a newer RC-500 bank: no pedal
        // leaves one, but the rule must hold anyway. The reader takes the
        // newest bank, and a mutation the newest bank's model keeps closed is
        // refused in that model's words - the older bank is not quietly
        // read in its place.
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x38, 0x39, testkit::syntheticMemoryText(),
                                         testkit::syntheticTwoTrackMemoryText());
        CHECK(rc0::profileOf(commands::readMemory(volume)).familyName == "RC-500");
        CHECK_THROWS(commands::readMemoryFor(volume, profile::Operation::setTempo), "RC-500");
        CHECK_EQ(banksLine(volume), std::string("memory banks 0x38 / 0x39, reading MEMORY2"));
        // The doctor names the refused bank and judges nothing by the other.
        const commands::BankPair writing = commands::inspectMemory(volume, profile::Operation::setTempo);
        CHECK_EQ(writing.newest, 2);
        CHECK(writing.errors.contains(2) && !writing.texts.contains(2));
        CHECK(writing.generations.contains(2)); // its counter is real, the vote stands
    }

    // --- the doctor judges slots by the document the player is shown ---

    {
        // A save on the pedal landed in MEMORY2: slot 5 holds a take there,
        // and the take is in its folder. MEMORY1, stale, still says the slot
        // is empty. A doctor reading MEMORY1 would call the fresh take "not
        // indexed yet" - the very symptom issue #37 fixed in the reader.
        TempDir tmp;
        const fs::path volume =
            makeCard(tmp.path, 236, 237, testkit::syntheticMemoryText(), withTake(5, 4410));
        putWav(volume, 5, "005_1.WAV", kFloat32);
        CHECK(commands::doctor(volume).empty());
    }
    {
        // The mirror: the stale bank still claims a take the newer bank has
        // released, and the folder is empty. Nothing is "configured with
        // audio but its folder is empty" in the document the player sees.
        TempDir tmp;
        const fs::path volume =
            makeCard(tmp.path, 237, 236, testkit::syntheticMemoryText(), withTake(5, 4410));
        CHECK(commands::doctor(volume).empty());
    }

    // --- a non-float take: the facts, and no word about a reboot ---

    {
        // Slot 30 of the card of 2026-11-05: a hand-copied PCM file, kept
        // there on purpose. It came through the reboot untouched and
        // unindexed, so the finding says what the pedal plays and what to do,
        // and nothing about what a reboot would do.
        TempDir tmp;
        const fs::path volume = makeCard(tmp.path, 0x25a, 0x259);
        putWav(volume, 30, "LOOPA.wav", kPcm16);
        const auto findings = commands::doctor(volume);
        CHECK_EQ(findings.size(), 1u);
        if (!findings.empty()) {
            const commands::Finding& finding = findings.front();
            CHECK(finding.level == commands::Level::warn);
            CHECK(finding.message.find("slot 30 has LOOPA.wav") != std::string::npos);
            CHECK(finding.message.find("32-bit float only") != std::string::npos);
            CHECK(finding.message.find("Re-push it through LooperCat to convert it")
                  != std::string::npos);
            CHECK(!mentions(finding.message, "reboot"));
            CHECK(!mentions(finding.message, "discard"));
        }
    }

    // --- every doctor sentence about boot is listed here with its reading ---

    {
        // The sentences the doctor may still say about what the pedal does at
        // boot, each with the dated hardware reading behind it. A new boot
        // promise fails this sweep until it is listed here with a reading; a
        // listed sentence the doctor no longer says fails it too, so the list
        // cannot go stale.
        struct Backed {
            std::string_view sentence;
            std::string_view reading;
        };
        const Backed backed[] = {
            { "reboot the pedal to index it",
              "rc5cat 0.1.0, 2026-07-19: a WAV pushed with --no-config was indexed at the next "
              "boot, the pedal filling the slot config itself; golden.json's pedal-authored slot "
              "params are what that indexing wrote. That it takes 32-bit float only is PR #45, "
              "2026-08-19, which is why the sentence is said of a float32 file alone" },
            { "pedal may refuse to boot",
              "rc5cat 0.1.0, 2026-07-19 (README, the macOS landmine): the pedal's parser chokes "
              "at boot on AppleDouble sidecars in its own tree" },
        };

        // Every kind of finding the doctor has, over four cards.
        std::vector<std::string> messages;
        {
            TempDir tmp;
            const fs::path volume =
                makeCard(tmp.path, 0x25b, 0x25b, renamed(5, "ONE"), renamed(5, "OTHER"));
            for (const auto& message : messagesOf(volume))
                messages.push_back(message);
        }
        {
            TempDir tmp;
            // two steps apart; slot 7 configured with audio over an empty folder
            const fs::path volume = makeCard(tmp.path, 0x25b, 0x259, withTake(7, 4410));
            commands::writeFileBytes(volume / "ROLAND" / "WAVE" / "._junk", "sidecar"); // boot hazard
            fs::create_directories(volume / "ROLAND" / "WAVE" / commands::kSwapParkName); // interrupted swap
            putWav(volume, 5, "take.wav", kFloat32);     // float32, not indexed
            putWav(volume, 6, "sixteen.wav", kPcm16);    // non-float, not indexed
            for (const auto& message : messagesOf(volume))
                messages.push_back(message);
        }
        {
            TempDir tmp;
            const fs::path volume = makeCard(tmp.path, 0x38, 0x39);
            dropTrailer(volume::memoryPath(volume, 2)); // malformed trailer
            for (const auto& message : messagesOf(volume))
                messages.push_back(message);
        }
        {
            TempDir tmp;
            const fs::path volume = makeCard(tmp.path, 0x38, 0x39);
            fs::remove(volume::memoryPath(volume, 1)); // unreadable bank
            for (const auto& message : messagesOf(volume))
                messages.push_back(message);
        }

        // The sweep saw every kind, or it proves nothing.
        for (const std::string_view kind :
             { "AppleDouble junk", "interrupted swap", "trailer is malformed", "MEMORY1.RC0:",
               "more than one step apart", "differ under the same write generation",
               "configured with audio but its folder is empty", "not indexed yet",
               "cannot index" })
            if (!says(messages, kind))
                testkit::fail("the sweep never saw the finding: " + std::string(kind), __FILE__,
                              __LINE__);

        for (const auto& message : messages) {
            if (!mentions(message, "boot") && !mentions(message, "power"))
                continue;
            const bool listed = std::any_of(std::begin(backed), std::end(backed),
                                            [&message](const Backed& b) {
                                                return message.find(b.sentence) != std::string::npos;
                                            });
            if (!listed)
                testkit::fail("a boot promise with no reading behind it: " + message, __FILE__,
                              __LINE__);
        }

        // Each listed sentence is still said, its reading here is dated, and
        // the source says the sentence with a dated reading in the comment
        // above it - the player's banner and the engineer's comment cannot
        // drift apart without this failing.
        const std::vector<std::string> source = doctorSource();
        constexpr std::size_t kCommentReach = 60; // lines between a reading and the sentence it backs
        for (const Backed& b : backed) {
            if (!says(messages, b.sentence))
                testkit::fail("listed with a reading but no longer said: " + std::string(b.sentence),
                              __FILE__, __LINE__);
            CHECK(b.reading.find("2026-") != std::string_view::npos); // a reading is dated
            std::size_t at = source.size();
            for (std::size_t i = 0; i < source.size(); ++i)
                if (source[i].find(b.sentence) != std::string::npos) {
                    at = i;
                    break;
                }
            if (at == source.size()) {
                testkit::fail("the doctor's source does not say: " + std::string(b.sentence),
                              __FILE__, __LINE__);
                continue;
            }
            bool dated = false;
            for (std::size_t i = at - std::min(at, kCommentReach); i < at; ++i)
                dated = dated || (source[i].find("//") != std::string::npos
                                  && source[i].find("2026-") != std::string::npos);
            if (!dated)
                testkit::fail("no dated reading in the comment above: " + std::string(b.sentence),
                              __FILE__, __LINE__);
        }
    }

    return testkit::summary("doctor_claims_tests");
}
