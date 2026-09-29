// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryRecorder.h"
#include "HistoryStore.h"
#include "SlotRows.h"
#include "Undo.h"

#include <loopercat/Commands.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/SystemFile.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

//==============================================================================
// loopercat::history::undo — carrying out Undo and Redo (#73): the words the
// Edit menu wears, the speed bump's reasons, and the run itself.
//
// Undo.h decides WHAT a revert would put back; this carries it out on the
// card, as one operation of kind 'undo' or 'redo' that names its target
// (ops.reverts) before the first byte is written. Everything the run does
// goes through the core's own primitives — restore for a slot, swap for a
// swap, the settings pair for a section — so every write is recorded in the
// history and verified exactly as any other operation's,
// and the undo can be undone in turn.
//
// The press and the run are apart in time: the plan the dialog showed was
// read before the player answered, and a job may have landed in between. So
// the run asks the store again, and refuses — before it begins, with nothing
// written — when the target it was sent for is no longer the one on offer.
//
// A slot whose audio the operation did not change gets its body back and
// keeps its file: the take is not read, archived or rewritten, so undoing a
// rename of a two-minute loop costs a memory write, not a card's worth of
// audio. What guards that path is the rule restore guards by: the body's
// audio fields have to describe the take in the slot. They are compared with
// the body the slot holds now, which describes the take in the slot now; a
// slot whose take changed since is refused by name, and nothing is written.
//==============================================================================
namespace loopercat::history::undo
{

namespace fs = std::filesystem;

// --- words ---

// An operation, as the Edit menu names what it undoes: "trim of slot 14",
// "swap of slots 3 and 7", "controls change of the pedal settings". A kind
// this build has no words for is named as recorded — never guessed.
inline std::string kindWords(const std::string& kind)
{
    static const std::map<std::string, std::string> words {
        { "rename", "rename" },
        { "tempo", "tempo change" },
        { "swap", "swap" },
        { "trim", "trim" },
        { "restore", "restore" },
        { "oneshot", "One Shot change" },
        { "countin", "Play Count-In change" },
        { "rhythm", "rhythm change" },
        { "push", "push" },
        { "downmix", "downmix" },
        { "normalize", "normalize" },
        { "clear", "clear" },
        { "undo", "undo" },
        { "redo", "redo" },
        { "controls", "controls change" },
    };
    const auto found = words.find(kind);
    return found != words.end() ? found->second : kind;
}

inline std::string operationWords(const HistoryStore::CardEntry& entry)
{
    std::string words = kindWords(entry.kind);
    if (entry.slots.size() == 1)
        words += " of slot " + std::to_string(entry.slots[0].slot);
    else if (entry.slots.size() == 2)
        words += " of slots " + std::to_string(entry.slots[0].slot) + " and "
            + std::to_string(entry.slots[1].slot);
    else if (entry.slots.size() > 2)
        words += " of " + std::to_string(entry.slots.size()) + " slots";
    if (!entry.system.empty())
        words += entry.slots.empty() ? " of the pedal settings" : " and the pedal settings";
    return words;
}

inline const HistoryStore::CardEntry* findEntry(const std::vector<HistoryStore::CardEntry>& timeline,
                                                std::int64_t op)
{
    for (const auto& entry : timeline)
        if (entry.op == op)
            return &entry;
    return nullptr;
}

// What Edit -> Undo and Edit -> Redo would do right now, in words. `redo`
// is the undo row that reverting brings its operation back; the words are
// that operation's.
struct Offer {
    std::optional<std::int64_t> undo;
    std::optional<std::int64_t> redo;
    std::string undoWords; // "trim of slot 14"
    std::string redoWords;
};

inline Offer offerFrom(const Targets& targets, const std::vector<HistoryStore::CardEntry>& timeline)
{
    Offer out;
    out.undo = targets.undo;
    out.redo = targets.redo;
    if (targets.undo)
        if (const auto* entry = findEntry(timeline, *targets.undo))
            out.undoWords = operationWords(*entry);
    if (targets.redoRestores)
        if (const auto* entry = findEntry(timeline, *targets.redoRestores))
            out.redoWords = operationWords(*entry);
    return out;
}

inline Offer offer(HistoryStore& store)
{
    return offerFrom(store.offeredTargets(), store.cardTimeline());
}

// The menu item: "Undo trim of slot 14", or plain "Undo" with nothing to undo.
inline std::string menuText(bool redo, const Offer& offer)
{
    const std::string& words = redo ? offer.redoWords : offer.undoWords;
    return std::string(redo ? "Redo" : "Undo") + (words.empty() ? "" : " " + words);
}

// --- the speed bump ---

// Of these later operations, the ones whose effect is still on the card:
// an undo or redo row is the timeline moving back and forth, not a change of
// its own, and an operation an undo took back — and no redo brought back —
// is no longer there to be written over. Plan::writesOver lists every
// finished later operation on the same slots; without this the bump would
// stand in front of every second Cmd-Z, for the undo row the first one wrote.
inline std::vector<std::int64_t> stillInEffect(const std::vector<HistoryStore::CardEntry>& timeline,
                                               const std::vector<std::int64_t>& ops)
{
    std::set<std::int64_t> undone;
    std::map<std::int64_t, std::int64_t> undoneBy; // undo row -> the operation it took back
    for (const auto& entry : timeline) {
        if (entry.status != "done" || !entry.reverts)
            continue;
        if (entry.kind == "undo") {
            undone.insert(*entry.reverts);
            undoneBy[entry.op] = *entry.reverts;
        } else if (entry.kind == "redo") {
            const auto back = undoneBy.find(*entry.reverts);
            if (back != undoneBy.end())
                undone.erase(back->second);
        }
    }
    std::vector<std::int64_t> out;
    for (const std::int64_t op : ops) {
        const auto* entry = findEntry(timeline, op);
        if (entry == nullptr || entry->kind == "undo" || entry->kind == "redo" || undone.count(op))
            continue;
        out.push_back(op);
    }
    return out;
}

// Why a press deserves a word before it writes, one reason per line, each
// with a key: the same crossing is asked about once a session, and a new
// one is asked about again.
struct Bump {
    std::vector<std::string> keys;
    std::vector<std::string> reasons;
    bool needed() const { return !keys.empty(); }
};

inline std::string rowWords(const HistoryStore::CardEntry& entry)
{
    const auto rows = rows::forCard({ entry });
    std::string words = rows.empty() ? kindWords(entry.kind) : rows.front().action;
    const auto slots = rows.empty() ? std::vector<int> {} : rows.front().slots();
    if (slots.size() == 1)
        words += " (slot " + std::to_string(slots[0]) + ")";
    return words;
}

// `current` is the session this run records in on the card (the recorder's
// sessionOn): a target from any other session lies across a connection, and
// with no session yet — the first press after the app started — every target
// does. Plan::crossesConnection compares with the newest operation's session
// instead, and so misses exactly that first press.
inline bool crossesConnection(const HistoryStore::CardEntry& target, std::optional<std::int64_t> current)
{
    return !current || target.session != *current;
}

inline Bump bumpFor(const Plan& plan, const std::vector<HistoryStore::CardEntry>& timeline,
                    std::optional<std::int64_t> current)
{
    Bump out;
    const auto* target = findEntry(timeline, plan.target);
    if (target == nullptr || !plan.possible())
        return out;
    if (crossesConnection(*target, current)) {
        // Keyed by the connection the press goes back INTO: once a player has
        // said yes to reaching into it, the next press there — after the
        // first one wrote its own row in this session — is the same crossing.
        out.keys.push_back("connection:" + std::to_string(target->session));
        out.reasons.push_back("It goes back past another connection of the pedal.");
    }
    if (plan.crossesPedal) {
        bool after = false;
        for (const auto& entry : timeline) {
            if (entry.op == target->op) {
                after = true;
                continue;
            }
            if (after && entry.status == "done" && entry.actor == "pedal") {
                out.keys.push_back("pedal:" + std::to_string(entry.op));
                out.reasons.push_back("It goes back past a change made on the pedal itself: "
                                      + rowWords(entry) + ".");
            }
        }
    }
    for (const std::int64_t op : stillInEffect(timeline, plan.writesOver)) {
        out.keys.push_back("over:" + std::to_string(op));
        out.reasons.push_back("It writes over: " + rowWords(*findEntry(timeline, op)) + ".");
    }
    return out;
}

// The plan in words, for the --undo-plan seam: what would go back where,
// what it crosses, and why it would be refused.
inline std::string describe(const Plan& plan, const std::vector<HistoryStore::CardEntry>& timeline,
                            std::optional<std::int64_t> current)
{
    std::string out;
    const auto* target = findEntry(timeline, plan.target);
    out += "target: op " + std::to_string(plan.target)
        + (target != nullptr ? " - " + operationWords(*target) : std::string()) + "\n";
    if (!plan.possible())
        return out + "refused: " + plan.reason + "\n";
    if (plan.swapBack)
        out += "  swap back: slots " + std::to_string(plan.swapBack->first) + " and "
            + std::to_string(plan.swapBack->second) + "\n";
    for (const Step& step : plan.steps) {
        out += "  slot " + std::to_string(step.slot) + ": "
            + (step.body ? "body back" : "body as it is");
        if (step.keepTake)
            out += ", its take stays";
        else if (step.takeName)
            out += ", take \"" + *step.takeName + "\" back from the history";
        else
            out += ", the slot goes empty";
        out += "\n";
    }
    for (const SystemStep& step : plan.system)
        out += "  settings: <" + step.section + "> back\n";
    const Bump bump = bumpFor(plan, timeline, current);
    out += std::string("crosses a connection: ")
        + (target != nullptr && crossesConnection(*target, current) ? "yes" : "no")
        + (current ? "" : " (no session in this run yet)") + "\n";
    out += std::string("crosses a change on the pedal: ") + (plan.crossesPedal ? "yes" : "no") + "\n";
    out += "writes over (recorded): " + std::to_string(plan.writesOver.size()) + ", still in effect: "
        + std::to_string(stillInEffect(timeline, plan.writesOver).size()) + "\n";
    for (const std::string& reason : bump.reasons)
        out += "  asks first: " + reason + "\n";
    return out;
}

// --- the run ---

// One section of a settings document replaced by the text it had — tags and
// all. Every other byte of the file, the trailer included, is reproduced.
inline std::string spliceSection(std::string_view text, const std::string& section,
                                 const std::string& sectionText)
{
    const auto region = rc0::detail::sectionRegion(text, section); // throws for a missing section
    const std::string open = "<" + section + ">";
    const std::string close = "</" + section + ">";
    if (sectionText.size() < open.size() + close.size()
        || sectionText.compare(0, open.size(), open) != 0
        || sectionText.compare(sectionText.size() - close.size(), close.size(), close) != 0)
        throw Error("the recorded <" + section + "> settings are not that section's text");
    const std::size_t from = region.bodyStart - open.size();
    const std::size_t to = region.bodyEnd + close.size();
    return std::string(text.substr(0, from)) + sectionText + std::string(text.substr(to));
}

// A slot's body back beside the take it holds now — the operation did not
// change its audio. Refused when the body does not describe that take.
inline void putBodyBack(const fs::path& volume, int slot, const std::string& body,
                        const commands::WriteOptions& options)
{
    const std::string document = commands::readMemoryFor(volume, profile::Operation::restore);
    const std::string now = rc0::slotBody(document, slot);
    for (const char* field : { "WavStat", "WavLen" })
        if (rc0::sectionField(body, rc0::kSectionTrack1, field)
            != rc0::sectionField(now, rc0::kSectionTrack1, field))
            throw Error("slot " + std::to_string(slot)
                        + " holds another take than it did then (" + field
                        + " differs) — its settings would not match its audio, so nothing was"
                          " put back");
    commands::writeMemoryPair(volume, rc0::replaceSlotBody(document, slot, body), options);
}

// What a plan puts back, carried out on the card. The store is read for the
// takes the plan names; nothing else is decided here.
inline void apply(HistoryStore& store, const Plan& plan, const fs::path& volume,
                  const commands::WriteOptions& options)
{
    if (!plan.possible())
        throw Error(plan.reason);
    if (plan.swapBack)
        commands::swap(volume, plan.swapBack->first, plan.swapBack->second, options);
    for (const Step& step : plan.steps) {
        if (step.keepTake) {
            if (step.body)
                putBodyBack(volume, step.slot, *step.body, options);
            continue; // nothing changed in this slot that needs putting back
        }
        commands::SlotState state;
        state.body = step.body
            ? *step.body
            : rc0::slotBody(commands::readMemoryFor(volume, profile::Operation::restore), step.slot);
        if (step.takeName) {
            if (!step.takeHash)
                throw Error("the take slot " + std::to_string(step.slot)
                            + " held before has no recorded content");
            std::optional<std::string> bytes = store.takeBytes(*step.takeHash);
            if (!bytes)
                throw Error("the take slot " + std::to_string(step.slot)
                            + " held before is no longer kept");
            state.take = commands::Take { *step.takeName, std::move(*bytes) };
        }
        commands::restore(volume, step.slot, state, options);
    }
    if (!plan.system.empty()) {
        const std::string before = commands::readSystem(volume);
        std::string text = before;
        for (const SystemStep& step : plan.system)
            text = spliceSection(text, step.section, step.before);
        if (text != before)
            commands::writeSystemPair(volume, text, options);
    }
}

// The press, checked against the store as it is now: the target must still
// be the one on offer, and the plan for it possible. Returns the plan and the
// words for the operation's note ("trim", for "Undid trim").
struct Checked {
    Plan plan;
    std::string note;
};

inline Checked check(HistoryStore& store, bool redo, std::int64_t target)
{
    const Targets targets = store.offeredTargets();
    const std::optional<std::int64_t> offered = redo ? targets.redo : targets.undo;
    if (!offered || *offered != target)
        throw Error(std::string("the history moved on since the press — nothing was written;"
                                " press ")
                    + (redo ? "Redo" : "Undo") + " again");
    const auto timeline = store.cardTimeline();
    Checked out { plan(timeline, target), {} };
    if (!out.plan.possible())
        throw Error(out.plan.reason);
    const std::optional<std::int64_t> named = redo ? targets.redoRestores : targets.undo;
    if (named)
        if (const auto* entry = findEntry(timeline, *named))
            out.note = kindWords(entry->kind);
    return out;
}

// The first half of a press, run where the operation opens (the worker job's
// `before`): checked against the store as it is now — a refusal here begins
// nothing and writes nothing — then the operation is opened and names its
// target before the card is touched. The second half is apply(); the job's
// `after` closes the operation with Checked::note.
inline Checked beginPress(HistoryRecorder& recorder, const std::string& opId, bool redo,
                          std::int64_t target, const fs::path& volume)
{
    recorder.selectVolume(volume);
    Checked checked = check(recorder.store(), redo, target);
    recorder.begin(opId, redo ? "redo" : "undo", volume);
    recorder.reverts(opId, target);
    return checked;
}

} // namespace loopercat::history::undo
