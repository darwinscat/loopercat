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

#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <optional>
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

// A wall-clock moment on this computer, in the store's milliseconds: the
// clock a player reads, built from its own fields rather than from the
// function under test.
std::int64_t localMs(int year, int month, int day, int hour, int minute, int second = 0)
{
    std::tm fields {};
    fields.tm_year = year - 1900;
    fields.tm_mon = month - 1;
    fields.tm_mday = day;
    fields.tm_hour = hour;
    fields.tm_min = minute;
    fields.tm_sec = second;
    fields.tm_isdst = -1;
    const std::time_t seconds = std::mktime(&fields);
    if (seconds == static_cast<std::time_t>(-1))
        throw Error("no such local moment");
    return static_cast<std::int64_t>(seconds) * 1000;
}

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
        // without a body left behind — an attempt that failed, or found
        // nothing to change — the switch is named and no position invented
        CHECK_EQ(story::tell({ .kind = "oneshot" }).action, std::string("One Shot"));
        CHECK_EQ(story::tell({ .kind = "oneshot", .beforeBody = pushed }).action, std::string("One Shot"));
        CHECK_EQ(story::tell({ .kind = "countin" }).action, std::string("Play Count-In"));
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

    // --- the app's own names for a slot's drums and its Start & Stop, never
    // the kinds the store files them under ---
    {
        const story::Line rhythm = story::tell({ .kind = "rhythm", .beforeBody = pushed,
                                                 .afterBody = pushed, .status = "done" });
        CHECK_EQ(rhythm.action, std::string("Rhythm"));
        const story::Line playStop = story::tell({ .kind = "playstop", .beforeBody = pushed,
                                                   .afterBody = pushed, .status = "done" });
        CHECK_EQ(playStop.action, std::string("Start & Stop"));
        for (const story::Line& line : { rhythm, playStop }) {
            CHECK(line.action.find("rhythm") == std::string::npos);
            CHECK(line.action.find("playstop") == std::string::npos);
        }
    }

    // --- a row this build has no words for says its own name ---
    {
        const story::Line line = story::tell({ .kind = "teleport", .note = "to slot 99" });
        CHECK_EQ(line.action, std::string("teleport"));
        CHECK_EQ(line.detail, std::string("to slot 99"));
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

    // --- a push or a trim that replaced a note-value length says so, after its numbers ---
    {
        CHECK_EQ(std::string(story::kNoteLengthReplaced), std::string("note length replaced by bars"));
        const std::string longer = rc0::setSectionField(rc0::factorySlotBody(40), rc0::kSectionTrack1, "WavLen", 54352);
        const std::string shorter = rc0::setSectionField(rc0::factorySlotBody(40), rc0::kSectionTrack1, "WavLen", 27176);
        const auto trimLine = story::tell({ .kind = "trim", .beforeBody = longer, .afterBody = shorter,
                                           .take = story::Take::onCard,
                                           .note = std::string(story::kNoteLengthReplaced) });
        CHECK_EQ(trimLine.action, std::string("Trimmed"));
        CHECK_EQ(trimLine.detail, std::string("0:01 -> 0:00 - note length replaced by bars"));
        const auto plain = story::tell({ .kind = "trim", .beforeBody = longer, .afterBody = shorter,
                                         .take = story::Take::onCard });
        CHECK_EQ(plain.detail, std::string("0:01 -> 0:00"));
        // a push carries the fact behind a normalize line, or alone, or not at all
        const auto pushLine = story::tell({ .kind = "push", .beforeBody = longer, .afterBody = shorter,
                                          .takeName = "take.wav", .take = story::Take::onCard,
                                          .status = "done",
                                          .note = "normalized -3.2 dB; " + std::string(story::kNoteLengthReplaced) });
        CHECK(pushLine.detail.find("take.wav") == 0);
        CHECK(pushLine.detail.find(" - note length replaced by bars") == pushLine.detail.size() - std::string(" - note length replaced by bars").size());
        const auto alone = story::tell({ .kind = "push", .takeName = "take.wav", .take = story::Take::onCard,
                                         .note = std::string(story::kNoteLengthReplaced) });
        CHECK_EQ(alone.detail, std::string("take.wav - note length replaced by bars"));
        const auto quiet = story::tell({ .kind = "push", .takeName = "take.wav", .take = story::Take::onCard,
                                         .status = "done", .note = "normalized -3.2 dB" });
        CHECK_EQ(quiet.detail, std::string("take.wav - normalized -3.2 dB"));
        // the fact belongs to push and trim: a rename's note is a rename's note
        const auto other = story::tell({ .kind = "rename", .take = story::Take::none,
                                         .note = std::string(story::kNoteLengthReplaced) });
        CHECK(other.detail.find("note length") == std::string::npos);
    }

    // --- a push row says what the job said about the take (issue #139) ---
    //
    // The row names the take as it landed — the mark is in the name — keeps
    // its numbers, and then draws the note's sentences: the rebuild's facts,
    // the normalization's. The note-length fact keeps its own place at the
    // end, drawn once, and is left out of the sentences.
    {
        const auto converted = story::tell({ .kind = "push", .beforeBody = trimmed, .afterBody = pushed,
                                             .takeName = "song-pedal.wav", .take = story::Take::onCard,
                                             .status = "done",
                                             .note = "48000 Hz, 24-bit, mono \xe2\x86\x92 44100 Hz, 32-bit float, stereo" });  // utf8-data: a job's note as the importer writes it
        CHECK_EQ(converted.detail,
                 std::string("song-pedal.wav - 4:36 - 111.1 BPM - 48000 Hz, 24-bit, mono \xe2\x86\x92 44100 Hz, 32-bit float, stereo"));  // utf8-data: the same note, passed through
        CHECK_EQ(converted.audio, std::string("in the slot now"));

        const auto both = story::tell({ .kind = "push", .beforeBody = trimmed, .afterBody = pushed,
                                        .takeName = "song-pedal.wav", .take = story::Take::onCard,
                                        .status = "done",
                                        .note = "24-bit \xe2\x86\x92 32-bit float; normalized +3.0 dB; "  // utf8-data: a job's note as the importer writes it
                                            + std::string(story::kNoteLengthReplaced) });
        CHECK_EQ(both.detail,
                 std::string("song-pedal.wav - 4:36 - 111.1 BPM - 24-bit \xe2\x86\x92 32-bit float; normalized +3.0 dB"  // utf8-data: the same note, passed through
                             " - note length replaced by bars"));

        // no bodies (a row an older store kept): the name and the sentence
        const auto bare = story::tell({ .kind = "push", .takeName = "song.wav", .take = story::Take::kept,
                                        .status = "done", .note = "MP3 \xe2\x86\x92 32-bit float" });  // utf8-data: a job's note as the importer writes it
        CHECK_EQ(bare.detail, std::string("song.wav - MP3 \xe2\x86\x92 32-bit float"));  // utf8-data: the same note, passed through

        // a note that is only the fact adds nothing twice
        const auto onlyFact = story::tell({ .kind = "push", .takeName = "take.wav", .take = story::Take::onCard,
                                            .status = "done",
                                            .note = std::string(story::kNoteLengthReplaced) + "; " });
        CHECK_EQ(onlyFact.detail, std::string("take.wav - note length replaced by bars"));

        // a failed push's note is its error, and the row's state already says
        // it failed: the line keeps its old shape, the error is not a sentence
        const auto failed = story::tell({ .kind = "push", .takeName = "take.wav", .take = story::Take::onCard,
                                          .status = "failed",
                                          .note = "slot 9 already has audio (loop.wav); pass force to replace" });
        CHECK_EQ(failed.detail, std::string("take.wav"));
        const auto failedBodies = story::tell({ .kind = "push", .beforeBody = trimmed, .afterBody = pushed,
                                                .takeName = "take.wav", .take = story::Take::onCard,
                                                .status = "failed", .note = "cannot write MEMORY1.RC0" });
        CHECK_EQ(failedBodies.detail, std::string("take.wav - 4:36 - 111.1 BPM"));
        // an interrupted one, and a row an older build wrote with no status: the same
        const auto interrupted = story::tell({ .kind = "push", .takeName = "take.wav", .take = story::Take::onCard,
                                               .status = "interrupted", .note = "24-bit \xe2\x86\x92 32-bit float" });  // utf8-data: a job's note as the importer writes it
        CHECK_EQ(interrupted.detail, std::string("take.wav"));
        const auto unknown = story::tell({ .kind = "push", .takeName = "take.wav", .take = story::Take::onCard,
                                           .note = "24-bit \xe2\x86\x92 32-bit float" });  // utf8-data: a job's note as the importer writes it
        CHECK_EQ(unknown.detail, std::string("take.wav"));

        // the helper itself: the fact is dropped wherever it sits, the rest keeps its order
        CHECK_EQ(story::sentencesBesides("a; b; c", "b"), std::string("a; c"));
        CHECK_EQ(story::sentencesBesides("b; a", "b"), std::string("a"));
        CHECK_EQ(story::sentencesBesides("b", "b"), std::string(""));
        CHECK_EQ(story::sentencesBesides("", "b"), std::string(""));
        CHECK_EQ(story::sentencesBesides("a;b", "b"), std::string("a;b")); // not the join
    }

    // --- the card as first seen: the Original, named by the take it held ---
    {
        const story::Line line = story::tell({ .kind = "snapshot", .afterBody = pushed,
                                               .takeName = "deep-space.wav", .take = story::Take::kept,
                                               .status = "done" });
        CHECK_EQ(line.action, std::string("Original"));
        CHECK_EQ(line.detail, std::string("deep-space.wav"));
        CHECK_EQ(line.audio, std::string("take kept"));
    }

    // --- a row's clock: the time alone on the reference's own day, the date
    // with it on any other — the History tab's two forms ---
    {
        const std::int64_t at = localMs(2026, 10, 11, 17, 52);
        CHECK_EQ(story::clockWords(at, at), std::string("17:52"));
        CHECK_EQ(story::clockWords(at, localMs(2026, 10, 11, 23, 59, 59)), std::string("17:52"));
        CHECK_EQ(story::clockWords(at, localMs(2026, 10, 11, 0, 0)), std::string("17:52"));
        CHECK_EQ(story::clockWords(at, localMs(2026, 10, 12, 0, 0)), std::string("11 Oct 17:52"));
        CHECK_EQ(story::clockWords(at, localMs(2026, 10, 10, 23, 59)), std::string("11 Oct 17:52"));
        // the minute is cut, never rounded up — 17:52:59 is still 17:52
        CHECK_EQ(story::clockWords(at + 59'999, at), std::string("17:52"));
        CHECK_EQ(story::clockWords(localMs(2026, 10, 11, 0, 0), at), std::string("00:00"));
        // the same day of another year is another day
        CHECK_EQ(story::clockWords(localMs(2025, 1, 5, 9, 7), localMs(2026, 1, 5, 9, 7)),
                 std::string("05 Jan 09:07"));
    }

    // --- a restore says what came back and which row it came from ---
    {
        const std::int64_t restoredAt = localMs(2026, 10, 11, 17, 52);
        const std::int64_t sameDay = localMs(2026, 10, 11, 18, 3);
        const std::int64_t nextDay = localMs(2026, 10, 12, 9, 30);
        const auto restore = [&](std::optional<std::string> after, std::string takeName,
                                 std::int64_t at, std::optional<std::int64_t> to) {
            return story::tell({ .kind = "restore",
                                 .beforeBody = bodyWith(kFrames4m36, 1111, "Memory42"),
                                 .afterBody = std::move(after),
                                 .takeName = std::move(takeName),
                                 .take = story::Take::onCard,
                                 .status = "done",
                                 .at = at,
                                 .restoredAt = to });
        };
        const auto line = restore(trimmed, "i-am-projection.wav", sameDay, restoredAt);
        CHECK_EQ(line.action, std::string("Restored"));
        CHECK_EQ(line.detail, std::string("i-am-projection.wav - 0:39 - 113.0 BPM - back to 17:52"));
        CHECK_EQ(restore(trimmed, "i-am-projection.wav", nextDay, restoredAt).detail,
                 std::string("i-am-projection.wav - 0:39 - 113.0 BPM - back to 11 Oct 17:52"));
        // back to a state that held no take
        CHECK_EQ(restore(bodyWith(0, 1200, "Memory42"), "", sameDay, restoredAt).detail,
                 std::string("empty slot - back to 17:52"));
        // a restore recorded before the store kept its row, or whose row was
        // forgotten: what came back, and no invented time
        CHECK_EQ(restore(trimmed, "i-am-projection.wav", sameDay, std::nullopt).detail,
                 std::string("i-am-projection.wav - 0:39 - 113.0 BPM"));
        // no state recorded (cut off before the card was written): where it was going
        CHECK_EQ(restore(std::nullopt, "", sameDay, restoredAt).detail, std::string("back to 17:52"));
        // the job writes no note; one that is there says nothing more about the row
        const auto noted = story::tell({ .kind = "restore", .beforeBody = pushed, .afterBody = trimmed,
                                         .takeName = "a.wav", .take = story::Take::onCard,
                                         .status = "done", .note = "stray words",
                                         .at = sameDay, .restoredAt = restoredAt });
        CHECK_EQ(noted.detail, std::string("a.wav - 0:39 - 113.0 BPM - back to 17:52"));
    }

    // --- ATTACK (review of feat/history-row-menu): the clock at its edges, in
    // a zone that keeps daylight time (Europe/Berlin), set for this block
    // alone. A moment is a millisecond count; its minute is the local clock's
    // minute, before the epoch as after it. ---
#if !defined(_WIN32)
    {
        const char* hadZone = std::getenv("TZ");
        const std::optional<std::string> oldZone =
            hadZone != nullptr ? std::optional<std::string>(hadZone) : std::nullopt;
        setenv("TZ", "Europe/Berlin", 1);
        tzset();
        // midnight, and the last millisecond before it
        CHECK_EQ(story::clockWords(localMs(2026, 10, 11, 0, 0), localMs(2026, 10, 11, 12, 0)), std::string("00:00"));
        CHECK_EQ(story::clockWords(localMs(2026, 10, 12, 0, 0) - 1, localMs(2026, 10, 12, 0, 0)),
                 std::string("11 Oct 23:59"));
        // the year's edge
        CHECK_EQ(story::clockWords(localMs(2025, 12, 31, 23, 59), localMs(2026, 1, 1, 0, 0)),
                 std::string("31 Dec 23:59"));
        CHECK_EQ(story::clockWords(localMs(2026, 1, 1, 0, 0), localMs(2025, 12, 31, 23, 59)),
                 std::string("01 Jan 00:00"));
        // the day the clocks go forward (2026-03-29, 02:00 becomes 03:00)
        CHECK_EQ(story::clockWords(localMs(2026, 3, 29, 1, 59), localMs(2026, 3, 29, 3, 1)), std::string("01:59"));
        CHECK_EQ(story::clockWords(localMs(2026, 3, 29, 3, 0), localMs(2026, 3, 28, 23, 0)),
                 std::string("29 Mar 03:00"));
        CHECK_EQ(story::clockWords(localMs(2026, 3, 29, 1, 59) + 60'000, localMs(2026, 3, 29, 1, 59)),
                 std::string("03:00")); // the minute after 01:59 is 03:00
        // the day they go back (2026-10-25): 02:30 happens twice, and both are 02:30
        const std::int64_t fallBack = localMs(2026, 10, 25, 1, 30); // 01:30 CEST, before the repeated hour
        CHECK_EQ(story::clockWords(fallBack + 3'600'000, fallBack), std::string("02:30"));
        CHECK_EQ(story::clockWords(fallBack + 7'200'000, fallBack), std::string("02:30"));
        CHECK_EQ(story::clockWords(fallBack + 7'200'000, localMs(2026, 10, 26, 9, 0)), std::string("25 Oct 02:30"));
        // the epoch is 1970-01-01 01:00 in Berlin; the millisecond before it is 00:59
        CHECK_EQ(story::clockWords(0, 0), std::string("01:00"));
        CHECK_EQ(story::clockWords(-1000, 0), std::string("00:59"));
        CHECK_EQ(story::clockWords(-1, 0), std::string("00:59"));
        CHECK_EQ(story::clockWords(-1, -1), std::string("00:59"));
        // the ends of the count: refused or read, never a crash
        for (const std::int64_t edge : { std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min() }) {
            try {
                const std::string words = story::clockWords(edge, 0);
                CHECK(!words.empty());
            } catch (const Error&) {
                CHECK(true);
            }
        }
        if (oldZone)
            setenv("TZ", oldZone->c_str(), 1);
        else
            unsetenv("TZ");
        tzset();
    }
#endif

    // --- ATTACK (review): the month's name is strftime's, which answers in
    // the process locale. A row's words are built in a JUCE-free header and
    // are meant to be ASCII (the lint's rule); under a locale with another
    // alphabet they are not, and the column and the row would both change
    // alphabet with the locale. ---
    {
        const char* was = std::setlocale(LC_TIME, nullptr);
        const std::string before = was != nullptr ? was : "C";
        if (std::setlocale(LC_TIME, "ru_RU.UTF-8") != nullptr) {
            const std::string words =
                story::clockWords(localMs(2026, 10, 11, 17, 52), localMs(2026, 10, 12, 0, 0));
            std::setlocale(LC_TIME, before.c_str());
            bool ascii = true;
            for (const char c : words)
                ascii = ascii && static_cast<unsigned char>(c) < 0x80;
            CHECK(ascii);
            CHECK_EQ(words, std::string("11 Oct 17:52"));
        }
    }

    // --- One Shot of a two-track memory: the track that changed, read off its own section ---
    {
        const std::string loops = testkit::syntheticTwoTrackSlotBody("Memory 4", { 4, 176400 },
                                                                     { 8, 352800 }, 1200, 4);
        const std::string t2on = rc0::setSectionField(loops, "TRACK2", "One", 1);
        CHECK_EQ(story::tell({ .kind = "oneshot", .beforeBody = loops, .afterBody = t2on }).action,
                 std::string("One Shot T2 on"));
        const std::string t1on = rc0::setSectionField(loops, "TRACK1", "One", 1);
        const std::string both = rc0::setSectionField(t1on, "TRACK2", "One", 1);
        // Track 2 off while track 1 stays on: "off", and about track 2.
        CHECK_EQ(story::tell({ .kind = "oneshot", .beforeBody = both, .afterBody = t1on }).action,
                 std::string("One Shot T2 off"));
        CHECK_EQ(story::tell({ .kind = "oneshot", .beforeBody = t2on, .afterBody = both }).action,
                 std::string("One Shot T1 on"));
        // A body that did not change names no track and claims nothing.
        CHECK_EQ(story::tell({ .kind = "oneshot", .beforeBody = both, .afterBody = both }).action,
                 std::string("One Shot"));
        // Without both bodies: the switch, no claim.
        CHECK_EQ(story::tell({ .kind = "oneshot", .afterBody = t2on }).action, std::string("One Shot"));
    }

    return testkit::summary("slot_story_tests");
}
