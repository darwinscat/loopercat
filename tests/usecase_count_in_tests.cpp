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

#include "support.hpp"

#include <loopercat/Catalog.hpp>
#include <loopercat/usecases/CountIn.hpp>

using namespace loopercat;

namespace
{
    constexpr long long kSomeGroove = 11; // any pattern that is neither Blank nor the factory 0

    std::string bodyWith(long long state, long long playCount, long long pattern)
    {
        std::string body = testkit::syntheticSlotBody();
        body = rc0::setField(body, "State", state);
        body = rc0::setField(body, "PlayCount", playCount);
        body = rc0::setField(body, "Pattern", pattern);
        return body;
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

    return testkit::summary("usecase_count_in_tests");
}
