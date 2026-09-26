// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Retention.h"
#include "Sqlite.h"

#include <loopercat/Commands.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//==============================================================================
// loopercat::history::HistoryStore — the operation journal and the archive of
// takes, in one SQLite file (issue #72; the tables, and why one file with a
// rollback journal: Schema.h).
//
// The store records; it does not decide. The core reports what a command did
// (commands::Archive, commands::Journal) and the recorder hands that here, one
// transaction per fact:
//
//   beginOp        the op exists, pending, before the command touches the card
//   keepAudio      a take about to be replaced: its bytes and the row naming
//                  them commit together, or neither does
//   recordBodies   the slot bodies the write is about to change, before/after
//   recordLanded   a take that landed: name, size and hash — not the bytes,
//                  which are on the card and are archived when replaced
//   finishOp       done or failed
//
// Opening the store turns every op still pending into `interrupted`: the app
// stopped between two of those steps, and the history says so rather than
// pretending the op finished or never began.
//
// One connection, used from one thread at a time (the pedal worker).
//==============================================================================
namespace loopercat::history
{

enum class OpStatus { done, failed };

class HistoryStore
{
public:
    // <dir>/history.db. Creates it when absent, verifies every storage
    // property it relies on, and refuses a store it cannot read correctly
    // rather than reading it on a guess.
    explicit HistoryStore(const std::filesystem::path& dir);

    // SHA-256 of the bytes, raw (32 bytes): the audio store's key.
    static std::string contentHash(std::string_view bytes);

    // --- where and when ---
    std::int64_t card(const std::string& model, const std::string& label, std::int64_t nowMs);
    std::int64_t openSession(std::int64_t card, std::int64_t nowMs);
    void closeSession(std::int64_t session, std::int64_t nowMs);

    // --- one operation ---
    std::int64_t beginOp(std::int64_t session, const std::string& opId, const std::string& kind,
                         std::int64_t atMs);
    void keepAudio(std::int64_t op, int slot, int track, const std::string& name,
                   std::string_view bytes, std::int64_t nowMs);
    void recordBodies(std::int64_t op, const std::vector<commands::SlotChange>& changes);
    void recordLanded(std::int64_t op, int slot, int track, const std::string& name,
                      std::string_view bytes);
    void finishOp(std::int64_t op, OpStatus status, const std::string& note);

    // The audio a slot holds AFTER an operation — the rows that make each
    // slot's timeline readable on its own. `hash` is absent when the store
    // has never seen those bytes: the file is then known by name and size,
    // and no take can be fetched for it.
    void recordPresentAudio(std::int64_t op, int slot, int track, const std::string& name,
                            std::int64_t size, const std::optional<std::string>& hash);

    // One row of a slot's timeline, with everything the tab needs to write a
    // sentence and to know what it may offer for it.
    struct TimelineEntry {
        std::int64_t op = 0;
        std::int64_t at = 0; // wall clock, the order a player reads
        std::string kind;
        std::string actor;
        std::string status;
        std::string note;
        std::optional<std::string> beforeBody;
        std::optional<std::string> afterBody;
        std::optional<int> swappedWith; // the slot a swap exchanged with
        // The take this row's state holds. A legacy row has no state of its
        // own, so it offers the take it archived instead — that is the only
        // take it knows about.
        std::string takeName;
        std::optional<std::string> takeHash;
        bool takeKept = false; // the bytes are in the store: it can be played
    };

    // Ordered by time, not by insertion: rows imported from the folders that
    // predate the store are written last and belong first (#72).
    std::vector<TimelineEntry> slotTimeline(int slot);

    // --- reads: what the tests look at today, and what #50 builds on ---
    std::optional<std::string> takeBytes(const std::string& hash);
    std::string opStatus(std::int64_t op);
    // Every slot an operation touched, by its body or its audio.
    std::vector<int> touchedSlots(std::int64_t op);
    bool hasAfterAudio(std::int64_t op, int slot);
    // The hash a slot's last recorded state gives a file of this name and
    // size, looking only before `op`. Absent when nothing matches — and then
    // it stays absent rather than being guessed from another file.
    std::optional<std::string> hashHeldBefore(std::int64_t op, int slot, const std::string& name,
                                              std::int64_t size);

    sqlite::Db& db() { return db_; }

    // --- legacy folders (LegacyImport.h): the store's side of the import ---

    // Which op an id names, if any: its row and who made it. The import tells
    // a folder the app already recorded from one it must adopt by this.
    struct OpIdentity {
        std::int64_t seq;
        std::string actor;
    };
    std::optional<OpIdentity> findOp(const std::string& opId);

    // An operation that ran before the history existed. Recorded by 'legacy',
    // kind 'legacy', done: the folder is there, which is all such an op can
    // say about itself. `note` names the folders it was read from.
    std::int64_t recordLegacyOp(std::int64_t session, const std::string& opId, std::int64_t atMs,
                                const std::string& note);

    // One file out of a legacy folder, in one transaction: its bytes kept once
    // (the content-addressed rule keepAudio follows), for a take the
    // slot_audio row naming it as the slot's 'before', and the legacy_files
    // row that says this path is done. `path` is the file's path under the
    // data home, '/'-separated; a path already recorded is refused by the
    // ledger's key. Returns whether the bytes were new to the store.
    bool keepLegacyTake(std::int64_t op, const std::string& path, int slot, int track,
                        const std::string& name, std::string_view bytes, std::int64_t nowMs);
    bool keepLegacyDocument(std::int64_t op, const std::string& path, std::string_view bytes,
                            std::int64_t nowMs);
    bool legacyFileImported(const std::string& path);

    // --- Undo and Redo over the timeline (Undo.h): the store's side ---

    // The ops row alone, in timeline order: what the undo cursor reads.
    struct OpSummary {
        std::int64_t op = 0;
        std::string kind;
        std::string status;
        std::string actor;
        std::optional<std::int64_t> reverts;
    };
    std::vector<OpSummary> operations();

    // What Cmd-Z and Cmd-Shift-Z would put back, read from the rows by the
    // cursor in Undo.h: `undo` is the newest live operation, `redo` the
    // newest undo row still standing — reverting it is the redo — and
    // `redoRestores` the operation that redo brings back, for its words.
    struct UndoTargets {
        std::optional<std::int64_t> undo;
        std::optional<std::int64_t> redo;
        std::optional<std::int64_t> redoRestores;
    };
    UndoTargets offeredTargets();

    // An 'undo' or 'redo' row names the operation it reverts.
    void setReverts(std::int64_t op, std::int64_t target);

    // --- what the history costs, and giving space back (Retention.h) ---

    // What the file holds, read from the file. The store reports; the policy
    // decides; a person releases. `otherBytes` is the rows, bodies and indexes
    // — what remains of the file after the takes and the pages already free.
    struct Usage {
        std::int64_t fileBytes;     // the file as it is on disk
        std::int64_t audioBytes;    // takes and documents whose bytes are kept, each once
        std::int64_t otherBytes;    // rows, bodies, indexes
        std::int64_t freeBytes;     // pages the file holds but no longer uses: vacuum() returns them
        std::int64_t diskAvailable; // on the volume the file is on
    };
    Usage usage();

    // Every blob whose bytes are kept, with what holds it: a pinned operation
    // naming it (or a pin on the blob), an operation still pending naming it,
    // or one of the cursor's targets naming it as a 'before' — the audio that
    // undo, or redo, would put back (the redo's bytes are what its undo row
    // archived). References count the rows naming the hash, both sides.
    std::vector<retention::Blob> keptBlobs(const UndoTargets& targets);

    void pinOp(std::int64_t op, bool pinned);

    // Frees the bytes of these blobs in one transaction: the bytes go, each
    // blobs_meta row is marked released at `nowMs`, and every row that named
    // the take keeps naming it — by name, size and hash — with no bytes behind
    // it. Refused whole, nothing freed, if any of them is not kept or is held
    // (see keptBlobs). Returns the bytes freed.
    std::int64_t releaseBlobs(const std::vector<std::string>& hashes, const UndoTargets& targets,
                              std::int64_t nowMs);

    // Hands up to `pages` free pages back to the system and returns how many
    // remain. Its own short transaction, a slice at a time, so a worker can
    // stay responsive between slices. The file was created with
    // auto_vacuum=INCREMENTAL for exactly this.
    std::int64_t vacuum(int pages);

    // Every take and document ever kept — when and how big, released since or
    // not — for the rate the history grows at (retention::forecast).
    std::vector<retention::Write> writes();

    // --- the whole card's timeline (the History window, #73) ---

    // One entry per operation, ordered by time — a row imported from the
    // folders that predate the store is written last and belongs first —
    // with every slot the operation touched carrying the same facts
    // slotTimeline gives that slot: one story, two views. `newest` says the
    // operation is the last FINISHED one on that slot, so its state is the
    // one the slot is in — a failed or interrupted write may never have
    // reached the card, so it is not where the slot is, and its state can be
    // offered back like any other. An operation that touched no slot (it
    // failed before the card, or kept only documents) is an entry with no
    // slots.
    // What an operation did to one section of the pedal's own settings
    // (SYSTEM*.RC0, issue #73): the section's text — <CTL>...</CTL> — as the
    // card held it before the write, and as the write left it. The section,
    // not the file: the pedal restamps the file's trailer by itself.
    struct SystemChange {
        std::string section; // sysfile::kSectionSetup / kSectionMidi / kSectionCtl
        std::string before;
        std::string after;
    };

    // A take named by a row: what an operation archived before it wrote.
    struct TakeRef {
        std::string name;
        std::string hash;
        bool kept = false; // the bytes are in the store
    };
    struct CardEntry {
        std::int64_t op = 0;
        std::int64_t at = 0;
        std::string kind;
        std::string actor;
        std::string status;
        std::string note;
        bool pinned = false;
        std::int64_t session = 0;            // the connection it happened in
        std::optional<std::int64_t> reverts; // for an 'undo' or 'redo' row: the operation it reverts
        struct Slot {
            int slot = 0;
            bool newest = false;
            TimelineEntry facts;
            // The take this operation replaced or removed in the slot — its
            // 'before' row — which is what putting the operation back needs.
            std::optional<TakeRef> archived;
        };
        std::vector<Slot> slots; // ascending by slot
        // What the operation did to the pedal's own settings, per section:
        // an operation that changed settings and no slot is an entry with
        // no slots and these.
        std::vector<SystemChange> system; // in section order: SETUP, MIDI, CTL
    };
    std::vector<CardEntry> cardTimeline();


    // --- the pedal's own settings in the history (system_changes, v4) ---

    // One row per operation and section, checked on the way in: a section
    // the file has, both texts that very section (its own tags around a
    // body), and a change that changed something. Refused otherwise.
    void recordSystemChange(std::int64_t op, const SystemChange& change);
    std::vector<SystemChange> systemChanges(std::int64_t op);

private:
    // What one operation recorded about one slot — the take it offers and,
    // for a swap, the other slot — shared by slotTimeline and cardTimeline.
    void fillSlotFacts(TimelineEntry& row, int slot);

    // The content-addressed rule, inside the caller's transaction: bytes are
    // kept once; a hash already known costs nothing; one released earlier
    // (#74) gets its bytes back. Returns whether the bytes were written.
    bool keepBlob(const std::string& hash, std::string_view bytes, std::int64_t nowMs);
    bool recordLegacyFile(std::int64_t op, const std::string& path, const char* kind,
                          std::string_view bytes, std::int64_t nowMs, const std::string& hash);
    // What holds one kept blob, for keptBlobs and for releaseBlobs' refusal.
    struct Holds {
        bool pinned;
        bool undo;
        bool inFlight;
    };
    Holds holdsOn(const std::string& hash, const UndoTargets& targets);

    std::filesystem::path file_;
    sqlite::Db db_;
};

} // namespace loopercat::history
