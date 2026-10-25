// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"
#include "RecordedState.h"

#include <loopercat/Rc0.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

//==============================================================================
// loopercat::history::undo — Undo and Redo over a timeline that is only ever
// appended to (#73).
//
// An undo is a new row, never a stack pop: the bytes really were written to
// the pedal, and a history that pretended otherwise would be lying about a
// write that happened. Redo is the undo of that undo, and it is a row too.
// So the timeline holds rows of kind 'undo' and 'redo', each naming the
// operation it reverts (ops.reverts), and what Cmd-Z and Cmd-Shift-Z target
// is read back from those rows by a cursor — never remembered anywhere else.
//
// The cursor: a finished operation of the app's or the pedal's becomes live
// and closes the way back (a step forward after an undo is a new branch, as
// in any editor); a finished 'undo' row moves the operation it reverts from
// live to undone and is itself transparent; a finished 'redo' row moves the
// operation its target had undone back to live. A row that never finished
// is neither, and one that was cut off may have changed the card, so it
// closes the way back like a step forward. Cmd-Z targets the newest live operation;
// Cmd-Shift-Z targets the newest undo row still standing — reverting it is
// the redo.
//
// The plan for reverting an operation is what its rows say the slots held
// before it — the body it recorded and the take it archived — and, for the
// pedal's own settings, the text each section had (system_changes); it is
// refused, by name, when any of that is missing: a take no longer kept, a
// state never recorded, an operation that did not finish. Whether a slot
// held a take before is read from the body it recorded, never guessed from
// the operation's kind: an operation that archived nothing replaced
// nothing, so the take the body names is still on the card and stays
// there; only a body that says empty lets the slot go back to empty. A
// kind this build does not know can therefore never make undo remove a
// take. Crossing a connection, or a change made on the pedal
// itself, is allowed but flagged, so the first such press can show what it
// is about to write over — a speed bump, not a wall. Until slice 1b brings
// the card marker, "another connection" is read as "another session".
//==============================================================================
namespace loopercat::history::undo
{

using Entry = HistoryStore::OpSummary;
using Targets = HistoryStore::UndoTargets;

inline Targets cursor(const std::vector<Entry>& entries)
{
    std::vector<std::int64_t> live;
    std::vector<std::pair<std::int64_t, std::int64_t>> undone; // {operation, the undo row that took it}
    for (const Entry& e : entries) {
        if (e.kind == "snapshot" || e.kind == "forget-history")
            continue; // a baseline observed the card; it did not change it
        if (e.status == "interrupted") {
            undone.clear(); // the card may have changed: no way back past here
            continue;
        }
        if (e.status != "done")
            continue;
        if (e.kind == "undo") {
            if (!e.reverts)
                continue;
            const auto found = std::find(live.rbegin(), live.rend(), *e.reverts);
            if (found == live.rend())
                continue; // reverts nothing that was live: the row stands, the cursor ignores it
            live.erase(std::next(found).base());
            undone.emplace_back(*e.reverts, e.op);
        } else if (e.kind == "redo") {
            if (!e.reverts)
                continue;
            const auto found = std::find_if(undone.rbegin(), undone.rend(),
                                            [&](const auto& u) { return u.second == *e.reverts; });
            if (found == undone.rend())
                continue;
            live.push_back(found->first);
            undone.erase(std::next(found).base());
        } else {
            live.push_back(e.op);
            undone.clear();
        }
    }
    Targets out;
    if (!live.empty())
        out.undo = live.back();
    if (!undone.empty()) {
        out.redo = undone.back().second;
        out.redoRestores = undone.back().first;
    }
    return out;
}

enum class Refusal {
    none,
    noSuchOperation,
    notFinished,
    nothingToPutBack, // an operation that changed no slot or setting
    stateNotRecorded, // an audio-changing operation whose take before it is unknown
    takeNotKept       // the take the slot held before is no longer in the store
};

// A take that goes back onto one track of a slot, out of the store.
struct TakeBack {
    int track = 0;
    std::string name;
    std::string hash;
};

// What one slot goes back to. The body absent: it did not change, keep it.
// A track named in `takes` gets that take back; one in `emptied` goes empty
// — a take arrived into nothing, and leaves with the undo; every other track
// keeps the take it holds, because the operation did not change its audio.
struct Step {
    int slot = 0;
    std::optional<std::string> body;
    std::vector<TakeBack> takes; // ascending by track
    std::vector<int> emptied;    // ascending
    // The audio of no track goes back: the body alone, if anything, and the
    // files are left alone.
    bool keepsAudio() const { return takes.empty() && emptied.empty(); }
};

// One section of the pedal's own settings goes back to the text it had.
struct SystemStep {
    std::string section;
    std::string before;
};

struct Plan {
    std::int64_t target = 0;
    Refusal refusal = Refusal::none;
    std::string reason; // for the dialog, when refused
    int refusedSlot = 0;
    std::vector<Step> steps;                  // ascending by slot
    std::vector<SystemStep> system;           // the settings' sections, in section order
    std::optional<std::pair<int, int>> swapBack; // a swap goes back by swapping again: no bytes needed
    // Finished operations after the target on the same slots — AS RECORDED:
    // undo and redo rows and operations already undone are in it too. What is
    // still in effect, the list a warning may rest on, is
    // UndoRun.h's stillInEffect(); to move here after #73 lands.
    std::vector<std::int64_t> writesOver;
    // The target is from another session than the newest operation — a
    // proxy that misses the first press of a new run, before it has written
    // anything. The warning rests on the recorder's current session instead:
    // UndoRun.h bumpFor(), with HistoryRecorder::sessionOn().
    bool crossesConnection = false;
    bool crossesPedal = false;                // a change made on the pedal itself lies after the target

    bool possible() const { return refusal == Refusal::none; }
};

// Whether a slot's body says one of its tracks held a take: empty only when
// both the pedal's own flag and the length in that track's section say so
// (Rc0.hpp, the rule #83 restores by). Anything else is read as held — the
// direction that never removes.
inline bool bodyHoldsTake(const std::string& body, int track)
{
    const std::string section = rc0::trackSectionName(track);
    return rc0::sectionField(body, section, "WavStat") != rc0::kWavStatNone
        || rc0::sectionField(body, section, "WavLen") != 0;
}


inline Plan plan(const std::vector<HistoryStore::CardEntry>& timeline, std::int64_t target)
{
    Plan out;
    out.target = target;
    const auto refuse = [&out](Refusal why, std::string reason, int slot = 0) {
        out.refusal = why;
        out.reason = std::move(reason);
        out.refusedSlot = slot;
        out.steps.clear(); // a swap's plan is set only once every refusal has passed
        out.system.clear();
        return out;
    };

    std::size_t index = timeline.size();
    for (std::size_t i = 0; i < timeline.size(); ++i)
        if (timeline[i].op == target)
            index = i;
    if (index == timeline.size())
        return refuse(Refusal::noSuchOperation, "that operation is not in the history");
    const HistoryStore::CardEntry& entry = timeline[index];
    if (entry.status != "done")
        return refuse(Refusal::notFinished, "that operation did not finish");
    if (entry.slots.empty() && entry.system.empty())
        return refuse(Refusal::nothingToPutBack, "that operation changed no slot and no setting");

    // The pedal's own settings: each section goes back to the text it had.
    // A row that is not a section's text — nothing before it, or another
    // tag's — was not recorded, whatever it says.
    for (const HistoryStore::SystemChange& change : entry.system) {
        const std::string open = "<" + change.section + ">";
        const std::string close = "</" + change.section + ">";
        const bool sound = change.before.size() >= open.size() + close.size()
            && change.before.compare(0, open.size(), open) == 0
            && change.before.compare(change.before.size() - close.size(), close.size(), close) == 0;
        if (change.section.empty() || !sound)
            return refuse(Refusal::stateNotRecorded,
                          "what the pedal's <" + change.section
                              + "> settings were before that operation was not recorded");
        out.system.push_back({ change.section, change.before });
    }

    // An undo or redo of a swap moved the audio the way the swap did — by
    // exchanging the folders, archiving nothing — so it goes back the same
    // way, at any depth of undo on undo. Per-slot steps would put each
    // slot's old body beside the other slot's take.
    const auto revertsASwap = [&timeline](const HistoryStore::CardEntry& row) {
        const HistoryStore::CardEntry* at = &row;
        for (std::size_t hops = 0; hops <= timeline.size(); ++hops) {
            if (at->kind == "swap")
                return true;
            if ((at->kind != "undo" && at->kind != "redo") || !at->reverts)
                return false;
            const auto next = std::find_if(timeline.begin(), timeline.end(),
                                           [&](const auto& e) { return e.op == *at->reverts; });
            if (next == timeline.end())
                return false;
            at = &*next;
        }
        return false; // a cycle in reverts is no swap
    };
    if (entry.slots.size() == 2 && revertsASwap(entry)) {
        out.swapBack = std::make_pair(entry.slots[0].slot, entry.slots[1].slot);
    } else {
        for (const HistoryStore::CardEntry::Slot& touched : entry.slots) {
            Step step;
            step.slot = touched.slot;
            step.body = touched.facts.beforeBody;
            // The takes this operation replaced or removed — exactly what was
            // there, track by track.
            for (const HistoryStore::SlotTake& archived : touched.archived) {
                const std::string onTrack = archived.track != 1 || touched.archived.size() > 1
                    ? " on track " + std::to_string(archived.track)
                    : std::string();
                if (!archived.kept || !archived.hash)
                    return refuse(Refusal::takeNotKept,
                                  "the take slot " + std::to_string(touched.slot) + " held before"
                                      + onTrack + " is no longer kept",
                                  touched.slot);
                step.takes.push_back({ archived.track, archived.name, *archived.hash });
            }
            const auto archivedOn = [&touched](int track) {
                return std::any_of(touched.archived.begin(), touched.archived.end(),
                                   [track](const HistoryStore::SlotTake& t) { return t.track == track; });
            };
            if (touched.facts.beforeBody) {
                // Nothing archived on a track means nothing replaced there: a
                // take the body names is still on the card and stays. A body
                // that says the track was empty lets it go back to empty — a
                // take arrived into nothing, and leaves with the undo.
                const std::string& before = *touched.facts.beforeBody;
                for (int track = 1; track <= trackSectionsIn(before); ++track)
                    if (!archivedOn(track) && !bodyHoldsTake(before, track))
                        step.emptied.push_back(track);
            } else if (touched.archived.empty()) {
                return refuse(Refusal::stateNotRecorded,
                              "what slot " + std::to_string(touched.slot)
                                  + " held before that operation was not recorded",
                              touched.slot);
            }
            out.steps.push_back(std::move(step));
        }
    }

    // What the press writes over, and what it crosses.
    const auto touchesMine = [&entry](const HistoryStore::CardEntry& later) {
        return std::any_of(later.slots.begin(), later.slots.end(), [&entry](const auto& touched) {
            return std::any_of(entry.slots.begin(), entry.slots.end(),
                               [&touched](const auto& mine) { return mine.slot == touched.slot; });
        });
    };
    std::int64_t newestSession = entry.session;
    for (std::size_t i = index + 1; i < timeline.size(); ++i) {
        const HistoryStore::CardEntry& later = timeline[i];
        if (later.status != "done")
            continue;
        newestSession = later.session;
        if (later.actor == "pedal")
            out.crossesPedal = true;
        if (touchesMine(later))
            out.writesOver.push_back(later.op);
    }
    out.crossesConnection = newestSession != entry.session;
    return out;
}

} // namespace loopercat::history::undo
