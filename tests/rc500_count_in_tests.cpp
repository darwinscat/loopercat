// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Play Count-In on the two-track model (opened 2026-11-07).
//
// The theory: the feature owns three fields of <RHYTHM> - PlayCount, State,
// Pattern - and the two-track card lays that section out as the RC-5 does,
// with the RC-5's numbers: a memory saved on the pedal with the rhythm on,
// pattern Blank, beat 4/4 read State 1, Pattern 57, Beat 2, and against the
// memory's earlier reading exactly State and Pattern had moved (hardware,
// 2026-11-07). So the switch does on this card what it does on the RC-5 and
// not a byte more: on over a silent section borrows it (the triple), on over
// a playing groove moves the count alone, off hands a borrowed section back
// and leaves a groove where it was; neither track section is its business;
// the #149 rule stands at any beat but 4/4; and the pair is written as every
// write writes it - both banks, one document, stamped past the highest
// generation on the card. The pattern and kit lists stay uncharted for this
// model, so the rhythm card stays closed, and the sentence a closed command
// says now lists the count-in among what is open.

#include "support.hpp"
#include "archive_support.hpp"
#include "field_changes.hpp"
#include "two_track_card.hpp"

#include <loopercat/Catalog.hpp>
#include <loopercat/Commands.hpp>
#include <loopercat/DeviceProfile.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/Volume.hpp>
#include <loopercat/usecases/Beat.hpp>
#include <loopercat/usecases/CountIn.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path()
        / ("loopercat-rc500-count-in-"
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

commands::WriteOptions writeOpts(const fs::path& root, std::string opId)
{
    commands::Archive archive = testkit::fileArchive(root / "archive", opId);
    return { .opId = std::move(opId), .archive = std::move(archive), .journal = testkit::noOpJournal() };
}

// The 2026-11-07 reading of memory 5 on the two-track pedal, saved there with
// the rhythm on, pattern Blank, beat 4/4: what the card stored. Stated here
// as read, not as the code names it - the test's premise is that the two
// coincide.
constexpr long long kReadRhythmOn = 1;
constexpr long long kReadBlank = 57;
constexpr long long kReadFourFour = 2;

// The hardware pairing of #147 for the uncharted beat: Beat 4 reads 6/4, and
// Rock2 chosen on the pedal is stored as 3 there, as 12 in the 4/4 list.
constexpr long long kSixFour = 4;
constexpr long long kRock2At64 = 3;
constexpr long long kRock2At44 = 12;

// Every file of the volume with its bytes: the card as a whole, to say
// "nothing moved" after a refusal.
std::map<std::string, std::string> volumeBytes(const fs::path& volume)
{
    std::map<std::string, std::string> out;
    for (const auto& entry : fs::recursive_directory_iterator(volume))
        if (entry.is_regular_file())
            out[fs::relative(entry.path(), volume).generic_string()] =
                commands::readFileBytes(entry.path());
    return out;
}

// The takes alone: every file of the volume but the memory pair, which a
// write stamps by design.
std::map<std::string, std::string> takeBytes(const fs::path& volume)
{
    std::map<std::string, std::string> out = volumeBytes(volume);
    for (const int fileNo : { 1, 2 })
        out.erase(fs::relative(volume::memoryPath(volume, fileNo), volume).generic_string());
    return out;
}

// One section's bytes, opener to closer - the unit of "untouched".
std::string section(const std::string& body, const std::string& tag)
{
    const auto open = body.find("<" + tag + ">");
    const auto close = body.find("</" + tag + ">", open);
    if (open == std::string::npos || close == std::string::npos)
        throw Error("test fixture: missing section <" + tag + ">");
    return body.substr(open, close - open + tag.size() + 3);
}

// RHYTHM fields as the pedal's own screens would have left them in one
// memory, in the document the two banks are about to carry.
std::string withRhythm(std::string text, int slot,
                       std::initializer_list<std::pair<const char*, long long>> fields)
{
    std::string body = rc0::slotBody(text, slot);
    for (const auto& [tag, value] : fields)
        body = rc0::setSectionField(body, rc0::kSectionRhythm, tag, value);
    return rc0::replaceSlotBody(text, slot, body);
}

// The factory pair, as a fresh card carries it: one generation per bank.
void stampFactoryPair(const fs::path& volume, const std::string& text)
{
    for (const int fileNo : { 1, 2 })
        commands::writeFileBytes(volume::memoryPath(volume, fileNo), rc0::setTailMarker(text, fileNo));
}

long long rhythmField(const std::string& text, int slot, const char* tag)
{
    return rc0::sectionField(rc0::slotBody(text, slot), rc0::kSectionRhythm, tag);
}

} // namespace

int main()
{
    // --- the premise: the card's numbers are the RC-5's ---

    CHECK_EQ(kReadRhythmOn, rc0::kRhythmStateOn);
    CHECK_EQ(kReadBlank, rc0::kRhythmPatternBlank);
    CHECK_EQ(kReadFourFour, usecases::beat::kBeatFourFour);
    CHECK(usecases::beat::patternListCharted(kReadFourFour));
    CHECK(!usecases::beat::patternListCharted(kSixFour));

    // --- the profile: the bit is open, and a closed command's sentence lists
    // it where the enum puts it - between One Shot and push. The expected
    // words are the brief's list laid in that order, not the code's own
    // output copied back.

    profile::require(profile::kRc500, profile::Operation::setCountIn); // no throw
    CHECK(profile::kRc500.allows(profile::Operation::setCountIn));
    CHECK_EQ(profile::openOperations(profile::kRc500),
             std::string("rename, set one-shot, set count-in, push, pull, clear and restore"));
    for (const auto op : { profile::Operation::setRhythm, profile::Operation::setPlayStop,
                           profile::Operation::setTempo })
        CHECK_THROWS(profile::require(profile::kRc500, op),
                     std::string(profile::operationName(op)) + " refused on an \"RC-500\" card -"
                         " on this model LooperCat does rename, set one-shot, set count-in, push,"
                         " pull, clear and restore only");

    // --- the card ---
    //
    // Memory 1: a take on each track, the rhythm silent at 4/4 (the factory
    // shape). Memory 3: a groove playing at 4/4 - Rock2 on kit 3, the
    // player's. Memory 7: the same groove playing at 6/4. Memory 11: track 2
    // alone, silent at 6/4 with the pedal's Rock2 there. Memory 42: as the
    // factory left it.
    TempDir tmp;
    const fs::path volume = testkit::makeTwoTrackCard(
        tmp.path, { { 1, 1, "001_1.WAV", 44100, 1 }, { 1, 2, "001_2.WAV", 22050, 2 },
                    { 11, 2, "011_2.WAV", 8820, 5 } });
    {
        std::string text = commands::readMemory(volume);
        text = withRhythm(text, 3, { { "State", kReadRhythmOn }, { "Pattern", kRock2At44 }, { "Kit", 3 } });
        text = withRhythm(text, 7, { { "State", kReadRhythmOn }, { "Beat", kSixFour }, { "Pattern", kRock2At64 } });
        text = withRhythm(text, 11, { { "Beat", kSixFour }, { "Pattern", kRock2At64 } });
        stampFactoryPair(volume, text);
    }
    const std::string card = commands::readMemory(volume);
    const auto memory = [&volume] { return commands::readMemory(volume); };
    const auto bank = [&volume](int fileNo) {
        return commands::readFileBytes(volume::memoryPath(volume, fileNo));
    };
    const auto takesBefore = takeBytes(volume);
    const std::uint32_t factoryNewest = std::max(rc0::tailMarkerFor(1), rc0::tailMarkerFor(2));

    // The catalog's view before a click: a switch on the silent memory, a
    // switch beside the groove (nothing at risk: the groove stays), a lamp on
    // the 6/4 memory whose borrow the core would refuse.
    {
        const auto memories = catalog::listSlots(card);
        CHECK(!memories.at(0).countIn && !memories.at(0).countInRefused
              && !memories.at(0).countInTakesPattern);
        CHECK(!memories.at(2).countIn && !memories.at(2).countInRefused
              && !memories.at(2).countInTakesPattern);
        CHECK(!memories.at(6).countInRefused); // the count alone is open at 6/4
        CHECK(memories.at(10).countInRefused == usecases::countin::Refusal::onBorrowsSection);
        CHECK_EQ(memories.at(0).tracks.size(), 2u);
    }

    // --- on over a silent section: the triple, and not one byte elsewhere ---

    commands::setCountIn(volume, { 1 }, true, writeOpts(tmp.path, "on-1"));
    const std::string onOne = memory();
    CHECK_EQ(testkit::slotChangesOnCard(card, onOne, 1),
             std::string("RHYTHM.Pattern 0->57, RHYTHM.PlayCount 0->1, RHYTHM.State 0->1"));
    CHECK(catalog::readSlot(onOne, 1).countIn);
    CHECK(section(rc0::slotBody(onOne, 1), "TRACK1") == section(rc0::slotBody(card, 1), "TRACK1"));
    CHECK(section(rc0::slotBody(onOne, 1), "TRACK2") == section(rc0::slotBody(card, 1), "TRACK2"));
    CHECK(takeBytes(volume) == takesBefore);
    // The pair: one document in both banks, each stamped past the highest
    // generation the card held - the fixture's factory pair.
    CHECK(rc0::splitFile(bank(1)).document == rc0::splitFile(bank(2)).document);
    CHECK_EQ(rc0::tailMarker(bank(1)).value(), factoryNewest + 1);
    CHECK_EQ(rc0::tailMarker(bank(2)).value(), factoryNewest + 2);

    // --- on over a playing groove: the count alone, the groove untouched ---

    commands::setCountIn(volume, { 3 }, true, writeOpts(tmp.path, "on-3"));
    const std::string onThree = memory();
    CHECK_EQ(testkit::slotChangesOnCard(onOne, onThree, 3), std::string("RHYTHM.PlayCount 0->1"));
    CHECK(catalog::readSlot(onThree, 3).countIn);
    CHECK_EQ(rhythmField(onThree, 3, "Pattern"), kRock2At44);
    CHECK_EQ(rhythmField(onThree, 3, "Kit"), 3);
    CHECK_EQ(rc0::tailMarker(bank(1)).value(), factoryNewest + 3);
    CHECK_EQ(rc0::tailMarker(bank(2)).value(), factoryNewest + 4);

    // --- off: the groove stays where it was; the borrowed section goes back ---

    commands::setCountIn(volume, { 3 }, false, writeOpts(tmp.path, "off-3"));
    const std::string offThree = memory();
    CHECK_EQ(testkit::slotChangesOnCard(onThree, offThree, 3), std::string("RHYTHM.PlayCount 1->0"));
    CHECK(rc0::slotBody(offThree, 3) == rc0::slotBody(card, 3));
    commands::setCountIn(volume, { 1 }, false, writeOpts(tmp.path, "off-1"));
    const std::string offOne = memory();
    CHECK_EQ(testkit::slotChangesOnCard(offThree, offOne, 1),
             std::string("RHYTHM.Pattern 57->0, RHYTHM.PlayCount 1->0, RHYTHM.State 1->0"));
    // The feature keeps no hidden state: the document is back to the byte.
    CHECK(rc0::splitFile(offOne).document == rc0::splitFile(card).document);
    CHECK(takeBytes(volume) == takesBefore);

    // --- at 6/4 the borrow is refused with the #149 words, and nothing moves ---

    {
        const auto before = volumeBytes(volume);
        CHECK_THROWS(commands::setCountIn(volume, { 11 }, true, writeOpts(tmp.path, "on-11")),
                     "Switching the count-in on at 6/4");
        CHECK_THROWS(commands::setCountIn(volume, { 11 }, true, writeOpts(tmp.path, "on-11b")),
                     std::string(usecases::beat::kOnlyFourFourCharted));
        CHECK(volumeBytes(volume) == before);
        CHECK_EQ(rhythmField(memory(), 11, "Pattern"), kRock2At64);
        // One refused memory refuses the batch: a 4/4 memory ahead of it in
        // the list does not land first.
        CHECK_THROWS(commands::setCountIn(volume, { 42, 11 }, true, writeOpts(tmp.path, "on-42-11")),
                     "at 6/4");
        CHECK(volumeBytes(volume) == before);
        CHECK(!catalog::readSlot(memory(), 42).countIn);
        CHECK(!fs::exists(tmp.path / "archive"));
    }

    // --- the count alone at 6/4 is still the player's ---

    commands::setCountIn(volume, { 7 }, true, writeOpts(tmp.path, "on-7"));
    const std::string onSeven = memory();
    CHECK_EQ(testkit::slotChangesOnCard(offOne, onSeven, 7), std::string("RHYTHM.PlayCount 0->1"));
    CHECK(catalog::readSlot(onSeven, 7).countIn);
    CHECK_EQ(rhythmField(onSeven, 7, "Pattern"), kRock2At64);
    CHECK_EQ(rhythmField(onSeven, 7, "Beat"), kSixFour);
    commands::setCountIn(volume, { 7 }, false, writeOpts(tmp.path, "off-7"));
    CHECK(rc0::splitFile(memory()).document == rc0::splitFile(card).document);

    // --- what stays closed says so, the count-in now among what is open ---

    {
        const auto before = volumeBytes(volume);
        CHECK_THROWS(commands::setTempo(volume, 1, 1200, writeOpts(tmp.path, "tempo")),
                     "set tempo refused on an \"RC-500\" card - on this model LooperCat does rename,"
                     " set one-shot, set count-in, push, pull, clear and restore only");
        CHECK_THROWS(commands::setRhythm(volume, 1, { .on = true }, writeOpts(tmp.path, "rhythm")),
                     "set rhythm refused on an \"RC-500\" card");
        CHECK(volumeBytes(volume) == before);
    }

    // --- the bit is this model's: a card of a model this app does not know
    // is refused at the door, the count-in included ---

    {
        TempDir foreign;
        const fs::path unknown = testkit::makeTwoTrackCard(foreign.path, {}, "RC-505");
        const std::string root = "<database name=\"RC-500\" revision=\"0\">";
        for (const int fileNo : { 1, 2 }) {
            std::string text = commands::readFileBytes(volume::memoryPath(unknown, fileNo));
            text.replace(text.find(root), root.size(), "<database name=\"RC-505\" revision=\"0\">");
            commands::writeFileBytes(volume::memoryPath(unknown, fileNo), text);
        }
        const auto before = volumeBytes(unknown);
        CHECK_THROWS(commands::setCountIn(unknown, { 1 }, true, writeOpts(foreign.path, "foreign")),
                     "this is an \"RC-505\" card, not an RC-5");
        CHECK(volumeBytes(unknown) == before);
    }

    return testkit::summary("rc500_count_in");
}
