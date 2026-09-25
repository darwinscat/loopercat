// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What a row of a slot's history says (issue #50), tested from the shapes a
// real card produced: the numbers below are the ones Alisa's RC-5 wrote
// during the first live run — a 4:36 loop pushed at 111.1 BPM, its tempo set
// to 113.0, trimmed to 0:39, swapped, cleared.
//
// The theory being tested is what a row is FOR: it names the operation and
// the one thing that operation was about. A swap moves the name, the tempo,
// the length, the bar count and the one-shot flag all at once — a row that
// listed them would bury the reader, so it says what happened instead. And a
// row never invents: an operation this build has no words for says its own
// name rather than nothing.

#include "support.hpp"

#include "../app/history/SlotStory.h"

#include <string>

using namespace loopercat;
namespace story = loopercat::history::story;

namespace {

// A slot body as the card holds it, with the fields a row reads.
std::string bodyWith(long long wavLen, long long tempo, const std::string& name, bool oneShot = false)
{
    std::string body = rc0::factorySlotBody(42);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "WavLen", wavLen);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "One", oneShot ? 1 : 0);
    body = rc0::setSectionField(body, rc0::kSectionMaster, "Tempo", tempo);
    return rc0::setName(body, name);
}

constexpr long long kFrames4m36 = 12193650; // 276.5 s, the pushed loop
constexpr long long kFrames0m39 = 1719900;  // 39.0 s, what the trim left

} // namespace

int main()
{
    const std::string pushed = bodyWith(kFrames4m36, 1111, "Memory42");
    const std::string trimmed = bodyWith(kFrames0m39, 1130, "TEST_42_HIST");

    // --- each operation says the one thing it was about ---
    {
        const story::Line line = story::tell({ .kind = "push",
                                               .beforeBody = bodyWith(0, 1200, "Memory42"),
                                               .afterBody = pushed,
                                               .takeName = "1 - deep-space.wav",
                                               .take = story::Take::onCard });
        CHECK_EQ(line.action, std::string("Pushed"));
        CHECK_EQ(line.detail, std::string("1 - deep-space.wav - 4:36 - 111.1 BPM"));
        CHECK_EQ(line.audio, std::string("in the slot now"));
    }
    {
        const story::Line line = story::tell({ .kind = "trim",
                                               .beforeBody = pushed,
                                               .afterBody = trimmed,
                                               .take = story::Take::kept });
        CHECK_EQ(line.action, std::string("Trimmed"));
        CHECK_EQ(line.detail, std::string("4:36 -> 0:39"));
        CHECK_EQ(line.audio, std::string("take kept"));
    }
    {
        const story::Line line = story::tell({ .kind = "rename",
                                               .beforeBody = bodyWith(kFrames0m39, 1130, "Memory42"),
                                               .afterBody = trimmed });
        CHECK_EQ(line.action, std::string("Renamed"));
        CHECK_EQ(line.detail, std::string("Memory42 -> TEST_42_HIST"));
    }
    {
        const story::Line line = story::tell({ .kind = "tempo",
                                               .beforeBody = pushed,
                                               .afterBody = bodyWith(kFrames4m36, 1130, "Memory42") });
        CHECK_EQ(line.action, std::string("Tempo"));
        CHECK_EQ(line.detail, std::string("111.1 BPM -> 113.0 BPM"));
    }
    {
        const story::Line on = story::tell({ .kind = "oneshot",
                                             .beforeBody = pushed,
                                             .afterBody = bodyWith(kFrames4m36, 1111, "Memory42", true) });
        CHECK_EQ(on.action, std::string("One Shot on"));
        const story::Line off = story::tell({ .kind = "oneshot",
                                              .beforeBody = bodyWith(kFrames4m36, 1111, "Memory42", true),
                                              .afterBody = pushed });
        CHECK_EQ(off.action, std::string("One Shot off"));
    }

    // --- a swap says what happened, not the six fields it moved ---
    {
        const story::Line line = story::tell({ .kind = "swap",
                                               .beforeBody = trimmed,
                                               .afterBody = bodyWith(0, 1200, "Memory43"),
                                               .swappedWith = 43 });
        CHECK_EQ(line.action, std::string("Swapped with slot 43"));
        CHECK_EQ(line.detail, std::string()); // the name, tempo and length all moved: none is the point
        // and the slot it exchanged with is named, so the row reads on its own
        const story::Line other = story::tell({ .kind = "swap",
                                                .beforeBody = bodyWith(0, 1200, "Memory43"),
                                                .afterBody = trimmed,
                                                .swappedWith = 42,
                                                .takeName = "1 - deep-space.wav",
                                                .take = story::Take::onCard });
        CHECK_EQ(other.action, std::string("Swapped with slot 42"));
        CHECK_EQ(other.audio, std::string("in the slot now"));
    }

    // --- the two clears look different, because they are ---
    {
        const story::Line kept = story::tell({ .kind = "clear",
                                               .beforeBody = trimmed,
                                               .afterBody = rc0::factorySlotBody(43),
                                               .take = story::Take::kept });
        CHECK_EQ(kept.action, std::string("Cleared"));
        CHECK_EQ(kept.audio, std::string("take kept"));
        const story::Line gone = story::tell({ .kind = "clear",
                                               .beforeBody = trimmed,
                                               .afterBody = rc0::factorySlotBody(43),
                                               .take = story::Take::none });
        CHECK_EQ(gone.audio, std::string("nothing left in the slot"));
    }

    // --- an operation that changed no field speaks through its own line ---
    {
        // normalize found the slot already at target: no body moved, and the
        // sentence the job wrote is the only record there is
        const story::Line line = story::tell({ .kind = "normalize",
                                               .note = "already at -18.0 LUFS" });
        CHECK_EQ(line.action, std::string("Normalized"));
        CHECK_EQ(line.detail, std::string("already at -18.0 LUFS"));
    }

    // --- a take whose bytes are gone says so, and never silently ---
    {
        const story::Line line = story::tell({ .kind = "trim",
                                               .beforeBody = pushed,
                                               .afterBody = trimmed,
                                               .take = story::Take::lost });
        CHECK_EQ(line.audio, std::string("take no longer kept"));
    }

    // --- a row this build has no words for says its own name ---
    {
        const story::Line line = story::tell({ .kind = "legacy", .note = "trash/2026-09-01T21-35-46" });
        CHECK_EQ(line.action, std::string("legacy"));
        CHECK_EQ(line.detail, std::string("trash/2026-09-01T21-35-46"));
    }

    // --- lengths and tempos read the way a musician says them ---
    {
        CHECK_EQ(story::minutes(0), std::string("0:00"));
        CHECK_EQ(story::minutes(44100), std::string("0:01"));
        CHECK_EQ(story::minutes(44100 * 59), std::string("0:59"));
        CHECK_EQ(story::minutes(44100 * 60), std::string("1:00"));
        CHECK_EQ(story::minutes(44100 * 61), std::string("1:01"));
        // cut, not rounded: the slot table and the player say 4:36 for this
        // loop, and the history must not call it 4:37
        CHECK_EQ(story::minutes(44099), std::string("0:00"));
        CHECK_EQ(story::minutes(kFrames4m36), std::string("4:36"));
        CHECK_EQ(story::bpm(400), std::string("40.0 BPM"));    // the pedal's floor
        CHECK_EQ(story::bpm(3000), std::string("300.0 BPM"));  // and its ceiling
        CHECK_EQ(story::bpm(1111), std::string("111.1 BPM"));
    }

    // --- an undo and a redo say what they undid, and where the slot went back to ---
    {
        const std::string before = rc0::setSectionField(rc0::factorySlotBody(14), rc0::kSectionTrack1,
                                                        "WavLen", 441000);
        const std::string after = rc0::setSectionField(rc0::factorySlotBody(14), rc0::kSectionTrack1,
                                                       "WavLen", 1719900);
        const auto undone = story::tell({ .kind = "undo", .beforeBody = before, .afterBody = after,
                                          .take = story::Take::kept, .note = "trim" });
        CHECK_EQ(undone.action, std::string("Undid trim"));
        CHECK_EQ(undone.detail, std::string("back to 0:39"));
        CHECK_EQ(undone.audio, std::string("take kept"));
        const auto redone = story::tell({ .kind = "redo", .beforeBody = after, .afterBody = before,
                                          .take = story::Take::onCard, .note = "trim" });
        CHECK_EQ(redone.action, std::string("Redid trim"));
        CHECK_EQ(redone.detail, std::string("back to 0:10"));
        const auto emptied = story::tell({ .kind = "undo", .beforeBody = after,
                                           .afterBody = rc0::setSectionField(rc0::factorySlotBody(14), rc0::kSectionTrack1, "WavLen", 0),
                                           .take = story::Take::none, .note = "push" });
        CHECK_EQ(emptied.action, std::string("Undid push"));
        CHECK_EQ(emptied.detail, std::string("back to empty"));
        const auto bare = story::tell({ .kind = "undo", .take = story::Take::none });
        CHECK_EQ(bare.action, std::string("Undid"));
        CHECK_EQ(bare.detail, std::string());
    }

    // --- a settings section: which fields moved, in the file's order, numbers only ---
    {
        const std::string before = "<CTL>\n\t<Pedal1>4</Pedal1>\n\t<Ctl1>17</Ctl1>\n\t<Ctl2>18</Ctl2>\n\t<Cc80>0</Cc80>\n</CTL>";
        const std::string after = "<CTL>\n\t<Pedal1>4</Pedal1>\n\t<Ctl1>-1</Ctl1>\n\t<Ctl2>22</Ctl2>\n\t<Cc80>0</Cc80>\n</CTL>";
        const auto fields = story::sectionFields(before);
        CHECK_EQ(fields.size(), 4u);
        CHECK(fields.size() == 4 && fields[0].first == "Pedal1" && fields[0].second == 4);
        CHECK(fields.size() == 4 && fields[3].first == "Cc80" && fields[3].second == 0);
        const auto moved = story::fieldChanges(before, after);
        CHECK_EQ(moved.size(), 2u);
        CHECK(moved.size() == 2 && moved[0].tag == "Ctl1" && moved[0].before == 17 && moved[0].after == -1);
        CHECK(moved.size() == 2 && moved[1].tag == "Ctl2" && moved[1].before == 18 && moved[1].after == 22);
        CHECK(story::fieldChanges(before, before).empty());
        // a tag on one side only, and a value that is not a number, are no words
        CHECK(story::fieldChanges(before, "<CTL>\n\t<Ctl2>18</Ctl2>\n\t<New>5</New>\n</CTL>").empty());
        CHECK(story::sectionFields("<CTL>\n\t<Name>abc</Name>\n\t<Ctl2>3</Ctl2>\n</CTL>").size() == 1);
        CHECK(story::sectionFields("").empty());
        CHECK(story::sectionFields("<CTL>").empty());
        CHECK(story::sectionFields("<CTL><Ctl2>1").empty()); // unterminated: nothing claimed
        const auto line = story::tellSystem({ { "CTL", before, after } });
        CHECK_EQ(line.action, std::string("Pedal controls changed"));
        CHECK_EQ(line.detail, std::string("Ctl1 17 -> -1, Ctl2 18 -> 22"));
        const auto mixed = story::tellSystem({ { "MIDI", "<MIDI>\n\t<RxCh>1</RxCh>\n</MIDI>", "<MIDI>\n\t<RxCh>2</RxCh>\n</MIDI>" }, { "CTL", before, after } });
        CHECK_EQ(mixed.action, std::string("Pedal settings changed"));
        CHECK_EQ(mixed.detail, std::string("RxCh 1 -> 2, Ctl1 17 -> -1, Ctl2 18 -> 22"));
        CHECK_EQ(story::tellSystem({}).action, std::string("Pedal settings changed"));
        CHECK_EQ(story::tellSystem({}).detail, std::string());
    }

    return testkit::summary("slot_story_tests");
}
