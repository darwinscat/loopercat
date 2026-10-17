// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Play Count-In as a feature, tested from what the PLAYER hears, not from
// the fields the implementation happens to write. The theory, from the RC-5
// itself (hardware 2026-08-10):
//
//   1. A count is heard only when the rhythm section is on — PlayCount alone
//      reaches nobody's ears.
//   2. A count is heard whatever the pattern is: a groove and a count-in
//      coexist on the pedal.
//   3. A groove the player set on the pedal is the player's. Switching the
//      count on or off must not take it away, and its presence must not hide
//      the count from the table.
//   4. The feature keeps no hidden state: a slot that only ever had a
//      count-in returns to the exact bytes it started with.
//   5. A PATTERN number is a number in the list of the memory's CURRENT
//      BEAT, and only the 4/4 list is charted (hardware 2026-10-01, #147):
//      at any other beat the feature writes no pattern number — the two
//      paths that would (the borrow on the way on, the hand-back on the way
//      off) are refused before a byte moves, naming the beat as the screen
//      prints it — while the count alone, PlayCount over a playing rhythm,
//      is still the player's.

#include "support.hpp"

#include <loopercat/Catalog.hpp>
#include <loopercat/usecases/Beat.hpp>
#include <loopercat/usecases/CountIn.hpp>

#include <vector>

using namespace loopercat;

namespace
{
    constexpr long long kSomeGroove = 11; // any pattern that is neither Blank nor the factory 0

    // The hardware pairing of #147: Beat 2 reads 4/4, Beat 4 reads 6/4, and
    // Rock2 chosen on the pedal is stored as 3 at 6/4 (12 at 4/4).
    constexpr long long kFourFour = 2;
    constexpr long long kSixFour = 4;
    constexpr long long kRock2At64 = 3;

    std::string bodyWith(long long state, long long playCount, long long pattern)
    {
        std::string body = testkit::syntheticSlotBody();
        body = rc0::setField(body, "State", state);
        body = rc0::setField(body, "PlayCount", playCount);
        body = rc0::setField(body, "Pattern", pattern);
        return body;
    }

    std::string atBeat(std::string body, long long beat)
    {
        return rc0::setSectionField(body, rc0::kSectionRhythm, "Beat", beat);
    }

    // Byte proof that nothing outside the named fields moved: restore them to
    // the values the original carried, and the two bodies must be identical.
    bool onlyTheseFieldsMoved(const std::string& before, std::string after,
                              std::initializer_list<const char*> fields)
    {
        for (const char* tag : fields)
            after = rc0::setField(after, tag, rc0::field(before, tag));
        return after == before;
    }

    // One section's bytes, opener to closer — the unit of "untouched".
    std::string section(const std::string& body, const std::string& tag)
    {
        const auto open = body.find("<" + tag + ">");
        const auto close = body.find("</" + tag + ">", open);
        if (open == std::string::npos || close == std::string::npos)
            throw loopercat::Error("test fixture: missing section <" + tag + ">");
        return body.substr(open, close - open + tag.size() + 3);
    }
}

int main()
{
    // --- what the player hears (reading) ---

    // Factory: rhythm off, no count.
    CHECK(!usecases::countin::isOn(testkit::syntheticSlotBody()));

    // The count-in as this app writes it over a silent rhythm.
    CHECK(usecases::countin::isOn(
        bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, rc0::kRhythmPatternBlank)));

    // A count in front of a groove: the pedal plays both, so the app says on.
    // (The old strict-AND read called this "no count-in" and lied.)
    CHECK(usecases::countin::isOn(
        bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, kSomeGroove)));

    // PlayCount set while the rhythm section is off: nothing is heard.
    CHECK(!usecases::countin::isOn(bodyWith(0, rc0::kRhythmPlayCount1Meas, kSomeGroove)));
    CHECK(!usecases::countin::isOn(bodyWith(0, rc0::kRhythmPlayCount1Meas,
                                            rc0::kRhythmPatternBlank)));

    // A groove playing without a count is not a count-in.
    CHECK(!usecases::countin::isOn(bodyWith(rc0::kRhythmStateOn, 0, kSomeGroove)));

    // The table's indicator is this same question, through the read model.
    {
        std::string text = testkit::syntheticMemoryText();
        text = rc0::replaceSlotBody(
            text, 4, bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, kSomeGroove));
        CHECK(catalog::readSlot(text, 4).countIn);
    }

    // --- switching it on ---

    // Over a silent rhythm the feature borrows the section: count, then
    // silence. Exactly three fields move.
    {
        const std::string before = testkit::syntheticSlotBody();
        const std::string after = usecases::countin::apply(before, true);
        CHECK(usecases::countin::isOn(after));
        CHECK_EQ(rc0::field(after, "State"), rc0::kRhythmStateOn);
        CHECK_EQ(rc0::field(after, "PlayCount"), rc0::kRhythmPlayCount1Meas);
        CHECK_EQ(rc0::field(after, "Pattern"), rc0::kRhythmPatternBlank);
        CHECK(onlyTheseFieldsMoved(before, after, { "State", "PlayCount", "Pattern" }));
    }

    // Over a groove the player is already using, the count is ALL that moves:
    // the groove keeps playing, the kit and the rest stay untouched.
    {
        std::string before = bodyWith(rc0::kRhythmStateOn, 0, kSomeGroove);
        before = rc0::setField(before, "Kit", 3);
        const std::string after = usecases::countin::apply(before, true);
        CHECK(usecases::countin::isOn(after));
        CHECK_EQ(rc0::field(after, "Pattern"), kSomeGroove);
        CHECK_EQ(rc0::field(after, "Kit"), 3);
        CHECK(onlyTheseFieldsMoved(before, after, { "PlayCount" }));
    }

    // Turning it on twice is turning it on.
    {
        const std::string once = usecases::countin::apply(testkit::syntheticSlotBody(), true);
        CHECK(usecases::countin::apply(once, true) == once);
    }

    // --- switching it off ---

    // What this app turned on, it gives back byte for byte — no hidden state,
    // no memory of anything.
    {
        const std::string factory = testkit::syntheticSlotBody();
        const std::string roundTrip
            = usecases::countin::apply(usecases::countin::apply(factory, true), false);
        CHECK(roundTrip == factory);
    }

    // Off over a groove clears the count and NOTHING else — the groove was
    // never ours to switch off.
    {
        const std::string before
            = bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, kSomeGroove);
        const std::string after = usecases::countin::apply(before, false);
        CHECK(!usecases::countin::isOn(after));
        CHECK_EQ(rc0::field(after, "State"), rc0::kRhythmStateOn);
        CHECK_EQ(rc0::field(after, "Pattern"), kSomeGroove);
        CHECK(onlyTheseFieldsMoved(before, after, { "PlayCount" }));
    }

    // A rhythm the player switched on WITHOUT a count (even a silent one)
    // must survive "count-in off": we only hand back what we borrowed.
    {
        const std::string before = bodyWith(rc0::kRhythmStateOn, 0, rc0::kRhythmPatternBlank);
        CHECK(usecases::countin::apply(before, false) == before);
    }

    // Off on a factory slot changes nothing at all.
    {
        const std::string factory = testkit::syntheticSlotBody();
        CHECK(usecases::countin::apply(factory, false) == factory);
    }

    // --- the one thing worth warning about ---

    // A groove chosen but not playing: switching the count on replaces it,
    // so the UI has something honest to say before the click.
    {
        const auto risk = usecases::countin::patternAtRisk(bodyWith(0, 0, kSomeGroove));
        CHECK(risk.has_value());
        CHECK_EQ(*risk, kSomeGroove);
    }
    // The factory pattern is not a choice anyone made — no crying wolf.
    CHECK(!usecases::countin::patternAtRisk(testkit::syntheticSlotBody()).has_value());
    // Blank is our own silence, not a groove.
    CHECK(!usecases::countin::patternAtRisk(bodyWith(0, 0, rc0::kRhythmPatternBlank)).has_value());
    // A playing groove is never at risk: we do not touch Pattern at all there.
    CHECK(!usecases::countin::patternAtRisk(bodyWith(rc0::kRhythmStateOn, 0, kSomeGroove))
               .has_value());

    // The count-in is RHYTHM's. A tag of the same name in another section of
    // the memory neither reads as the rhythm's state nor is written by the
    // feature — the section is the address, not the tag.
    {
        const std::string other = "<OTHER>\n\t<State>1</State>\n\t<PlayCount>1</PlayCount>\n"
                                  "\t<Pattern>5</Pattern>\n</OTHER>\n";
        std::string body = bodyWith(0, 0, 0);
        body.insert(body.find("<RHYTHM>"), other);
        CHECK(!usecases::countin::isOn(body));
        CHECK(!usecases::countin::patternAtRisk(body).has_value());
        const std::string on = usecases::countin::apply(body, true);
        CHECK(usecases::countin::isOn(on));
        CHECK_EQ(section(on, "OTHER"), section(body, "OTHER"));
        CHECK(usecases::countin::apply(on, false) == body);
    }

    // --- a PATTERN number is a 4/4 number: the count-in writes none elsewhere ---
    //
    // The P0 of #149 as it was found: the pedal's own Rock2 at 6/4 is stored
    // as 3, and "count-in on" over it wrote 57. The rule, from the spec: on
    // writes Pattern exactly when the section is off (the borrow); off
    // writes it exactly when the section was borrowed for the count (the
    // hand-back). Both are refused at every uncharted beat, before any byte
    // moves; everything else moves PlayCount alone and passes. Every beat
    // but 4/4, both toggles, every section state.
    {
        namespace countin = usecases::countin;
        struct Section {
            long long state, playCount, pattern;
        };
        const Section sections[] = {
            { 0, 0, kRock2At64 },                            // silent, the pedal's own groove
            { 0, 0, rc0::kRhythmPatternBlank },              // silent, 57
            { 0, 0, 0 },                                     // silent, factory 0
            { 0, rc0::kRhythmPlayCount1Meas, kRock2At64 },   // PlayCount set, section off
            { rc0::kRhythmStateOn, 0, kRock2At64 },          // playing, no count
            { rc0::kRhythmStateOn, 0, rc0::kRhythmPatternBlank },  // on with 57, no count
            { rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, kRock2At64 }, // count over a groove
            { rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, rc0::kRhythmPatternBlank }, // the app's own shape
        };
        std::vector<long long> uncharted;
        for (const usecases::Choice& b : usecases::beat::kBeats)
            if (b.number != kFourFour)
                uncharted.push_back(b.number);
        for (const long long odd : { -1LL, 17LL, 99LL }) // numbers the list lacks
            uncharted.push_back(odd);
        CHECK_EQ(uncharted.size(), 19u);

        for (const long long beat : uncharted) {
            for (const Section& s : sections) {
                const std::string before = atBeat(bodyWith(s.state, s.playCount, s.pattern), beat);
                for (const bool on : { true, false }) {
                    const bool writesPattern = on
                        ? s.state != rc0::kRhythmStateOn
                        : s.state == rc0::kRhythmStateOn && s.playCount == rc0::kRhythmPlayCount1Meas
                              && s.pattern == rc0::kRhythmPatternBlank;
                    const auto why = countin::refusal(before, on);
                    CHECK_EQ(why.has_value(), writesPattern);
                    if (writesPattern) {
                        CHECK(why == (on ? countin::Refusal::onBorrowsSection
                                         : countin::Refusal::offReturnsSection));
                        std::string after;
                        CHECK_THROWS(after = countin::apply(before, on), "4/4 list");
                        CHECK(after.empty());
                    } else {
                        const std::string after = countin::apply(before, on);
                        CHECK(onlyTheseFieldsMoved(before, after, { "PlayCount" }));
                        CHECK_EQ(rc0::field(after, "Pattern"), s.pattern);
                        CHECK_EQ(rc0::field(after, "State"), s.state);
                        CHECK_EQ(rc0::field(after, "PlayCount"),
                                 on ? rc0::kRhythmPlayCount1Meas : 0);
                    }
                }
            }
        }

        // The sentence names the beat as the screen prints it, not the number;
        // says what the click would have written and what to do instead; and
        // for a number the list lacks, says that.
        {
            const std::string silent = atBeat(bodyWith(0, 0, kRock2At64), kSixFour);
            std::string after;
            CHECK_THROWS(after = countin::apply(silent, true), "at 6/4");
            CHECK_THROWS(after = countin::apply(silent, true), "Blank");
            CHECK_THROWS(after = countin::apply(silent, true), "switch the rhythm on first");
            CHECK(after.empty());
            try {
                countin::apply(silent, true);
                CHECK(false);
            } catch (const Error& e) {
                CHECK(std::string(e.what()).find("at 4") == std::string::npos);
            }
            const std::string borrowed
                = atBeat(bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas,
                                  rc0::kRhythmPatternBlank),
                         kSixFour);
            CHECK_THROWS(after = countin::apply(borrowed, false), "at 6/4");
            CHECK_THROWS(after = countin::apply(borrowed, false), "on the pedal");
            CHECK(after.empty());
            CHECK_THROWS(countin::apply(atBeat(bodyWith(0, 0, kRock2At64), 17), true),
                         "at BEAT 17 (not in the manual's list) would");
            CHECK_THROWS(after = countin::apply(borrowed, false),
                         "switch the count-in off on the pedal instead");
            CHECK_THROWS(countin::apply(atBeat(bodyWith(0, 0, kRock2At64), -1), true), "BEAT -1");
        }

        // At 4/4 nothing is refused, whatever the section holds: the rule is
        // the beat's, not the pattern's.
        for (const Section& s : sections)
            for (const bool on : { true, false })
                CHECK(!countin::refusal(bodyWith(s.state, s.playCount, s.pattern), on).has_value());

        // Nothing is "at risk" at an uncharted beat: the switch that would
        // replace the pattern is refused there, and the refusal is what the
        // card says instead of the warning. The same section at 4/4 warns.
        CHECK(!countin::patternAtRisk(atBeat(bodyWith(0, 0, kRock2At64), kSixFour)).has_value());
        CHECK(!countin::patternAtRisk(atBeat(bodyWith(0, 0, 19), kSixFour)).has_value());
        CHECK(!countin::patternAtRisk(atBeat(bodyWith(0, 0, kSomeGroove), 17)).has_value());
        CHECK(countin::patternAtRisk(bodyWith(0, 0, kRock2At64)).has_value());

        // The read model carries the refusal of the NEXT click — on where the
        // count is off, off where it is on — and nothing where that click is
        // the player's.
        {
            std::string text = testkit::syntheticMemoryText();
            const auto put = [&text](int slot, const std::string& body) {
                text = rc0::replaceSlotBody(text, slot, body);
            };
            put(5, atBeat(bodyWith(0, 0, kRock2At64), kSixFour));                  // silent: on refused
            put(6, atBeat(bodyWith(rc0::kRhythmStateOn, 0, kRock2At64), kSixFour)); // playing: on passes
            put(7, atBeat(bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas,
                                   rc0::kRhythmPatternBlank),
                          kSixFour));                                              // borrowed: off refused
            put(8, atBeat(bodyWith(rc0::kRhythmStateOn, rc0::kRhythmPlayCount1Meas, kRock2At64),
                          kSixFour));                                              // count over a groove: off passes
            put(9, bodyWith(0, 0, kRock2At64));                                    // silent at 4/4: the warning
            CHECK(catalog::readSlot(text, 5).countInRefused == countin::Refusal::onBorrowsSection);
            CHECK(!catalog::readSlot(text, 5).countInTakesPattern);
            CHECK(!catalog::readSlot(text, 5).countIn);
            CHECK(!catalog::readSlot(text, 6).countInRefused.has_value());
            CHECK(catalog::readSlot(text, 7).countInRefused == countin::Refusal::offReturnsSection);
            CHECK(catalog::readSlot(text, 7).countIn);
            CHECK(!catalog::readSlot(text, 8).countInRefused.has_value());
            CHECK(catalog::readSlot(text, 8).countIn);
            CHECK(!catalog::readSlot(text, 9).countInRefused.has_value());
            CHECK(catalog::readSlot(text, 9).countInTakesPattern);
            CHECK(!catalog::readSlot(text, 1).countInRefused.has_value());
        }
    }

    return testkit::summary("usecase_count_in_tests");
}
