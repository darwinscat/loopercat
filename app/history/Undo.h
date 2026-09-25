// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"

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
// closes the way back like a step forward. A legacy row has no state to come
// back from and is never a target. Cmd-Z targets the newest live operation;
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
        if (e.status == "interrupted") {
            undone.clear(); // the card may have changed: no way back past here
            continue;
        }
        if (e.status != "done" || e.actor == "legacy")
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
    nothingToPutBack, // a legacy row, or an operation that touched no slot
    stateNotRecorded, // an audio-changing operation whose take before it is unknown
    takeNotKept       // the take the slot held before is no longer in the store
};

// What one slot goes back to. The body absent: it did not change, keep it.
// `keepTake`: the audio did not change, leave the file alone. Otherwise the
// take named here goes back — or, with none named, the slot goes empty.
struct Step {
    int slot = 0;
    std::optional<std::string> body;
    bool keepTake = false;
    std::optional<std::string> takeName;
    std::optional<std::string> takeHash;
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
    std::vector<std::int64_t> writesOver;     // finished operations after the target on the same slots
    bool crossesConnection = false;           // the target is from another session than the newest operation
    bool crossesPedal = false;                // a change made on the pedal itself lies after the target

    bool possible() const { return refusal == Refusal::none; }
};

// Whether a slot's body says it held a take: empty only when both the
// pedal's own flag and the length say so (Rc0.hpp, the rule #83 restores
// by). Anything else is read as held — the direction that never removes.
inline bool bodyHoldsTake(const std::string& body)
{
    return rc0::sectionField(body, rc0::kSectionTrack1, "WavStat") != rc0::kWavStatNone
        || rc0::sectionField(body, rc0::kSectionTrack1, "WavLen") != 0;
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
    if (entry.actor == "legacy")
        return refuse(Refusal::nothingToPutBack, "a row from before the history has no state to go back to");
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

    if (entry.kind == "swap" && entry.slots.size() == 2) {
        out.swapBack = std::make_pair(entry.slots[0].slot, entry.slots[1].slot);
    } else {
        for (const HistoryStore::CardEntry::Slot& touched : entry.slots) {
            Step step;
            step.slot = touched.slot;
            step.body = touched.facts.beforeBody;
            if (touched.archived) {
                // The take this operation replaced or removed — exactly what was there.
                if (!touched.archived->kept)
                    return refuse(Refusal::takeNotKept,
                                  "the take slot " + std::to_string(touched.slot)
                                      + " held before is no longer kept",
                                  touched.slot);
                step.takeName = touched.archived->name;
                step.takeHash = touched.archived->hash;
            } else if (touched.facts.beforeBody) {
                // Nothing archived means nothing replaced: a take the body
                // names is still on the card and stays. A body that says
                // empty lets the slot go back to empty — a take arrived into
                // nothing, and leaves with the undo.
                step.keepTake = bodyHoldsTake(*touched.facts.beforeBody);
            } else {
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
