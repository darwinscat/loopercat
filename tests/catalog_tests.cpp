// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The read-only slot view: field semantics (WavStat/WavLen/One/Tempo) against
// a synthetic file built from the format's theory, including slots edited to
// carry audio-like values.

#include "support.hpp"

#include <loopercat/Catalog.hpp>

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

    // Malformed input propagates as a typed error, never a default.
    CHECK_THROWS(catalog::readSlot(text, 0), "out of range");
    CHECK_THROWS(catalog::listSlots("<database></database>"), "missing <mem");

    return testkit::summary("catalog");
}
