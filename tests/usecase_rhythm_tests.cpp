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

#include "support.hpp"

#include <loopercat/Catalog.hpp>
#include <loopercat/usecases/CountIn.hpp>
#include <loopercat/usecases/Rhythm.hpp>

#include <string>

using namespace loopercat;
namespace rhythm = loopercat::usecases::rhythm;

namespace
{
    constexpr long long kSomeGroove = 11; // Rock1 in the manual's order: neither Blank nor factory 0

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
}

int main()
{
    const std::string factory = testkit::syntheticSlotBody();

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

    // --- the words the history gets ---

    CHECK_EQ(rhythm::describe({ .on = true }), "switched on");
    CHECK_EQ(rhythm::describe({ .on = false }), "switched off");
    CHECK_EQ(rhythm::describe({ .pattern = kSomeGroove, .kit = 2 }), "pattern Rock1, kit Jazz");
    CHECK_EQ(rhythm::describe({ .beat = 6, .variation = 1 }), "beat 5/8, variation B");
    CHECK_EQ(rhythm::describe({ .level = 120, .reverb = 0 }), "level 120, reverb 0");
    CHECK_EQ(rhythm::describe({ .toneLow = -3, .toneHigh = 2 }), "tone low -3, tone high +2");
    CHECK_EQ(rhythm::describe({ .toneHigh = 0 }), "tone high 0");
    CHECK_THROWS(rhythm::describe({ .pattern = 99 }), "PATTERN 99");

    return testkit::summary("usecase_rhythm_tests");
}
