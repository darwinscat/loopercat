// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The rhythm as a feature, tested from what the PLAYER hears and from the
// manual's page, not from the fields the implementation writes. The theory:
//
//   1. Drums are heard when the rhythm section is on AND a groove is chosen.
//      Blank is the count-in's silence: on + Blank is not drums.
//   2. The switch moves the fewest bytes the pedal's own model allows: State
//      is the switch, Pattern the selection. Off keeps the selection — except
//      with a count-in on, where State must stay and the groove goes Blank.
//   3. The feature keeps no hidden state: what it forgets, it says it will
//      forget (patternLostOnOff), and a slot that only ever had its rhythm
//      switched returns to the exact bytes it started with.
//   4. Names and ranges are the manual's (p. 10): a number outside a list is
//      a typed error carrying the number, never a guess; a value outside a
//      range is refused before any byte moves.
//   5. BEAT cannot be changed once a track is recorded (p. 10, stated).
//   6. The RHYTHM section is the address: the same tag elsewhere is nobody's.
//   7. PATTERN indexes the list of the memory's CURRENT BEAT, and only the
//      4/4 list is charted (hardware, 2026-10-01: Rock2 is 12 at 4/4 and 3
//      at 6/4). At any other beat the number is read and never named, no
//      edit writes <Pattern>, and a refusal names the beat as the screen
//      prints it — "6/4", not 4.

#include "support.hpp"

#include <loopercat/Catalog.hpp>
#include <loopercat/usecases/Beat.hpp>
#include <loopercat/usecases/CountIn.hpp>
#include <loopercat/usecases/Rhythm.hpp>

#include <string>

using namespace loopercat;
namespace rhythm = loopercat::usecases::rhythm;

namespace
{
    constexpr long long kSomeGroove = 11; // Rock1 in the manual's order: neither Blank nor factory 0

    // The hardware pairing of #147: one groove, two numbers, one per beat.
    constexpr long long kFourFour = 2;  // Beat 2 reads 4/4 on the screen
    constexpr long long kSixFour = 4;   // Beat 4 reads 6/4 on the screen (2026-10-01)
    constexpr long long kRock2At44 = 12; // Rock2 chosen on the pedal at 4/4 is stored as 12
    constexpr long long kRock2At64 = 3;  // the same Rock2 at 6/4 is stored as 3

    std::string bodyWith(long long state, long long playCount, long long pattern)
    {
        std::string body = testkit::syntheticSlotBody();
        body = rc0::setField(body, "State", state);
        body = rc0::setField(body, "PlayCount", playCount);
        body = rc0::setField(body, "Pattern", pattern);
        return body;
    }

    std::string withTake(std::string body)
    {
        body = rc0::setField(body, "WavStat", rc0::kWavStatIndexed);
        return rc0::setField(body, "WavLen", 44100);
    }

    // Byte proof that nothing outside the named fields moved: restore them to
    // the values the original carried, and the two bodies must be identical.
    bool onlyTheseFieldsMoved(const std::string& before, std::string after,
                              std::initializer_list<const char*> fields)
    {
        for (const char* tag : fields)
            after = rc0::setSectionField(after, rc0::kSectionRhythm, tag,
                                         rc0::sectionField(before, rc0::kSectionRhythm, tag));
        return after == before;
    }

    long long rhythmField(const std::string& body, const char* tag)
    {
        return rc0::sectionField(body, rc0::kSectionRhythm, tag);
    }

    std::string atBeat(std::string body, long long beat)
    {
        return rc0::setSectionField(body, rc0::kSectionRhythm, "Beat", beat);
    }

    // The same body at 6/4, holding `pattern` — a number from the 6/4 list,
    // which we cannot name.
    std::string atSixFour(std::string body, long long pattern)
    {
        return rc0::setSectionField(atBeat(std::move(body), kSixFour), rc0::kSectionRhythm,
                                    "Pattern", pattern);
    }
}

int main()
{
    const std::string factory = testkit::syntheticSlotBody();
    // Memory 3 as the pedal wrote it (#147): 6/4, Rock2, which the card stores as 3.
    const std::string sixFourRock2 = atSixFour(factory, kRock2At64);

    // --- the lists are the manual's, counted from zero ---

    // The two hardware anchors and the printed defaults.
    CHECK_EQ(rhythm::patternName(rc0::kRhythmPatternBlank), "Blank");
    CHECK_EQ(rc0::kRhythmPatternBlank, 57); // 57 grooves, then Blank: the count is the anchor
    CHECK_EQ(rhythm::patternName(0), "SimpleBeat1");
    CHECK_EQ(rhythm::patternName(56), "Metronome4");
    CHECK_EQ(rhythm::patternName(kSomeGroove), "Rock1");
    CHECK_EQ(rhythm::beatName(2), "4/4");
    CHECK_EQ(rhythm::beatName(0), "2/4");
    CHECK_EQ(rhythm::beatName(5), "7/4");
    CHECK_EQ(rhythm::beatName(6), "5/8"); // the /4 block ends, the /8 block begins
    CHECK_EQ(rhythm::beatName(16), "15/8");
    CHECK_EQ(rhythm::kitName(0), "Studio");
    CHECK_EQ(rhythm::kitName(6), "808+909");
    CHECK_EQ(rhythm::variationName(0), "A");
    CHECK_EQ(rhythm::variationName(1), "B");

    // Every list is dense — number i sits at position i — and no name repeats:
    // a list with a hole or a twin would put one name on two numbers.
    {
        const auto dense = [](auto list) {
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].number != static_cast<long long>(i) || list[i].name.empty())
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (list[j].name == list[i].name)
                        return false;
            }
            return true;
        };
        CHECK(dense(rhythm::kPatterns));
        CHECK(dense(rhythm::kKits));
        CHECK(dense(rhythm::kBeats));
        CHECK(dense(rhythm::kVariations));
        CHECK_EQ(rhythm::kPatterns.size(), 58u);
        CHECK_EQ(rhythm::kKits.size(), 7u);
        CHECK_EQ(rhythm::kBeats.size(), 17u);
    }

    // A number outside a list is a typed error that carries the number.
    CHECK_THROWS(rhythm::patternName(58), "PATTERN 58");
    CHECK_THROWS(rhythm::patternName(-1), "PATTERN -1");
    CHECK_THROWS(rhythm::kitName(7), "KIT 7");
    CHECK_THROWS(rhythm::beatName(17), "BEAT 17");
    CHECK_THROWS(rhythm::variationName(2), "VARIATION 2");

    // TONE on the screen is the file minus ten, both ways, at both ends.
    CHECK_EQ(rhythm::toneOnScreen(10), 0);
    CHECK_EQ(rhythm::toneOnScreen(0), -10);
    CHECK_EQ(rhythm::toneOnScreen(20), 10);
    CHECK_EQ(rhythm::toneInFile(-10), 0);
    CHECK_EQ(rhythm::toneInFile(10), 20);

    // --- what the player hears (reading) ---

    // Factory: rhythm off, SimpleBeat1 selected but silent.
    {
        const rhythm::Values v = rhythm::read(factory);
        CHECK(!v.on);
        CHECK_EQ(v.pattern, 0);
        CHECK_EQ(v.kit, 0);
        CHECK_EQ(v.beat, 2);
        CHECK_EQ(v.variation, 0);
        CHECK_EQ(v.level, 100);
        CHECK_EQ(v.reverb, 0); // the synthetic body's value, not the factory 30 — no accident passes
        CHECK_EQ(v.toneLow, 0);
        CHECK_EQ(v.toneHigh, 0);
        CHECK(!v.beatLocked);
        CHECK(!rhythm::isOn(factory));
    }

    // On with a groove: drums.
    CHECK(rhythm::isOn(bodyWith(rc0::kRhythmStateOn, 0, kSomeGroove)));
    // On with Blank: the count-in's silence, not drums — whatever PlayCount says.
    CHECK(!rhythm::isOn(bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas,
                                 rc0::kRhythmPatternBlank)));
    CHECK(!rhythm::isOn(bodyWith(rc0::kRhythmStateOn, 0, rc0::kRhythmPatternBlank)));
    // A groove chosen with the section off: silence.
    CHECK(!rhythm::isOn(bodyWith(0, 0, kSomeGroove)));
    // The count-in and the drums are two questions with two answers.
    {
        const std::string both
            = bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, kSomeGroove);
        CHECK(rhythm::isOn(both));
        CHECK(usecases::countin::isOn(both));
    }

    // The table's read model carries the same answer.
    {
        std::string text = testkit::syntheticMemoryText();
        text = rc0::replaceSlotBody(text, 4, bodyWith(rc0::kRhythmStateOn, 0, kSomeGroove));
        const catalog::SlotInfo info = catalog::readSlot(text, 4);
        CHECK(info.rhythm.on);
        CHECK_EQ(info.rhythm.pattern, kSomeGroove);
        CHECK(!catalog::readSlot(text, 5).rhythm.on);
    }

    // --- the switch ---

    // On over a silent factory slot: State alone moves; the selected groove
    // (SimpleBeat1) is what plays.
    {
        const std::string after = rhythm::applySwitch(factory, true);
        CHECK(rhythm::isOn(after));
        CHECK_EQ(rhythmField(after, "Pattern"), 0);
        CHECK(onlyTheseFieldsMoved(factory, after, { "State" }));
    }

    // On over a chosen-but-silent groove: the groove is kept, State alone moves.
    {
        const std::string before = bodyWith(0, 0, kSomeGroove);
        const std::string after = rhythm::applySwitch(before, true);
        CHECK(rhythm::isOn(after));
        CHECK_EQ(rhythmField(after, "Pattern"), kSomeGroove);
        CHECK(onlyTheseFieldsMoved(before, after, { "State" }));
    }

    // On over the count-in's silence: the count stays, and the groove is the
    // printed default — not a memory of anything, there is none.
    {
        const std::string before
            = bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, rc0::kRhythmPatternBlank);
        const std::string after = rhythm::applySwitch(before, true);
        CHECK(rhythm::isOn(after));
        CHECK(usecases::countin::isOn(after));
        CHECK_EQ(rhythmField(after, "Pattern"), rhythm::kPatternDefault);
        CHECK(onlyTheseFieldsMoved(before, after, { "Pattern" }));
    }

    // On over Blank with the section off (set so on the pedal): both move.
    {
        const std::string before = bodyWith(0, 0, rc0::kRhythmPatternBlank);
        const std::string after = rhythm::applySwitch(before, true);
        CHECK(rhythm::isOn(after));
        CHECK(onlyTheseFieldsMoved(before, after, { "State", "Pattern" }));
    }

    // On twice is on.
    {
        const std::string once = rhythm::applySwitch(factory, true);
        CHECK(rhythm::applySwitch(once, true) == once);
    }

    // Off with no count-in: State alone; the groove stays selected, as the
    // pedal itself keeps it.
    {
        const std::string before = bodyWith(rc0::kRhythmStateOn, 0, kSomeGroove);
        const std::string after = rhythm::applySwitch(before, false);
        CHECK(!rhythm::isOn(after));
        CHECK_EQ(rhythmField(after, "Pattern"), kSomeGroove);
        CHECK(onlyTheseFieldsMoved(before, after, { "State" }));
    }

    // Off with the count-in on: the count survives (State stays), the groove
    // becomes Blank — and the count-in reads as still on afterwards.
    {
        const std::string before
            = bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, kSomeGroove);
        const std::string after = rhythm::applySwitch(before, false);
        CHECK(!rhythm::isOn(after));
        CHECK(usecases::countin::isOn(after));
        CHECK_EQ(rhythmField(after, "Pattern"), rc0::kRhythmPatternBlank);
        CHECK(onlyTheseFieldsMoved(before, after, { "Pattern" }));
    }

    // Off on a factory slot changes nothing; off twice is off.
    CHECK(rhythm::applySwitch(factory, false) == factory);
    {
        const std::string before = bodyWith(rc0::kRhythmStateOn, 0, kSomeGroove);
        const std::string once = rhythm::applySwitch(before, false);
        CHECK(rhythm::applySwitch(once, false) == once);
    }

    // No hidden state: on then off over the factory slot is the factory slot.
    CHECK(rhythm::applySwitch(rhythm::applySwitch(factory, true), false) == factory);

    // --- the two features, in sequence, as a player would use them ---
    {
        // Drums on, then a count-in in front of them: the count-in moves
        // PlayCount only (CountIn.hpp's rule), the groove keeps playing.
        std::string body = rhythm::applySwitch(bodyWith(0, 0, kSomeGroove), true);
        body = usecases::countin::apply(body, true);
        CHECK(rhythm::isOn(body));
        CHECK(usecases::countin::isOn(body));
        CHECK_EQ(rhythmField(body, "Pattern"), kSomeGroove);

        // Drums off: count stays, groove forgotten — as warned.
        CHECK_EQ(*rhythm::patternLostOnOff(body), kSomeGroove);
        body = rhythm::applySwitch(body, false);
        CHECK(usecases::countin::isOn(body));
        CHECK_EQ(rhythmField(body, "Pattern"), rc0::kRhythmPatternBlank);

        // Drums on again: the default groove, not the forgotten one.
        body = rhythm::applySwitch(body, true);
        CHECK_EQ(rhythmField(body, "Pattern"), rhythm::kPatternDefault);
        CHECK(usecases::countin::isOn(body));

        // Count-in off now hands back nothing of ours: the drums stay on.
        body = usecases::countin::apply(body, false);
        CHECK(rhythm::isOn(body));
        CHECK(!usecases::countin::isOn(body));
    }

    // --- the one thing worth warning about ---

    // Only "drums on AND count-in on" forgets a groove on off.
    CHECK(!rhythm::patternLostOnOff(factory).has_value());
    CHECK(!rhythm::patternLostOnOff(bodyWith(rc0::kRhythmStateOn, 0, kSomeGroove)).has_value());
    CHECK(!rhythm::patternLostOnOff(bodyWith(0, rc0::kRhythmPlayCount1Meas, kSomeGroove))
               .has_value());
    CHECK(!rhythm::patternLostOnOff(
               bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, rc0::kRhythmPatternBlank))
               .has_value());
    // SimpleBeat1 is a real groove here: with the drums on, it is what plays.
    CHECK_EQ(*rhythm::patternLostOnOff(bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, 0)),
             0);

    // --- edits: each field alone, only that field moves ---

    {
        const std::string after = rhythm::apply(factory, { .pattern = kSomeGroove });
        CHECK_EQ(rhythmField(after, "Pattern"), kSomeGroove);
        CHECK(onlyTheseFieldsMoved(factory, after, { "Pattern" }));
        CHECK(!rhythm::isOn(after)); // choosing a groove does not switch the drums on
    }
    {
        const std::string after = rhythm::apply(factory, { .kit = 6 });
        CHECK_EQ(rhythm::read(after).kit, 6);
        CHECK(onlyTheseFieldsMoved(factory, after, { "Kit" }));
    }
    {
        const std::string after = rhythm::apply(factory, { .beat = 16 });
        CHECK_EQ(rhythm::read(after).beat, 16);
        CHECK(onlyTheseFieldsMoved(factory, after, { "Beat" }));
    }
    {
        const std::string after = rhythm::apply(factory, { .variation = 1 });
        CHECK_EQ(rhythm::read(after).variation, 1);
        CHECK(onlyTheseFieldsMoved(factory, after, { "Variation" }));
    }
    // The RHYTHM Level, never the MASTER one under the same tag.
    {
        const std::string after = rhythm::apply(factory, { .level = 200 });
        CHECK_EQ(rhythm::read(after).level, 200);
        CHECK_EQ(rc0::sectionField(after, rc0::kSectionMaster, "Level"), 100);
        CHECK(onlyTheseFieldsMoved(factory, after, { "Level" }));
    }
    {
        const std::string after = rhythm::apply(factory, { .reverb = 100 });
        CHECK_EQ(rhythm::read(after).reverb, 100);
        CHECK(onlyTheseFieldsMoved(factory, after, { "Reverb" }));
    }
    // Tone: the screen's number in, the file's number stored, both ends.
    {
        const std::string after = rhythm::apply(factory, { .toneLow = -10, .toneHigh = 10 });
        CHECK_EQ(rhythmField(after, "ToneLow"), 0);
        CHECK_EQ(rhythmField(after, "ToneHigh"), 20);
        CHECK_EQ(rhythm::read(after).toneLow, -10);
        CHECK_EQ(rhythm::read(after).toneHigh, 10);
        CHECK(onlyTheseFieldsMoved(factory, after, { "ToneLow", "ToneHigh" }));
    }
    // The switch inside an edit, together with a choice: on, and Rock1.
    {
        const std::string after = rhythm::apply(factory, { .on = true, .pattern = kSomeGroove });
        CHECK(rhythm::isOn(after));
        CHECK_EQ(rhythmField(after, "Pattern"), kSomeGroove);
        CHECK(onlyTheseFieldsMoved(factory, after, { "State", "Pattern" }));
    }
    // Writing what the slot already has is byte-identical output.
    {
        const rhythm::Values v = rhythm::read(factory);
        const std::string same = rhythm::apply(
            factory, { .on = v.on, .pattern = v.pattern, .kit = v.kit, .beat = v.beat,
                       .variation = v.variation, .level = v.level, .reverb = v.reverb,
                       .toneLow = v.toneLow, .toneHigh = v.toneHigh });
        CHECK(same == factory);
    }

    // --- edits: refused before any byte moves ---

    CHECK_THROWS(rhythm::apply(factory, {}), "no rhythm setting");
    CHECK_THROWS(rhythm::apply(factory, { .pattern = rc0::kRhythmPatternBlank }), "Blank");
    CHECK_THROWS(rhythm::apply(factory, { .pattern = 58 }), "PATTERN 58");
    CHECK_THROWS(rhythm::apply(factory, { .kit = 7 }), "KIT 7");
    CHECK_THROWS(rhythm::apply(factory, { .kit = -1 }), "KIT -1");
    CHECK_THROWS(rhythm::apply(factory, { .beat = 17 }), "BEAT 17");
    CHECK_THROWS(rhythm::apply(factory, { .variation = 2 }), "VARIATION 2");
    CHECK_THROWS(rhythm::apply(factory, { .level = 201 }), "LEVEL 201");
    CHECK_THROWS(rhythm::apply(factory, { .level = -1 }), "LEVEL -1");
    CHECK_THROWS(rhythm::apply(factory, { .reverb = 101 }), "REVERB 101");
    CHECK_THROWS(rhythm::apply(factory, { .toneLow = 11 }), "TONE LOW 11");
    CHECK_THROWS(rhythm::apply(factory, { .toneHigh = -11 }), "TONE HIGH -11");

    // One bad field spoils the whole edit: the good ones do not land first.
    {
        std::string after;
        CHECK_THROWS(after = rhythm::apply(factory, { .kit = 3, .level = 999 }), "LEVEL 999");
        CHECK(after.empty());
    }

    // --- BEAT is locked once a take is recorded (manual p. 10) ---

    {
        const std::string recorded = withTake(factory);
        CHECK(rhythm::read(recorded).beatLocked);
        CHECK_THROWS(rhythm::apply(recorded, { .beat = 6 }), "BEAT cannot be changed");
        // Everything else on the card is still the player's.
        const std::string after = rhythm::apply(recorded, { .on = true, .kit = 2, .level = 90 });
        CHECK(rhythm::isOn(after));
        CHECK_EQ(rhythm::read(after).kit, 2);
        CHECK(onlyTheseFieldsMoved(recorded, after, { "State", "Kit", "Level" }));
    }
    // A file present but not indexed (WavStat 2) is not a recorded take.
    {
        std::string unindexed = rc0::setField(factory, "WavStat", 2);
        CHECK(!rhythm::read(unindexed).beatLocked);
        CHECK_EQ(rhythm::read(rhythm::apply(unindexed, { .beat = 6 })).beat, 6);
    }
    // On a two-track memory a take on the SECOND track locks it too.
    {
        std::string two = factory;
        const auto at = two.find("<MASTER>");
        std::string track2 = factory.substr(factory.find("<TRACK1>"),
                                            factory.find("</TRACK1>") + 10 - factory.find("<TRACK1>"));
        track2.replace(track2.find("<TRACK1>"), 8, "<TRACK2>");
        track2.replace(track2.find("</TRACK1>"), 9, "</TRACK2>");
        two.insert(at, track2);
        CHECK(!rhythm::read(two).beatLocked);
        const std::string onSecond = rc0::setSectionField(two, "TRACK2", "WavStat", rc0::kWavStatIndexed);
        CHECK(rhythm::read(onSecond).beatLocked);
        CHECK_THROWS(rhythm::apply(onSecond, { .beat = 0 }), "BEAT cannot be changed");
    }

    // --- the section is the address, not the tag ---
    {
        const std::string other = "<OTHER>\n\t<State>1</State>\n\t<Pattern>5</Pattern>\n"
                                  "\t<Kit>3</Kit>\n\t<Level>7</Level>\n</OTHER>\n";
        std::string body = factory;
        body.insert(body.find("<RHYTHM>"), other);
        CHECK(!rhythm::isOn(body));
        CHECK_EQ(rhythm::read(body).kit, 0);
        const std::string after = rhythm::apply(body, { .on = true, .kit = 1, .level = 50 });
        CHECK(rhythm::isOn(after));
        CHECK_EQ(after.substr(after.find("<OTHER>"), other.size()), other);
        CHECK(rhythm::applySwitch(rhythm::apply(after, { .kit = 0, .level = 100 }), false) == body);
    }
    // A body without a RHYTHM section is refused by name, not read as silence.
    {
        const std::string noRhythm = factory.substr(0, factory.find("<RHYTHM>"));
        CHECK_THROWS(rhythm::read(noRhythm), "missing <RHYTHM>");
        CHECK_THROWS(rhythm::apply(noRhythm, { .on = true }), "missing <RHYTHM>");
    }

    // --- the words the history gets (at 4/4, the memory's own beat) ---

    CHECK_EQ(rhythm::describe({ .on = true }, kFourFour), "switched on");
    CHECK_EQ(rhythm::describe({ .on = false }, kFourFour), "switched off");
    CHECK_EQ(rhythm::describe({ .pattern = kSomeGroove, .kit = 2 }, kFourFour),
             "pattern Rock1, kit Jazz");
    CHECK_EQ(rhythm::describe({ .beat = 6, .variation = 1 }, kFourFour), "beat 5/8, variation B");
    CHECK_EQ(rhythm::describe({ .level = 120, .reverb = 0 }, kFourFour), "level 120, reverb 0");
    CHECK_EQ(rhythm::describe({ .toneLow = -3, .toneHigh = 2 }, kFourFour),
             "tone low -3, tone high +2");
    CHECK_EQ(rhythm::describe({ .toneHigh = 0 }, kFourFour), "tone high 0");
    CHECK_THROWS(rhythm::describe({ .pattern = 99 }, kFourFour), "PATTERN 99");

    // --- PATTERN is an index into the current BEAT's list; only 4/4 is charted ---

    // The hardware pairing (#147): Rock2 is stored as 12 at 4/4 and as 3 at
    // 6/4. 3 is inside the 4/4 list, where it reads SimpleBeat4, so the
    // range says nothing — only the beat does.
    CHECK_EQ(rhythm::beatName(kSixFour), "6/4");
    CHECK_EQ(rhythm::patternName(kRock2At44), "Rock2");
    CHECK_EQ(rhythm::kBeatFourFour, kFourFour);
    CHECK(rhythm::patternListCharted(kFourFour));
    for (const rhythm::Choice& b : rhythm::kBeats)
        CHECK_EQ(rhythm::patternListCharted(b.number), b.number == kFourFour);
    // A beat outside the list is not charted either — and not an error here:
    // the memory must still be readable. Naming is what fails (below).
    CHECK(!rhythm::patternListCharted(17));
    CHECK(!rhythm::patternListCharted(-1));

    // Reading a 6/4 memory: the number as the card holds it, and the word
    // that it cannot be named. No throw, whatever the number.
    {
        const rhythm::Values v = rhythm::read(sixFourRock2);
        CHECK_EQ(v.pattern, kRock2At64);
        CHECK_EQ(v.beat, kSixFour);
        CHECK(!v.patternCharted);
        CHECK(rhythm::read(factory).patternCharted);
        // The 19 the pedal could not name, and a number outside even the
        // 4/4 list: read, not named.
        CHECK_EQ(rhythm::read(atSixFour(factory, 19)).pattern, 19);
        CHECK(!rhythm::read(atSixFour(factory, 19)).patternCharted);
        CHECK_EQ(rhythm::read(atSixFour(factory, 99)).pattern, 99);
        CHECK(!rhythm::read(atSixFour(factory, 99)).patternCharted);
        // A 4/4 memory at the same numbers is charted as before.
        CHECK(rhythm::read(rc0::setSectionField(factory, rc0::kSectionRhythm, "Pattern", 3))
                  .patternCharted);
    }
    // The table's read model carries the same word, for the tab to act on.
    {
        std::string text = testkit::syntheticMemoryText();
        text = rc0::replaceSlotBody(text, 3, sixFourRock2);
        CHECK(!catalog::readSlot(text, 3).rhythm.patternCharted);
        CHECK_EQ(catalog::readSlot(text, 3).rhythm.pattern, kRock2At64);
        CHECK(catalog::readSlot(text, 4).rhythm.patternCharted);
    }

    // Writing a pattern at 6/4 is refused before any byte moves, naming the
    // beat as the screen prints it — even for the number the pedal itself
    // stored there, since it is OUR list the number would be checked against.
    {
        std::string after;
        CHECK_THROWS(after = rhythm::apply(sixFourRock2, { .pattern = kRock2At44 }),
                     "PATTERN cannot be chosen at 6/4");
        CHECK(after.empty());
        CHECK_THROWS(after = rhythm::apply(sixFourRock2, { .pattern = kRock2At64 }), "6/4");
        CHECK(after.empty());
        CHECK_THROWS(after = rhythm::apply(sixFourRock2, { .pattern = 0 }), "4/4 list");
        CHECK(after.empty());
        try {
            rhythm::apply(sixFourRock2, { .pattern = kRock2At44 });
            CHECK(false);
        } catch (const Error& e) {
            const std::string what = e.what();
            CHECK(what.find("6/4") != std::string::npos);
            CHECK(what.find("at 4") == std::string::npos); // the number is not the word
        }
        // One bad field spoils the whole edit here too: the good one does not land.
        CHECK_THROWS(after = rhythm::apply(sixFourRock2, { .pattern = kRock2At44, .kit = 3 }),
                     "6/4");
        CHECK(after.empty());
    }
    // Every other field of the card is still the player's at 6/4 (#149:
    // nothing suggests kit, level, reverb, tone or variation are
    // beat-relative — at 6/4 the pedal took Kit 6 and showed it). The
    // pattern bytes are reproduced exactly.
    {
        const std::string after = rhythm::apply(
            sixFourRock2, { .kit = 6, .variation = 1, .level = 90, .reverb = 40, .toneLow = -2,
                            .toneHigh = 3 });
        CHECK_EQ(rhythmField(after, "Pattern"), kRock2At64);
        CHECK_EQ(rhythm::read(after).kit, 6);
        CHECK(onlyTheseFieldsMoved(sixFourRock2, after,
                                   { "Kit", "Variation", "Level", "Reverb", "ToneLow",
                                     "ToneHigh" }));
    }

    // BEAT moves on its own; the pattern bytes stay exactly as they were.
    {
        const std::string fourFourRock2
            = rc0::setSectionField(factory, rc0::kSectionRhythm, "Pattern", kRock2At44);
        // 4/4 -> 6/4: allowed without a take, and 12 stays 12 — now a 6/4
        // number we cannot name, but the pedal's to resolve, not ours to guess.
        const std::string away = rhythm::apply(fourFourRock2, { .beat = kSixFour });
        CHECK_EQ(rhythm::read(away).beat, kSixFour);
        CHECK_EQ(rhythmField(away, "Pattern"), kRock2At44);
        CHECK(!rhythm::read(away).patternCharted);
        CHECK(onlyTheseFieldsMoved(fourFourRock2, away, { "Beat" }));
        // 6/4 -> 7/4: uncharted to uncharted, the pattern still untouched.
        const std::string onward = rhythm::apply(sixFourRock2, { .beat = 5 });
        CHECK_EQ(rhythmField(onward, "Pattern"), kRock2At64);
        CHECK(onlyTheseFieldsMoved(sixFourRock2, onward, { "Beat" }));
        // 6/4 -> 4/4: the number is a 4/4 number again, named from the list.
        const std::string back = rhythm::apply(sixFourRock2, { .beat = kFourFour });
        CHECK(rhythm::read(back).patternCharted);
        CHECK_EQ(rhythmField(back, "Pattern"), kRock2At64);
        CHECK(onlyTheseFieldsMoved(sixFourRock2, back, { "Beat" }));
    }
    // A pattern is checked against the beat the edit LEAVES the memory at.
    {
        // 6/4 -> 4/4 together with Rock2: allowed, checked against 4/4.
        const std::string after
            = rhythm::apply(sixFourRock2, { .pattern = kRock2At44, .beat = kFourFour });
        CHECK_EQ(rhythm::read(after).beat, kFourFour);
        CHECK_EQ(rhythmField(after, "Pattern"), kRock2At44);
        CHECK(rhythm::read(after).patternCharted);
        CHECK(onlyTheseFieldsMoved(sixFourRock2, after, { "Beat", "Pattern" }));
        // 4/4 -> 6/4 together with a pattern: refused, against the new beat.
        std::string refused;
        CHECK_THROWS(refused = rhythm::apply(factory, { .pattern = kRock2At44, .beat = kSixFour }),
                     "PATTERN cannot be chosen at 6/4");
        CHECK(refused.empty());
        // Under a take the move to 4/4 is the manual's refusal, and the
        // pattern does not land either way.
        CHECK_THROWS(refused = rhythm::apply(withTake(sixFourRock2),
                                             { .pattern = kRock2At44, .beat = kFourFour }),
                     "BEAT cannot be changed");
        CHECK(refused.empty());
    }
    // A beat outside the list is still the existing typed error, at any beat.
    CHECK_THROWS(rhythm::apply(sixFourRock2, { .beat = 17 }), "BEAT 17");
    CHECK_THROWS(rhythm::apply(sixFourRock2, { .beat = -1 }), "BEAT -1");

    // --- the switch at 6/4: State alone is ours, a pattern number is not ---

    // Blank's number and the default groove are 4/4 facts (57 and 0), so the
    // two paths that write them are refused; the two that move State alone
    // pass. The same through apply({.on}).
    {
        // Off over a groove with no count-in: State alone — passes.
        const std::string on = atSixFour(bodyWith(rc0::kRhythmStateOn, 0, kRock2At64), kRock2At64);
        const std::string off = rhythm::applySwitch(on, false);
        CHECK(!rhythm::isOn(off));
        CHECK_EQ(rhythmField(off, "Pattern"), kRock2At64);
        CHECK(onlyTheseFieldsMoved(on, off, { "State" }));
        CHECK(rhythm::apply(on, { .on = false }) == off);
        CHECK(!rhythm::switchRefusal(rhythm::read(on), false, false).has_value());
        // On over a chosen-but-silent groove: State alone — passes.
        const std::string silent = atSixFour(bodyWith(0, 0, kRock2At64), kRock2At64);
        const std::string loud = rhythm::applySwitch(silent, true);
        CHECK(rhythm::isOn(loud));
        CHECK_EQ(rhythmField(loud, "Pattern"), kRock2At64);
        CHECK(onlyTheseFieldsMoved(silent, loud, { "State" }));
        CHECK(rhythm::apply(silent, { .on = true }) == loud);
        // The 19 the pedal could not name is still a groove to switch on
        // under: State alone, the number untouched.
        const std::string nineteen = atSixFour(bodyWith(0, 0, 19), 19);
        CHECK_EQ(rhythmField(rhythm::applySwitch(nineteen, true), "Pattern"), 19);
        // Already there is already there, at any beat: no throw, nothing moves.
        CHECK(rhythm::applySwitch(on, true) == on);
        CHECK(rhythm::applySwitch(silent, false) == silent);
    }
    {
        // On over Blank: the default groove would be written — refused, with
        // the beat as the screen prints it and what the player should do.
        const std::string blankOff = atSixFour(bodyWith(0, 0, rc0::kRhythmPatternBlank),
                                               rc0::kRhythmPatternBlank);
        std::string after;
        CHECK_THROWS(after = rhythm::applySwitch(blankOff, true), "6/4");
        CHECK(after.empty());
        CHECK_THROWS(after = rhythm::applySwitch(blankOff, true), "4/4 list");
        CHECK_THROWS(after = rhythm::applySwitch(blankOff, true), "on the pedal");
        CHECK_THROWS(after = rhythm::apply(blankOff, { .on = true }), "6/4");
        CHECK(after.empty());
        CHECK(rhythm::switchRefusal(rhythm::read(blankOff), false, true)
              == rhythm::SwitchRefusal::onNeedsGroove);
        // The typed refusal and its sentence are one fact, told two ways.
        CHECK_THROWS(rhythm::applySwitch(blankOff, true),
                     rhythm::switchRefusalText(rhythm::SwitchRefusal::onNeedsGroove, kSixFour));
        // The count-in's silence (State on, Blank) is the same refusal on the
        // way on, and nothing at all on the way off: it is already off.
        const std::string countSilence
            = atSixFour(bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas,
                                 rc0::kRhythmPatternBlank),
                        rc0::kRhythmPatternBlank);
        CHECK_THROWS(after = rhythm::applySwitch(countSilence, true), "6/4");
        CHECK(after.empty());
        CHECK(rhythm::applySwitch(countSilence, false) == countSilence);
        // Off with the count-in on: Blank would be written — refused.
        const std::string counted = atSixFour(
            bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, kRock2At64), kRock2At64);
        CHECK(rhythm::isOn(counted));
        CHECK(usecases::countin::isOn(counted));
        CHECK_THROWS(after = rhythm::applySwitch(counted, false), "6/4");
        CHECK(after.empty());
        CHECK_THROWS(after = rhythm::applySwitch(counted, false), "count-in");
        CHECK_THROWS(after = rhythm::apply(counted, { .on = false }), "6/4");
        CHECK(after.empty());
        CHECK(rhythm::switchRefusal(rhythm::read(counted), true, false)
              == rhythm::SwitchRefusal::offNeedsBlank);
        CHECK_THROWS(rhythm::applySwitch(counted, false),
                     rhythm::switchRefusalText(rhythm::SwitchRefusal::offNeedsBlank, kSixFour));
        // The same memory with the count-in off is State alone again.
        CHECK(!rhythm::switchRefusal(rhythm::read(counted), false, false).has_value());
        // Together with a move to 4/4 the numbers are 4/4 numbers: both pass.
        const std::string onAtFourFour = rhythm::apply(blankOff, { .on = true, .beat = kFourFour });
        CHECK(rhythm::isOn(onAtFourFour));
        CHECK_EQ(rhythm::read(onAtFourFour).beat, kFourFour);
        CHECK_EQ(rhythmField(onAtFourFour, "Pattern"), rhythm::kPatternDefault);
        const std::string offAtFourFour = rhythm::apply(counted, { .on = false, .beat = kFourFour });
        CHECK(!rhythm::isOn(offAtFourFour));
        CHECK(usecases::countin::isOn(offAtFourFour));
        CHECK_EQ(rhythmField(offAtFourFour, "Pattern"), rc0::kRhythmPatternBlank);
        // And a move away from 4/4 together with the switch is refused the
        // same way, against the new beat.
        CHECK_THROWS(after = rhythm::apply(bodyWith(0, 0, rc0::kRhythmPatternBlank),
                                           { .on = true, .beat = kSixFour }),
                     "6/4");
        CHECK(after.empty());
    }
    // At 4/4 the switch never has a refusal to give, whatever it holds.
    CHECK(!rhythm::switchRefusal(rhythm::read(factory), false, true).has_value());
    CHECK(!rhythm::switchRefusal(rhythm::read(bodyWith(0, 0, rc0::kRhythmPatternBlank)), false,
                                 true)
               .has_value());
    CHECK(!rhythm::switchRefusal(rhythm::read(bodyWith(rc0::kRhythmStateOn,
                                                       rc0::kRhythmPlayCount1Meas, kSomeGroove)),
                                 true, false)
               .has_value());

    // --- the words at 6/4: a number is never given a 4/4 name ---

    CHECK_THROWS(rhythm::describe({ .pattern = kRock2At44 }, kSixFour), "6/4");
    CHECK_THROWS(rhythm::describe({ .pattern = kRock2At64 }, kSixFour),
                 "PATTERN cannot be chosen at 6/4");
    CHECK_THROWS(rhythm::describe({ .pattern = kRock2At44, .beat = kSixFour }, kFourFour), "6/4");
    CHECK_EQ(rhythm::describe({ .pattern = kRock2At44, .beat = kFourFour }, kSixFour),
             "pattern Rock2, beat 4/4");
    CHECK_EQ(rhythm::describe({ .beat = kSixFour }, kSixFour), "beat 6/4");
    CHECK_EQ(rhythm::describe({ .on = true, .kit = 2 }, kSixFour), "switched on, kit Jazz");

    // --- a beat the manual's list lacks: read and labelled, never a throw on a reading ---
    //
    // kBeats is inferred past its one anchor, so a memory can hold a number
    // the list lacks. Reading and labelling it must not throw — a tab paints
    // on every snapshot — while an action on BEAT itself still fails loudly,
    // and a pattern number is as refused there as at 6/4.
    CHECK_EQ(usecases::beat::label(kSixFour), "6/4");
    CHECK_EQ(*usecases::beat::nameIfListed(kFourFour), "4/4");
    for (const long long odd : { -1LL, 17LL, 99LL }) {
        const std::string number = std::to_string(odd);
        const std::string body = atBeat(bodyWith(0, 0, kRock2At64), odd);
        const rhythm::Values v = rhythm::read(body);
        CHECK_EQ(v.beat, odd);
        CHECK(!v.patternCharted);
        CHECK_EQ(v.pattern, kRock2At64);
        CHECK(!usecases::beat::nameIfListed(odd).has_value());
        CHECK_EQ(usecases::beat::label(odd), "BEAT " + number + " (not in the manual's list)");
        CHECK_THROWS(rhythm::beatName(odd), "BEAT " + number);
        // A State-only switch passes: no pattern number is needed for it.
        CHECK(!rhythm::switchRefusal(v, false, true).has_value());
        const std::string on = rhythm::applySwitch(body, true);
        CHECK(onlyTheseFieldsMoved(body, on, { "State" }));
        CHECK(rhythm::applySwitch(on, false) == body);
        CHECK(rhythm::apply(body, { .on = true }) == on);
        // The two pattern-writing clicks are refused, with the number told as
        // the truth it is rather than a name the list does not have.
        const std::string blank = atBeat(bodyWith(0, 0, rc0::kRhythmPatternBlank), odd);
        CHECK(rhythm::switchRefusal(rhythm::read(blank), false, true)
              == rhythm::SwitchRefusal::onNeedsGroove);
        CHECK_THROWS(rhythm::applySwitch(blank, true),
                     "at BEAT " + number + " (not in the manual's list) needs");
        // A pattern edit is refused, before any byte moves; the rest of the
        // card is still the player's, and so is the way out: BEAT itself.
        std::string after;
        CHECK_THROWS(after = rhythm::apply(body, { .pattern = kRock2At44 }),
                     "PATTERN cannot be chosen at BEAT " + number);
        CHECK(after.empty());
        CHECK_THROWS(rhythm::describe({ .pattern = kRock2At44 }, odd), "BEAT " + number);
        CHECK(onlyTheseFieldsMoved(body, rhythm::apply(body, { .kit = 2 }), { "Kit" }));
        CHECK_EQ(rhythm::read(rhythm::apply(body, { .beat = kFourFour })).beat, kFourFour);
        CHECK(rhythm::read(rhythm::apply(body, { .beat = kFourFour })).patternCharted);
    }

    // --- BEAT stays at 4/4 while the count-in borrows the rhythm section ---
    //
    // State on, Blank, count on is the count-in's own shape. Moved away from
    // 4/4 it is stuck in the app: the hand-back and the rhythm's switch would
    // both need a pattern number there, and BEAT itself would be the only
    // way back. So that one move is refused before any byte moves; every
    // other BEAT move keeps the pattern bytes as it did, and a move TO 4/4
    // is never refused.
    {
        const std::string borrowed = bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas,
                                              rc0::kRhythmPatternBlank);
        CHECK(rhythm::read(borrowed).beatHeldByCountIn);
        std::string after;
        CHECK_THROWS(after = rhythm::apply(borrowed, { .beat = kSixFour }), "BEAT cannot leave 4/4");
        CHECK(after.empty());
        CHECK_THROWS(after = rhythm::apply(borrowed, { .beat = 16 }), "switch the count-in off first");
        CHECK(after.empty());
        CHECK_THROWS(after = rhythm::apply(borrowed, { .kit = 3, .beat = kSixFour }),
                     "BEAT cannot leave 4/4");
        CHECK(after.empty());
        // The same beat is not a move, and the rest of the card is still the player's.
        CHECK(rhythm::apply(borrowed, { .beat = kFourFour }) == borrowed);
        CHECK(onlyTheseFieldsMoved(borrowed, rhythm::apply(borrowed, { .kit = 3 }), { "Kit" }));
        // The way out is the count-in's own switch: off, and BEAT moves again.
        const std::string released = usecases::countin::apply(borrowed, false);
        CHECK(!rhythm::read(released).beatHeldByCountIn);
        CHECK(onlyTheseFieldsMoved(released, rhythm::apply(released, { .beat = kSixFour }),
                                   { "Beat" }));
        // Not held: a count over a groove — 4/4 -> 6/4 passes and 12 stays 12;
        // a rhythm on with Blank but no count; the factory slot.
        const std::string counted
            = bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, kRock2At44);
        CHECK(!rhythm::read(counted).beatHeldByCountIn);
        const std::string moved = rhythm::apply(counted, { .beat = kSixFour });
        CHECK_EQ(rhythmField(moved, "Pattern"), kRock2At44);
        CHECK(onlyTheseFieldsMoved(counted, moved, { "Beat" }));
        CHECK(!rhythm::read(bodyWith(rc0::kRhythmStateOn, 0, rc0::kRhythmPatternBlank))
                   .beatHeldByCountIn);
        CHECK(!rhythm::read(factory).beatHeldByCountIn);
        // A memory already stuck at 6/4 is not "held" — the hold is a 4/4
        // fact — and BEAT back to 4/4 is never refused; nor is a move between
        // two uncharted beats, which changes nothing about it.
        const std::string stuck = atSixFour(borrowed, rc0::kRhythmPatternBlank);
        CHECK(!rhythm::read(stuck).beatHeldByCountIn);
        const std::string home = rhythm::apply(stuck, { .beat = kFourFour });
        CHECK(rhythm::read(home).beatHeldByCountIn);
        CHECK(onlyTheseFieldsMoved(stuck, home, { "Beat" }));
        CHECK(onlyTheseFieldsMoved(stuck, rhythm::apply(stuck, { .beat = 5 }), { "Beat" }));
        // Under a take the manual's refusal comes first.
        CHECK_THROWS(rhythm::apply(withTake(borrowed), { .beat = kSixFour }),
                     "BEAT cannot be changed");
    }

    return testkit::summary("usecase_rhythm_tests");
}
