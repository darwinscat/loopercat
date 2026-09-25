// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Rc0.hpp>
#include <loopercat/usecases/CountIn.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

//==============================================================================
// loopercat::history::story — what one row of a slot's history says, in words
// (issue #50). A pure reading of what the store recorded: no database, no
// files, no UI, so the sentences can be tested from the shapes a real card
// produces rather than from a screenshot.
//
// A row speaks about ONE slot. It names the operation and then the one thing
// that operation was about — a rename says the name, a tempo says the tempo,
// a trim says the length. Rows do not list every field that moved: a swap
// moves all of them at once, and a reader drowning in six changes learns
// nothing. What the slot held is a separate line, because it answers a
// separate question: is there anything here to listen to or put back.
//==============================================================================
namespace loopercat::history::story
{

// The audio a row can offer, and what the tab may do with it.
enum class Take {
    none,      // the slot held nothing at this point
    kept,      // the bytes are in the store: they can be played and restored
    onCard,    // the take is the one the slot holds now; the player has it
    lost       // a take was here, and its bytes are not kept — say so plainly
};

// What the store holds about one slot in one operation. Absent bodies mean
// the operation changed no field of this slot (normalize and downmix rewrite
// audio alone); `note` is the line the job wrote about itself, which for such
// an operation is the only thing there is to say.
struct Facts {
    std::string kind;
    std::optional<std::string> beforeBody;
    std::optional<std::string> afterBody;
    std::optional<int> swappedWith;
    std::string takeName; // the take the slot held after the operation
    Take take = Take::none;
    std::string note;
};

struct Line {
    std::string action; // "Trimmed", "Swapped with slot 43"
    std::string detail; // "4:36 -> 0:39", "Memory42 -> TEST_42_HIST", or empty
    std::string audio;  // "0:39 kept", "nothing kept", or empty
};

// Seconds are cut, not rounded — the slot table and the player already show
// a 276.5 s loop as 4:36, and a history that called the same loop 4:37 would
// be the app disagreeing with itself in front of the player.
inline std::string minutes(std::int64_t frames)
{
    const std::int64_t seconds = frames / 44100;
    return std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "")
        + std::to_string(seconds % 60);
}

inline std::string bpm(long long tenths)
{
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " BPM";
}

// Every number comes from the section that owns it, as catalog::readSlot
// reads them: the loop's facts from TRACK1, the playback tempo from MASTER.
inline long long track(const std::string& body, std::string_view tag)
{
    return rc0::sectionField(body, rc0::kSectionTrack1, tag);
}

inline long long tempoTenths(const std::string& body)
{
    return rc0::sectionField(body, rc0::kSectionMaster, "Tempo");
}

inline std::string trimmedName(const std::string& body)
{
    std::string name = rc0::decodeName(body);
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    return name;
}

inline Line tell(const Facts& facts)
{
    Line line;
    const bool bodies = facts.beforeBody && facts.afterBody;
    const auto both = [&facts](std::string_view tag) {
        return std::pair { track(*facts.beforeBody, tag), track(*facts.afterBody, tag) };
    };

    if (facts.kind == "push") {
        line.action = "Pushed";
        line.detail = facts.takeName;
        if (bodies) {
            const auto [was, now] = both("WavLen");
            (void) was;
            line.detail += " - " + minutes(now) + " - " + bpm(tempoTenths(*facts.afterBody));
        }
    } else if (facts.kind == "trim") {
        line.action = "Trimmed";
        if (bodies) {
            const auto [was, now] = both("WavLen");
            line.detail = minutes(was) + " -> " + minutes(now);
        }
    } else if (facts.kind == "rename") {
        line.action = "Renamed";
        if (bodies)
            line.detail = trimmedName(*facts.beforeBody) + " -> " + trimmedName(*facts.afterBody);
    } else if (facts.kind == "tempo") {
        line.action = "Tempo";
        if (bodies)
            line.detail = bpm(tempoTenths(*facts.beforeBody)) + " -> "
                + bpm(tempoTenths(*facts.afterBody));
    } else if (facts.kind == "oneshot") {
        line.action = std::string("One Shot ")
            + (bodies && track(*facts.afterBody, "One") == 1 ? "on" : "off");
    } else if (facts.kind == "countin") {
        line.action = std::string("Play Count-In ")
            + (bodies && usecases::countin::isOn(*facts.afterBody) ? "on" : "off");
    } else if (facts.kind == "swap") {
        line.action = "Swapped";
        if (facts.swappedWith)
            line.action += " with slot " + std::to_string(*facts.swappedWith);
    } else if (facts.kind == "clear") {
        line.action = "Cleared";
    } else if (facts.kind == "restore") {
        line.action = "Restored";
        line.detail = facts.note;
    } else if (facts.kind == "undo" || facts.kind == "redo") {
        // The row's note names what was undone ("trim"); its own bodies say
        // where the slot went back to — the length, when it has one.
        line.action = std::string(facts.kind == "undo" ? "Undid" : "Redid")
            + (facts.note.empty() ? "" : " " + facts.note);
        if (bodies) {
            const auto [was, now] = both("WavLen");
            (void) was;
            line.detail = "back to " + (now > 0 ? minutes(now) : std::string("empty"));
        }
    } else if (facts.kind == "downmix") {
        line.action = "Folded to mono";
        line.detail = facts.note;
    } else if (facts.kind == "normalize") {
        line.action = "Normalized";
        line.detail = facts.note;
    } else {
        // An operation this build has no words for — a newer LooperCat wrote
        // the row, or the legacy import did. Its own name is better than a
        // guess, and better than an empty line.
        line.action = facts.kind;
        line.detail = facts.note;
    }

    switch (facts.take) {
        case Take::none:
            line.audio = facts.kind == "clear" ? "nothing left in the slot" : "";
            break;
        case Take::kept: line.audio = "take kept"; break;
        case Take::onCard: line.audio = "in the slot now"; break;
        case Take::lost: line.audio = "take no longer kept"; break;
    }
    return line;
}

} // namespace loopercat::history::story
