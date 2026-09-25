// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The RC family as a table, and the guard that asks it. From the theory of
// the cards and the measurements behind the table, not from the code:
//
//   - every constant of a profile is the measured one, and the RC-5's model
//     id is the one the Connect frames carry
//   - the RC-5's factory body reproduces every never-touched memory of a real
//     card, byte for byte; the two-track model has none on record and none
//     is made up
//   - a document is identified by its root name and nothing else; an unknown
//     name is refused by name, in the words the guard has always used
//   - a document's track sections must fit its model: an "RC-5" with a
//     <TRACK2> is refused (the guard's oldest promise), an "RC-500" with a
//     <TRACK3> or with no <TRACK2> the same way
//   - the RC-5 is open for everything; the two-track model for nothing, and
//     each refusal names the model and the operation

#include "support.hpp"

#include <loopercat/DeviceProfile.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/Sysex.hpp>

#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace loopercat;

namespace {

std::string cardText()
{
    std::ifstream in(LOOPERCAT_RC5_CARD, std::ios::binary);
    if (!in)
        throw Error("cannot open the card fixture: " LOOPERCAT_RC5_CARD);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

constexpr profile::Operation kEveryOp[] = {
    profile::Operation::read,      profile::Operation::rename,    profile::Operation::setOneShot,
    profile::Operation::setTempo,  profile::Operation::setCountIn, profile::Operation::push,
    profile::Operation::pull,      profile::Operation::trim,      profile::Operation::downmix,
    profile::Operation::normalize, profile::Operation::clear,     profile::Operation::restore,
    profile::Operation::swap,      profile::Operation::setControls,
};

// The synthetic RC-5 file with its root line swapped.
std::string withRoot(std::string text, const std::string& root)
{
    const std::string original = "<database name=\"RC-5\" revision=\"0\">";
    const auto at = text.find(original);
    if (at == std::string::npos)
        throw Error("test fixture: root line not found");
    return text.replace(at, original.size(), root);
}

} // namespace

int main()
{
    // --- the table ---

    CHECK_EQ(sizeof(kEveryOp) / sizeof(kEveryOp[0]), profile::kOperationCount);

    {
        const auto& rc5 = profile::kRc5;
        CHECK_EQ(rc5.familyName, "RC-5");
        CHECK(rc5.modelId == (std::array<std::uint8_t, 4> { 0x00, 0x00, 0x00, 0x76 }));
        CHECK(rc5.modelId == sysex::kModelRc5); // the frames Connect sends carry this id
        CHECK_EQ(rc5.usbProductId, 0x0251);
        CHECK_EQ(rc5.trackCount, 1);
        CHECK_EQ(rc5.slotCount, rc0::kSlotCount);
        CHECK(rc5.hasFactoryBody());
        CHECK(rc5.allowsWrites());
        for (const auto op : kEveryOp)
            CHECK(rc5.allows(op));
    }
    {
        const auto& rc500 = profile::kRc500;
        CHECK_EQ(rc500.familyName, "RC-500");
        CHECK(rc500.modelId == (std::array<std::uint8_t, 4> { 0x00, 0x00, 0x00, 0x77 }));
        CHECK_EQ(rc500.usbProductId, 0x0252);
        CHECK_EQ(rc500.trackCount, 2);
        CHECK_EQ(rc500.slotCount, rc0::kSlotCount);
        CHECK(!rc500.hasFactoryBody());
        CHECK(!rc500.allowsWrites());
        // Reading, and reading alone: the browser may list such a card;
        // nothing may change it, pull included.
        CHECK_EQ(rc500.operations, profile::bit(profile::Operation::read));
        for (const auto op : kEveryOp)
            CHECK(rc500.allows(op) == (op == profile::Operation::read));
    }
    // Two entries, distinct names, distinct ids: a lookup can never answer twice.
    {
        std::set<std::string_view> names;
        std::set<std::array<std::uint8_t, 4>> ids;
        std::set<std::uint16_t> pids;
        for (const auto* p : profile::kAll) {
            names.insert(p->familyName);
            ids.insert(p->modelId);
            pids.insert(p->usbProductId);
            CHECK_EQ(p->slotCount, rc0::kSlotCount); // 99 memories on every model of the family
            CHECK(p->trackCount >= 1);
        }
        CHECK_EQ(names.size(), profile::kAll.size());
        CHECK_EQ(ids.size(), profile::kAll.size());
        CHECK_EQ(pids.size(), profile::kAll.size());
    }
    // Every operation has a name of its own.
    {
        std::set<std::string_view> names;
        for (const auto op : kEveryOp) {
            CHECK(!profile::operationName(op).empty());
            names.insert(profile::operationName(op));
        }
        CHECK_EQ(names.size(), profile::kOperationCount);
    }
    // The set arithmetic: everything, nothing, and the writes.
    CHECK_EQ(profile::operations({}), profile::kNoOperation);
    CHECK_EQ(profile::operations({ profile::Operation::read, profile::Operation::pull }),
             profile::kEveryOperation & ~profile::kWrites);
    CHECK((profile::kWrites & profile::bit(profile::Operation::read)) == 0);
    CHECK((profile::kWrites & profile::bit(profile::Operation::pull)) == 0);
    CHECK((profile::kWrites & profile::bit(profile::Operation::swap)) != 0);

    // --- lookup by the root name ---

    CHECK(&profile::byFamilyName("RC-5") == &profile::kRc5);
    CHECK(&profile::byFamilyName("RC-500") == &profile::kRc500);
    // The same lookup for a caller with a name and no card: nothing, not a throw.
    CHECK(profile::findFamily("RC-5") == &profile::kRc5);
    CHECK(profile::findFamily("RC-500") == &profile::kRc500);
    CHECK(profile::findFamily("RC-505") == nullptr);
    CHECK(profile::findFamily("") == nullptr);
    // Unknown, and every near-miss, refused by name in the guard's own words.
    for (const char* name : { "RC-505", "RC-600", "RC-10R", "rc-5", "RC-5 ", "", "RC" }) {
        CHECK_THROWS(profile::byFamilyName(name), "not an RC-5");
        CHECK_THROWS(profile::byFamilyName(name), std::string("\"") + name + "\" card");
        CHECK_THROWS(profile::byFamilyName(name), "LooperCat only speaks RC-5");
    }

    // --- the gate ---

    for (const auto op : kEveryOp)
        profile::require(profile::kRc5, op); // no throw
    // A closed read is a card that is not ours, in the words it has always
    // been said in; a closed mutation names itself and the model. The
    // two-track model's read is open, so the read refusal is asked of a
    // profile that has nothing open.
    constexpr profile::DeviceProfile closed {
        "RC-500", { 0x00, 0x00, 0x00, 0x77 }, 0x0252, 2, 99, {}, profile::kNoOperation
    };
    CHECK_THROWS(profile::require(closed, profile::Operation::read),
                 "this is an \"RC-500\" card, not an RC-5 \xe2\x80\x94 LooperCat only speaks RC-5");
    profile::require(profile::kRc500, profile::Operation::read); // open: no throw
    for (const auto op : kEveryOp) {
        if (op == profile::Operation::read)
            continue;
        CHECK_THROWS(profile::require(profile::kRc500, op),
                     std::string(profile::operationName(op)) + " refused on an \"RC-500\" card");
        CHECK_THROWS(profile::require(profile::kRc500, op), "LooperCat only speaks RC-5");
    }
    profile::requireWrites(profile::kRc5);
    CHECK_THROWS(profile::requireWrites(profile::kRc500), "no write on an \"RC-500\" card");

    // --- the factory body ---

    for (const int slot : { 1, 42, 99 })
        CHECK(rc0::factorySlotBody(profile::kRc5, slot) == rc0::factorySlotBody(slot));
    CHECK_THROWS(rc0::factorySlotBody(profile::kRc5, 0), "out of range");
    CHECK_THROWS(rc0::factorySlotBody(profile::kRc5, 100), "out of range");
    CHECK_THROWS(rc0::factorySlotBody(profile::kRc500, 1), "not on record");
    CHECK_THROWS(rc0::factorySlotBody(profile::kRc500, 0), "out of range"); // the range first

    // The claim the table makes about the RC-5's factory body, against a real
    // card (fixtures/rc5-card.RC0): every memory that reads as factory-empty
    // (Measure=1, MeasLen=0, WavStat=0) is byte-identical to it — the 57 of
    // them, none of which had ever been used, or was cleared back to it.
    {
        const std::string card = cardText();
        CHECK(&rc0::profileOf(card) == &profile::kRc5);
        CHECK(&rc0::assertMemoryFile(card) == &profile::kRc5);
        int factoryEmpty = 0, identical = 0;
        for (int slot = 1; slot <= rc0::kSlotCount; ++slot) {
            const std::string body = rc0::slotBody(card, slot);
            if (rc0::sectionField(body, rc0::kSectionTrack1, "Measure") != 1
                || rc0::sectionField(body, rc0::kSectionTrack1, "MeasLen") != 0
                || rc0::sectionField(body, rc0::kSectionTrack1, "WavStat") != 0)
                continue;
            ++factoryEmpty;
            if (body == rc0::factorySlotBody(profile::kRc5, slot))
                ++identical;
        }
        CHECK_EQ(factoryEmpty, 57);
        CHECK_EQ(identical, 57);
    }

    // --- documents: identified by the root, shaped for the model ---

    const std::string rc5 = testkit::syntheticMemoryText();
    const std::string twoTrack = testkit::syntheticTwoTrackMemoryText();
    CHECK(&rc0::profileOf(rc5) == &profile::kRc5);
    CHECK(&rc0::profileOf(twoTrack) == &profile::kRc500);
    CHECK(&rc0::assertMemoryFile(rc5) == &profile::kRc5);
    CHECK(&rc0::assertMemoryFile(twoTrack) == &profile::kRc500);

    // The guard's oldest promise: a document that claims RC-5 and carries a
    // second track is refused, and the refusal still says what the app speaks.
    {
        std::string liar = rc5;
        const auto at = liar.find("</TRACK1>");
        CHECK(at != std::string::npos);
        liar.insert(at + std::string("</TRACK1>").size(), "\n<TRACK2>\n\t<Rev>0</Rev>\n</TRACK2>");
        CHECK_THROWS(rc0::assertMemoryFile(liar), "carries <TRACK2> sections");
        CHECK_THROWS(rc0::assertMemoryFile(liar), "more tracks than an \"RC-5\" has");
        CHECK_THROWS(rc0::assertMemoryFile(liar), "LooperCat only speaks RC-5");
    }
    // The same rule the other way: a two-track root over one-track memories,
    // and a two-track root over memories with a third track.
    CHECK_THROWS(rc0::assertMemoryFile(withRoot(rc5, "<database name=\"RC-500\" revision=\"0\">")),
                 "carries no <TRACK2> section");
    {
        std::string three = twoTrack;
        const auto at = three.find("</TRACK2>");
        CHECK(at != std::string::npos);
        three.insert(at + std::string("</TRACK2>").size(), "\n<TRACK3>\n\t<Rev>0</Rev>\n</TRACK3>");
        CHECK_THROWS(rc0::assertMemoryFile(three), "carries <TRACK3> sections");
    }
    // An unknown model over a perfectly shaped body is still unknown.
    CHECK_THROWS(rc0::assertMemoryFile(withRoot(rc5, "<database name=\"RC-505\" revision=\"0\">")),
                 "this is an \"RC-505\" card, not an RC-5");
    // <TRACK1> is where the RC-5's one track lives; a document without it is
    // not a memory file of any model.
    {
        std::string noTrack = rc5;
        std::size_t at;
        while ((at = noTrack.find("TRACK1")) != std::string::npos)
            noTrack.replace(at, 6, "TRACKX");
        CHECK_THROWS(rc0::assertMemoryFile(noTrack), "carries no <TRACK1> section");
    }
    // The track sections are named by number, for every model.
    CHECK_EQ(rc0::trackSectionName(1), rc0::kSectionTrack1);
    CHECK_EQ(rc0::trackSectionName(2), "TRACK2");
    CHECK_EQ(rc0::trackSectionOpen(2), "<TRACK2>");

    // --- the family name the app speaks by is the table's ---

    CHECK_EQ(rc0::kFamilyName, profile::kRc5.familyName);

    return testkit::summary("device_profile");
}
