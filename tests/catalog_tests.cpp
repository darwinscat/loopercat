// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The read-only slot view: field semantics (WavStat/WavLen/One/Tempo) against
// a synthetic file built from the format's theory, including slots edited to
// carry audio-like values.

#include "support.hpp"

#include <loopercat/Catalog.hpp>
#include <loopercat/DeviceProfile.hpp>

using namespace loopercat;

int main()
{
    std::string text = testkit::syntheticMemoryText();

    // Give slot 7 an indexed loop and slot 9 a one-shot flag, the way the
    // pedal would (field edits only).
    {
        std::string body = rc0::slotBody(text, 7);
        body = rc0::setField(body, "WavStat", 1);
        body = rc0::setField(body, "WavLen", 6860867);
        body = rc0::setField(body, "Tempo", 987);
        text = rc0::replaceSlotBody(text, 7, body);

        std::string body9 = rc0::slotBody(text, 9);
        body9 = rc0::setField(body9, "One", 1);
        text = rc0::replaceSlotBody(text, 9, body9);

        // Slot 11: a count over a silenced rhythm (how this app writes it).
        // Slot 12: a count over a rhythm that plays a pattern — the pedal
        // sounds both. Slot 13: a rhythm playing with no count at all.
        std::string body11 = rc0::slotBody(text, 11);
        body11 = rc0::setField(body11, "State", rc0::kRhythmStateOn);
        body11 = rc0::setField(body11, "PlayCount", rc0::kRhythmPlayCount1Meas);
        body11 = rc0::setField(body11, "Pattern", rc0::kRhythmPatternBlank);
        text = rc0::replaceSlotBody(text, 11, body11);

        std::string body12 = rc0::slotBody(text, 12);
        body12 = rc0::setField(body12, "State", rc0::kRhythmStateOn);
        body12 = rc0::setField(body12, "PlayCount", rc0::kRhythmPlayCount1Meas);
        body12 = rc0::setField(body12, "Pattern", 11);
        text = rc0::replaceSlotBody(text, 12, body12);

        std::string body13 = rc0::slotBody(text, 13);
        body13 = rc0::setField(body13, "State", rc0::kRhythmStateOn);
        body13 = rc0::setField(body13, "Pattern", 11);
        text = rc0::replaceSlotBody(text, 13, body13);

        // Slot 14: a pattern picked on the pedal with the rhythm switched
        // off — the only slot where switching a count on costs something.
        std::string body14 = rc0::slotBody(text, 14);
        body14 = rc0::setField(body14, "Pattern", 11);
        text = rc0::replaceSlotBody(text, 14, body14);
    }

    const auto slots = catalog::listSlots(text);
    CHECK_EQ(slots.size(), static_cast<std::size_t>(rc0::kSlotCount));

    // Slot numbering is 1-based and in order.
    CHECK_EQ(slots.front().slot, 1);
    CHECK_EQ(slots.back().slot, 99);

    // An untouched slot: no audio, factory-ish defaults from the synthetic body.
    {
        const auto& s = slots.at(0);
        CHECK_EQ(s.name, "Memory 01   ");
        CHECK(!s.hasAudio);
        CHECK_EQ(s.frames, 0);
        CHECK(!s.oneShot);
        CHECK_EQ(s.tempoTenths, 1200);
        CHECK_EQ(s.recTempoTenths, 1200); // untouched: plays at its own tempo
    }

    // The slot with audio — Tempo was edited away from RecTmp, the state
    // where the pedal time-stretches on playback.
    {
        const auto& s = slots.at(6);
        CHECK(s.hasAudio);
        CHECK_EQ(s.frames, 6860867);
        CHECK_EQ(s.tempoTenths, 987);
        CHECK_EQ(s.recTempoTenths, 1200);
        CHECK(!s.oneShot);
    }

    // The one-shot slot.
    {
        const auto& s = slots.at(8);
        CHECK(s.oneShot);
        CHECK(!s.hasAudio);
    }

    // The indicator answers "will a count be heard", so a groove alongside
    // the count does not hide it, and a groove without one is not a count.
    CHECK(slots.at(10).countIn);
    CHECK(slots.at(11).countIn);
    CHECK(!slots.at(12).countIn);
    CHECK(!slots.at(0).countIn);

    // Only slot 14 pays for a count with a pattern it chose; the factory
    // pattern of an untouched slot is nobody's choice, and a rhythm already
    // playing keeps its pattern either way.
    CHECK(slots.at(13).countInTakesPattern);
    CHECK(!slots.at(12).countInTakesPattern);
    CHECK(!slots.at(0).countInTakesPattern);

    // readSlot agrees with listSlots.
    CHECK(catalog::readSlot(text, 7) == slots.at(6));

    // Each number comes from the section that owns it. A memory carrying a
    // second track (the two-track RC shape) holds every TRACK field twice;
    // the slot's loop facts are TRACK1's, and TRACK2's never leak into them.
    {
        const std::string track2 = "<TRACK2>\n\t<One>1</One>\n\t<MeasLen>64</MeasLen>\n"
                                   "\t<RecTmp>900</RecTmp>\n\t<WavStat>1</WavStat>\n"
                                   "\t<WavLen>999999</WavLen>\n</TRACK2>\n";
        std::string body = rc0::slotBody(text, 7);
        body.insert(body.find("<MASTER>"), track2);
        const std::string twoTrack = rc0::replaceSlotBody(text, 7, body);
        const catalog::SlotInfo info = catalog::readSlot(twoTrack, 7);
        CHECK(info == slots.at(6));
        CHECK_EQ(info.frames, 6860867);
        CHECK(!info.oneShot);
    }

    // Malformed input propagates as a typed error, never a default.
    CHECK_THROWS(catalog::readSlot(text, 0), "out of range");
    CHECK_THROWS(catalog::listSlots("<database></database>"), "not an RC0");

    // --- tracks: a memory holds as many as its model has ---

    // On the RC-5 every memory holds exactly one track, and the flat fields
    // are that track's — on every slot, edited or not.
    for (const auto& s : slots) {
        CHECK_EQ(s.tracks.size(), 1u);
        const catalog::TrackInfo& t = s.tracks.front();
        CHECK_EQ(t.track, 1);
        CHECK(t.hasAudio == s.hasAudio);
        CHECK_EQ(t.frames, s.frames);
        CHECK(t.oneShot == s.oneShot);
        CHECK_EQ(t.measures, s.measures);
        CHECK_EQ(t.recTempoTenths, s.recTempoTenths);
    }

    // The two-track model (testkit::syntheticTwoTrackMemoryText — the numbers
    // measured on its card): two tracks per memory, each with its own take,
    // length and recording tempo; the playback tempo is the memory's.
    {
        const std::string twoTrack = testkit::syntheticTwoTrackMemoryText();
        const auto memories = catalog::listSlots(twoTrack);
        CHECK_EQ(memories.size(), static_cast<std::size_t>(rc0::kSlotCount));
        for (const auto& m : memories)
            CHECK_EQ(m.tracks.size(), 2u);

        // Memory 1: both tracks 8 bars at 132.0 BPM.
        using catalog::TrackInfo;
        CHECK(memories.at(0).tracks.at(0) == (TrackInfo { 1, true, 641408, false, 8, 1320 }));
        CHECK(memories.at(0).tracks.at(1) == (TrackInfo { 2, true, 641408, false, 8, 1320 }));
        CHECK_EQ(memories.at(0).tempoTenths, 1320);
        // Memory 3: a 4-bar track 1 beside an 8-bar track 2 — different
        // lengths under one tempo.
        CHECK(memories.at(2).tracks.at(0) == (TrackInfo { 1, true, 282240, false, 4, 1500 }));
        CHECK(memories.at(2).tracks.at(1) == (TrackInfo { 2, true, 564480, false, 8, 1500 }));
        // Memory 2: track 2 empty, factory-shaped.
        CHECK(memories.at(1).tracks.at(1) == (TrackInfo { 2, false, 0, false, 0, 959 }));
        // Memory 11: the take is on track 2 alone. The flat fields are
        // TRACK1's and say "empty" — the one-track view's honest limit, which
        // is why a caller showing this card reads `tracks`.
        CHECK(!memories.at(10).hasAudio);
        CHECK_EQ(memories.at(10).frames, 0);
        CHECK(memories.at(10).tracks.at(1) == (TrackInfo { 2, true, 362496, false, 4, 1167 }));
        // Memory 42: factory-empty on both tracks.
        CHECK(memories.at(41).tracks.at(0) == (TrackInfo { 1, false, 0, false, 0, 1200 }));
        CHECK(memories.at(41).tracks.at(1) == (TrackInfo { 2, false, 0, false, 0, 1200 }));
        // readSlot, told the model or reading it off the root, agrees.
        CHECK(catalog::readSlot(twoTrack, 3) == memories.at(2));
        CHECK(catalog::readSlot(twoTrack, profile::kRc500, 3) == memories.at(2));

        // A memory that lacks a track its model promises is refused by name,
        // not read as an empty track.
        std::string body = rc0::slotBody(twoTrack, 5);
        const auto open = body.find("<TRACK2>");
        const auto close = body.find("</TRACK2>\n", open);
        CHECK(open != std::string::npos && close != std::string::npos);
        body.erase(open, close + std::string("</TRACK2>\n").size() - open);
        const std::string oneShort = rc0::replaceSlotBody(twoTrack, 5, body);
        CHECK_THROWS(catalog::readSlot(oneShort, 5), "missing <TRACK2> section");
        CHECK_THROWS(catalog::listSlots(oneShort), "missing <TRACK2> section");
        CHECK(catalog::readSlot(oneShort, 4) == memories.at(3)); // its neighbours still read
    }

    return testkit::summary("catalog");
}
