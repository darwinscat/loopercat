// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The model against a WHOLE REAL CARD, not a synthetic one.
//
// fixtures/rc5-card.RC0 is a MEMORY1.RC0 taken off an RC-5 (fw 1.10) after
// months of use: 99 memories, 41 of them holding a loop, values and byte
// layout exactly as the pedal wrote them, trailer included. The only edit is
// the names: every memory carries the pedal's own default MemoryNN, so the
// fixture does not age with someone's song titles. Nothing else was touched —
// not a byte of formatting, not a field, not the write counter.
//
// What the synthetic fixtures cannot prove and this one does: that the bytes
// this module writes are the bytes the hardware writes. A name block or a
// field rewritten in our own formatting would still parse, still round-trip
// through our own reader, and still be wrong on the pedal. Here every such
// write is asked to reproduce the card exactly.

#include "support.hpp"

#include <loopercat/Catalog.hpp>
#include <loopercat/DeviceProfile.hpp>
#include <loopercat/Params.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/usecases/CountIn.hpp>

#include <fstream>
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

} // namespace

int main()
{
    const std::string card = cardText();

    // --- it is a card, and it is ours ---

    // Its root names the RC-5, the model whose memories hold one track each.
    CHECK(&rc0::assertMemoryFile(card) == &profile::kRc5); // throws if the structure is off
    CHECK(rc0::tailMarker(card).has_value());
    CHECK_EQ(rc0::splitFile(card).tail.size(), static_cast<std::size_t>(5));

    // The pedal had saved many times by then: the counter is far past the
    // factory pair, and re-stamping it with its own value changes nothing.
    const std::uint32_t generation = *rc0::tailMarker(card);
    CHECK(generation > rc0::tailMarkerFor(2));
    CHECK(rc0::setTailGeneration(card, generation) == card);

    // --- what the browser sees ---

    const auto slots = catalog::listSlots(card);
    CHECK_EQ(slots.size(), static_cast<std::size_t>(rc0::kSlotCount));

    int withAudio = 0;
    for (const auto& slot : slots)
        withAudio += slot.hasAudio ? 1 : 0;
    CHECK_EQ(withAudio, 41); // the card as captured

    for (const auto& slot : slots) {
        const std::string body = rc0::slotBody(card, slot.slot);
        // One track per memory, and the flat fields are its.
        CHECK_EQ(slot.tracks.size(), 1u);
        CHECK_EQ(slot.tracks.front().frames, slot.frames);
        CHECK_EQ(slot.tracks.front().level,
                 rc0::sectionField(body, rc0::kSectionTrack1, "PlyLvl")); // 100 on every memory
        // The view agrees with the sections the values live in.
        CHECK_EQ(slot.frames, rc0::sectionField(body, rc0::kSectionTrack1, "WavLen"));
        CHECK_EQ(slot.measures, rc0::sectionField(body, rc0::kSectionTrack1, "MeasLen"));
        CHECK_EQ(slot.recTempoTenths, rc0::sectionField(body, rc0::kSectionTrack1, "RecTmp"));
        CHECK_EQ(slot.tempoTenths, rc0::sectionField(body, rc0::kSectionMaster, "Tempo"));
        // <Measure> is a bar count only above the offset: the first seven
        // values are the pedal's own measure modes, and this card uses one
        // (a memory whose loop is counted in beats, not bars). A reader that
        // subtracts 7 unconditionally would report a nonsense bar count, so
        // the two shapes are counted apart and both are expected.
        const long long measureField = rc0::sectionField(body, rc0::kSectionTrack1, "Measure");
        if (measureField >= params::kMeasureFieldOffset)
            CHECK_EQ(measureField, slot.measures + params::kMeasureFieldOffset);
        else
            CHECK(measureField >= 0);
        if (slot.hasAudio) {
            CHECK(slot.frames > 0);
            CHECK(slot.measures > 0);
        }
    }

    // The three shapes this card actually holds, pinned so that a reader
    // which stops seeing one of them fails here rather than in a musician's
    // hands: bars, a measure mode, and the factory-empty memory.
    {
        int bars = 0, measureMode = 0, factoryEmpty = 0, audio = 0, unindexed = 0;
        for (const auto& slot : slots) {
            const std::string body = rc0::slotBody(card, slot.slot);
            const long long measureField = rc0::sectionField(body, rc0::kSectionTrack1, "Measure");
            const long long wavStat = rc0::sectionField(body, rc0::kSectionTrack1, "WavStat");
            if (measureField >= params::kMeasureFieldOffset)
                ++bars;
            else if (measureField == 1 && slot.measures == 0)
                ++factoryEmpty;
            else
                ++measureMode;
            if (wavStat == 1)
                ++audio;
            else if (wavStat != 0)
                ++unindexed; // WavStat=2 — bars and a tempo, but no audio indexed
        }
        CHECK_EQ(bars, 41);
        CHECK_EQ(measureMode, 1);
        CHECK_EQ(factoryEmpty, 57);
        CHECK_EQ(audio, 41);
        // One memory carries WavStat=2: a file the pedal found and refused
        // to index — here a 16-bit take from before #44, which the pedal
        // reports as "unsupported" and will not play (hardware, 2026-09-24).
        // docs/pedal-settings.md records the value; the browser shows no
        // loop and doctor() explains the file. The count is pinned so this
        // card keeps testing that path.
        CHECK_EQ(unindexed, 1);
        for (const auto& slot : slots)
            if (rc0::sectionField(rc0::slotBody(card, slot.slot), rc0::kSectionTrack1, "WavStat")
                == 2)
                CHECK(!slot.hasAudio);
    }

    // --- the invariant, on every memory of a real card ---

    for (int slot = 1; slot <= rc0::kSlotCount; ++slot) {
        const std::string body = rc0::slotBody(card, slot);

        // Putting a memory's own body back reproduces the file.
        CHECK(rc0::replaceSlotBody(card, slot, body) == card);

        // Writing a field the value it already holds reproduces the memory —
        // scoped and unscoped alike. Our formatting of a number is the
        // pedal's formatting, or these differ.
        CHECK(rc0::setField(body, "Pan", rc0::field(body, "Pan")) == body);
        CHECK(rc0::setSectionField(body, rc0::kSectionTrack1, "MeasLen",
                                   rc0::sectionField(body, rc0::kSectionTrack1, "MeasLen"))
              == body);
        CHECK(rc0::setSectionField(body, rc0::kSectionRhythm, "Level",
                                   rc0::sectionField(body, rc0::kSectionRhythm, "Level"))
              == body);

        // The name block we write is the block the pedal wrote: same tags,
        // same tabs, same newlines, same char codes.
        CHECK(rc0::setName(body, rc0::decodeName(body)) == body);

        // Switching the count-in on and back off gives the card its bytes
        // back. Four memories arrive with the count already on — for those,
        // switching it on again must move nothing at all; the round trip is
        // asked of the memories that start without it.
        const std::string on = usecases::countin::apply(body, true);
        CHECK(usecases::countin::isOn(on));
        if (usecases::countin::isOn(body))
            CHECK(on == body);
        else
            CHECK(usecases::countin::apply(on, false) == body);
    }

    // --- the fixture is free of anyone's song titles ---

    for (const auto& slot : slots)
        CHECK_EQ(slot.name, rc0::defaultSlotName(slot.slot) + std::string(4, ' '));

    return testkit::summary("rc5_card");
}
