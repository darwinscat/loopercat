// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Start & Stop as a feature, tested from the manual's page (p. 9) and the
// factory body, not from the fields the implementation writes. The theory:
//
//   1. START is IMMEDIATE or FADE IN; STOP is IMMEDIATE, FADE OUT or LOOP
//      END; FADE TIME is four note lengths then 1MEAS..64MEAS — 68 values,
//      2MEAS the printed default and the factory 5.
//   2. FADE TIME is heard only when a fade is set, on either end.
//   3. Each field is its section's: START/STOP are the track's, FADE TIME
//      the memory's; a change to one moves that field's bytes and no other.
//   4. A number outside a list is a typed error carrying the number, and a
//      refused edit leaves the body untouched — even beside good fields.
//   5. Nothing of the loop's length or modes is touched, ever.

#include "support.hpp"

#include <loopercat/Catalog.hpp>
#include <loopercat/usecases/PlayStop.hpp>

#include <string>

using namespace loopercat;
namespace playstop = loopercat::usecases::playstop;

namespace
{
    // Byte proof that nothing outside the named (section, tag) pairs moved.
    bool onlyTheseFieldsMoved(const std::string& before, std::string after,
                              std::initializer_list<std::pair<const char*, const char*>> fields)
    {
        for (const auto& [section, tag] : fields)
            after = rc0::setSectionField(after, section, tag,
                                         rc0::sectionField(before, section, tag));
        return after == before;
    }

    long long track(const std::string& body, const char* tag)
    {
        return rc0::sectionField(body, rc0::kSectionTrack1, tag);
    }
}

int main()
{
    const std::string factory = testkit::syntheticSlotBody();

    // --- the lists are the manual's, counted from zero ---

    CHECK_EQ(playstop::startModeName(0), "IMMEDIATE");
    CHECK_EQ(playstop::startModeName(1), "FADE IN");
    CHECK_EQ(playstop::stopModeName(0), "IMMEDIATE");
    CHECK_EQ(playstop::stopModeName(1), "FADE OUT");
    CHECK_EQ(playstop::stopModeName(2), "LOOP END");
    CHECK_EQ(playstop::kStartFadeIn, 1);
    CHECK_EQ(playstop::kStopFadeOut, 1);

    // FADE TIME: the four notes, then measures counted from 1MEAS at 4 —
    // the factory 5 is the printed default 2MEAS, the last is 64MEAS at 67.
    CHECK_EQ(playstop::kFadeTimeCount, 68);
    CHECK_EQ(playstop::fadeTimeName(0), "1/16 note");
    CHECK_EQ(playstop::fadeTimeName(3), "1/2 note");
    CHECK_EQ(playstop::fadeTimeName(4), "1MEAS");
    CHECK_EQ(playstop::fadeTimeName(5), "2MEAS");
    CHECK_EQ(playstop::fadeTimeName(67), "64MEAS");
    // Every value has a name and no two share one.
    {
        bool distinct = true;
        for (long long i = 0; i < playstop::kFadeTimeCount && distinct; ++i)
            for (long long j = 0; j < i; ++j)
                if (playstop::fadeTimeName(i) == playstop::fadeTimeName(j))
                    distinct = false;
        CHECK(distinct);
    }

    // A number outside a list is a typed error that carries the number.
    CHECK_THROWS(playstop::startModeName(2), "START 2");
    CHECK_THROWS(playstop::startModeName(-1), "START -1");
    CHECK_THROWS(playstop::stopModeName(3), "STOP 3");
    CHECK_THROWS(playstop::fadeTimeName(68), "FADE TIME 68");
    CHECK_THROWS(playstop::fadeTimeName(-1), "FADE TIME -1");

    // --- reading ---

    // Factory: on the spot both ways, 2MEAS set but unheard.
    {
        const playstop::Values v = playstop::read(factory);
        CHECK_EQ(v.startMode, 0);
        CHECK_EQ(v.stopMode, 0);
        CHECK_EQ(v.fadeTime, 5);
        CHECK(!v.fadeInUse);
    }
    // A fade on either end makes FADE TIME heard; LOOP END is not a fade.
    CHECK(playstop::fadeInUse(playstop::kStartFadeIn, 0));
    CHECK(playstop::fadeInUse(0, playstop::kStopFadeOut));
    CHECK(!playstop::fadeInUse(0, 2));
    CHECK(playstop::read(playstop::apply(factory, { .stopMode = 1 })).fadeInUse);
    CHECK(!playstop::read(playstop::apply(factory, { .stopMode = 2 })).fadeInUse);

    // The table's read model carries the same values.
    {
        std::string text = testkit::syntheticMemoryText();
        text = rc0::replaceSlotBody(
            text, 4, playstop::apply(rc0::slotBody(text, 4), { .startMode = 1, .fadeTime = 8 }));
        const catalog::SlotInfo info = catalog::readSlot(text, 4);
        CHECK_EQ(info.playStop.startMode, 1);
        CHECK_EQ(info.playStop.fadeTime, 8);
        CHECK(info.playStop.fadeInUse);
        CHECK(!catalog::readSlot(text, 5).playStop.fadeInUse);
    }

    // --- edits: each field alone, in its own section, and nothing else ---

    {
        const std::string after = playstop::apply(factory, { .startMode = 1 });
        CHECK_EQ(track(after, "StrtMod"), 1);
        CHECK(onlyTheseFieldsMoved(factory, after, { { "TRACK1", "StrtMod" } }));
    }
    {
        const std::string after = playstop::apply(factory, { .stopMode = 2 });
        CHECK_EQ(track(after, "StpMod"), 2);
        CHECK(onlyTheseFieldsMoved(factory, after, { { "TRACK1", "StpMod" } }));
    }
    {
        const std::string after = playstop::apply(factory, { .fadeTime = 67 });
        CHECK_EQ(rc0::sectionField(after, rc0::kSectionMaster, "FadeTime"), 67);
        CHECK(onlyTheseFieldsMoved(factory, after, { { "MASTER", "FadeTime" } }));
    }
    // All three at once, edges included.
    {
        const std::string after
            = playstop::apply(factory, { .startMode = 1, .stopMode = 2, .fadeTime = 0 });
        const playstop::Values v = playstop::read(after);
        CHECK_EQ(v.startMode, 1);
        CHECK_EQ(v.stopMode, 2);
        CHECK_EQ(v.fadeTime, 0);
        CHECK(onlyTheseFieldsMoved(
            factory, after, { { "TRACK1", "StrtMod" }, { "TRACK1", "StpMod" }, { "MASTER", "FadeTime" } }));
    }
    // Writing what the slot already has is byte-identical output.
    {
        const playstop::Values v = playstop::read(factory);
        CHECK(playstop::apply(factory, { .startMode = v.startMode, .stopMode = v.stopMode,
                                         .fadeTime = v.fadeTime })
              == factory);
    }
    // The loop's length and modes never move: Measure, MeasLen, LpLen,
    // LpMod, TrkMod, Sync are exactly what they were, whatever the edit.
    {
        std::string before = factory;
        before = rc0::setSectionField(before, "TRACK1", "MeasLen", 8);
        before = rc0::setSectionField(before, "TRACK1", "Measure", 15);
        before = rc0::setSectionField(before, "MASTER", "LpLen", 3);
        before = rc0::setSectionField(before, "MASTER", "LpMod", 1);
        const std::string after
            = playstop::apply(before, { .startMode = 1, .stopMode = 1, .fadeTime = 67 });
        for (const auto& [section, tag] : std::initializer_list<std::pair<const char*, const char*>> {
                 { "TRACK1", "MeasLen" }, { "TRACK1", "Measure" }, { "MASTER", "LpLen" },
                 { "MASTER", "LpMod" }, { "MASTER", "TrkMod" }, { "MASTER", "Sync" } })
            CHECK_EQ(rc0::sectionField(after, section, tag), rc0::sectionField(before, section, tag));
    }

    // --- refused before any byte moves ---

    CHECK_THROWS(playstop::apply(factory, {}), "no start/stop setting");
    CHECK_THROWS(playstop::apply(factory, { .startMode = 2 }), "START 2");
    CHECK_THROWS(playstop::apply(factory, { .stopMode = -1 }), "STOP -1");
    CHECK_THROWS(playstop::apply(factory, { .fadeTime = 68 }), "FADE TIME 68");
    // One bad field spoils the whole edit: the good ones do not land first.
    {
        std::string after;
        CHECK_THROWS(after = playstop::apply(factory, { .startMode = 1, .fadeTime = 999 }),
                     "FADE TIME 999");
        CHECK(after.empty());
    }
    // A body missing a section is refused by name, not read as defaults.
    {
        const std::string noMaster = factory.substr(0, factory.find("<MASTER>"));
        CHECK_THROWS(playstop::read(noMaster), "missing <MASTER>");
        CHECK_THROWS(playstop::apply(noMaster, { .fadeTime = 4 }), "missing <MASTER>");
    }
    // The section is the address: a second track's StrtMod is not this one's.
    {
        std::string two = factory;
        std::string track2 = factory.substr(factory.find("<TRACK1>"),
                                            factory.find("</TRACK1>") + 10 - factory.find("<TRACK1>"));
        track2.replace(track2.find("<TRACK1>"), 8, "<TRACK2>");
        track2.replace(track2.find("</TRACK1>"), 9, "</TRACK2>");
        two.insert(two.find("<MASTER>"), track2);
        const std::string set2 = rc0::setSectionField(two, "TRACK2", "StrtMod", 1);
        CHECK_EQ(playstop::read(set2).startMode, 0);
        const std::string after = playstop::apply(set2, { .startMode = 1, .stopMode = 2 });
        CHECK_EQ(rc0::sectionField(after, "TRACK2", "StpMod"), 0);
        CHECK(onlyTheseFieldsMoved(set2, after, { { "TRACK1", "StrtMod" }, { "TRACK1", "StpMod" } }));
    }

    // --- the words the history gets ---

    CHECK_EQ(playstop::describe({ .startMode = 1 }), "start FADE IN");
    CHECK_EQ(playstop::describe({ .stopMode = 2, .fadeTime = 5 }), "stop LOOP END, fade time 2MEAS");
    CHECK_EQ(playstop::describe({ .fadeTime = 2 }), "fade time 1/4 note");
    CHECK_THROWS(playstop::describe({ .stopMode = 9 }), "STOP 9");

    return testkit::summary("usecase_play_stop_tests");
}
