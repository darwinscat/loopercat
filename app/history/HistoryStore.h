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
//   recordLanded   a take that landed: name, size, hash and the file's
//                  modification time — not the bytes, which are on the card
//                  and are archived when replaced
//   finishOp       done or failed
//   recordReading  what a take's bytes measure (#140), by their hash: a fact
//                  about bytes, outside any operation
//
// Opening the store turns every op still pending into `interrupted`: the app
// stopped between two of those steps, and the history says so rather than
// pretending the op finished or never began.
//
// One connection, used from one thread at a time (the pedal worker).
//==============================================================================
namespace loopercat::history
{

enum class OpStatus { done, failed, interrupted };

class HistoryStore
{
public:
    // <dir>/history.db. Creates it when absent, verifies every storage
    // property it relies on, and refuses a store it cannot read correctly
    // rather than reading it on a guess.
    explicit HistoryStore(const std::filesystem::path& dir);

    // SHA-256 of the bytes, raw (32 bytes): the audio store's key
    // (history::contentHash, ContentHash.h — the one function, kept here by
    // name for every caller that knows the store).
    static std::string contentHash(std::string_view bytes);

    // --- where and when ---
    std::int64_t card(const std::string& markerId, const std::string& model,
                      const std::string& name, std::int64_t nowMs);
    void selectCard(std::optional<std::int64_t> card) { selectedCard_ = card; }
    std::optional<std::int64_t> selectedCard() const { return selectedCard_; }
    std::vector<int> snapshotSlots(std::int64_t op);

    // One baseline per marker. Resuming keeps the operation and committed slots.
    std::int64_t firstSeen(std::int64_t session, const std::string& opId, std::int64_t nowMs);
    // A take as the first sighting found it on the card: its bytes, and the
    // modification time its directory entry carried (ms since the epoch).
    struct SnapshotTake {
        int track;
        std::string name;
        std::string bytes;
        std::int64_t modifiedMs;
    };
    void snapshotSlot(std::int64_t op, int slot, const std::string& body,
                      const std::vector<SnapshotTake>& takes, std::int64_t nowMs);
    void snapshotFailed(std::int64_t op, int slot, const std::string& reason);
    // Opening a session selects that card for timeline and cursor queries.
    std::int64_t openSession(std::int64_t card, std::int64_t nowMs);
    void closeSession(std::int64_t session, std::int64_t nowMs);

    // --- one operation ---
    std::int64_t beginOp(std::int64_t session, const std::string& opId, const std::string& kind,
                         std::int64_t atMs);
    void keepAudio(std::int64_t op, int slot, int track, const std::string& name,
                   std::string_view bytes, std::int64_t nowMs);
    void recordBodies(std::int64_t op, const std::vector<commands::SlotChange>& changes);
    // `modifiedMs`: the landed file's modification time off the card's
    // directory entry, ms since the epoch — every row that says what a slot
    // holds carries one, except rows older than store version 9 and the
    // rows whose hash such a row vouched for by name and size (heldBefore,
    // #141). An archived take's row (keepAudio) names bytes on their way out
    // of the slot and carries none.
    void recordLanded(std::int64_t op, int slot, int track, const std::string& name,
                      std::string_view bytes, std::int64_t modifiedMs);
    void finishOp(std::int64_t op, OpStatus status, const std::string& note);

    // The audio a slot holds AFTER an operation — the rows that make each
    // slot's timeline readable on its own. `hash` is absent when the store
    // has never seen those bytes: the file is then known by name and size,
    // and no take can be fetched for it. `modifiedMs` as in recordLanded,
    // and absent only where heldBefore says the row may not carry one.
    void recordPresentAudio(std::int64_t op, int slot, int track, const std::string& name,
                            std::int64_t size, const std::optional<std::string>& hash,
                            std::optional<std::int64_t> modifiedMs);

    // --- loudness readings (#140): facts about bytes, under their hash ---

    // The raw reading of a take's bytes, filed under their content hash and
    // nothing else: no slot, no operation, no foreign key to blobs_meta — a
    // reading holds for bytes the history has only named (a slot_audio row
    // with no kept blob) and for bytes read off the card that no operation
    // archived. The same hash again replaces: the bytes did not change, so
    // a newer measurement is the same fact, dated anew. Refused: a hash that
    // is not 32 bytes, a negative count, a value that is not a number —
    // SQLite would file a NaN as NULL, which here reads "unmeasurable" — an
    // infinite loudness, and a loudness beside a -inf true peak.
    // What the numbers mean against today's target is the reader's business
    // (MainComponent::describeReading); the store keeps the numbers.
    void recordReading(const std::string& hash, const wav::LoudnessReading& reading,
                       std::int64_t nowMs);
    struct StoredReading {
        wav::LoudnessReading reading;
        std::int64_t measuredMs = 0; // when the reading was taken
    };
    // Absent when these bytes were never measured. A hash of the wrong
    // length is refused rather than reported as never measured.
    std::optional<StoredReading> readingFor(const std::string& hash);

    // --- what an operation was about (#144) ---

    // The slot an operation set out to work on, written down when it begins
    // and apart from what it changes: a normalize that finds its slot at
    // target changes nothing, and still was about that slot. A subject
    // names no state — it makes nothing restorable and no Undo target.
    // Refused for an operation the store does not have, one that is not
    // pending, a first sighting or maintenance (neither is about a slot), a
    // slot outside 1..99, and a slot named twice for one operation.
    void recordSubject(std::int64_t op, int slot);
    std::vector<int> subjects(std::int64_t op); // ascending

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
        // The state's take, or the archived take when the slot was emptied.
        std::string takeName;
        std::optional<std::string> takeHash;
        bool takeKept = false; // the bytes are in the store: it can be played
        // The take is the one the operation left in the slot (its 'after'
        // side). False when the row offers the take it archived instead — a
        // clear or an undo that emptied the slot. That take is
        // not on the card, whatever the row's place in the timeline.
        std::int64_t takeCount = 0;
        std::int64_t takeBytes = 0;
        bool takeIsAfter = false;
        // The operation was about this slot and recorded nothing here — a
        // normalize that found its slot at target (#144). The row says what
        // was tried, not what the slot holds: it is never the slot's state.
        bool subjectOnly = false;
    };

    // Selected card only; no selection returns no rows. Ordered by time, then
    // sequence. An operation that was about the slot is a row too, flagged
    // subjectOnly when it recorded nothing here.
    std::vector<TimelineEntry> slotTimeline(int slot);

    // --- reads: what the tests look at today, and what #50 builds on ---
    std::optional<std::string> takeBytes(const std::string& hash);
    std::string opStatus(std::int64_t op);
    // Every slot an operation touched, by its body or its audio.
    std::vector<int> touchedSlots(std::int64_t op);
    bool hasAfterAudio(std::int64_t op, int slot);
    // What a new row for a file of this name inherits from the newest word
    // about it before `op`. First a real read of these very facts (name,
    // size, stamp) that no earlier operation on the slot came after: the
    // hash of the bytes it read, with the file's stamp. Otherwise the slot's
    // newest row before `op`: that row's hash — only when it carries one,
    // its size is the file's, and its modification time is the file's now —
    // and the stamp the new row may carry. A newer row without a hash is the last
    // word (the file changed while the app was away), and a stamp or size
    // that differ are another file under the same name: no hash, never
    // guessed from an older row (#141 reads these hashes). A row written
    // before the store kept stamps (version 8 and older) is held to name and
    // size alone, as it was written, so migrated histories keep their slots
    // restorable — and the hash it vouches for is handed on with NO stamp,
    // so the guess stays a guess: a stampless row never tells a connect
    // which file is in the slot (hashOfSighting). The stamp is the file's
    // otherwise, a row without a hash included.
    struct Held {
        std::optional<std::string> hash;
        std::optional<std::int64_t> modifiedMs;
    };
    Held heldBefore(std::int64_t op, int slot, const std::string& name, std::int64_t size,
                    std::int64_t modifiedMs);
    // heldBefore's hash alone.
    std::optional<std::string> hashHeldBefore(std::int64_t op, int slot, const std::string& name,
                                              std::int64_t size, std::int64_t modifiedMs);

    // --- what a connect can tell from a directory entry (#141) ---

    // The card a marker names, when the store has met it. A lookup and
    // nothing else: card() above writes last_seen and the name, which a
    // question about the history must not do.
    std::optional<std::int64_t> cardFor(const std::string& markerId);

    // A take as a connect scan sees it without reading it: the directory
    // entry's name, size and stamp (TakeFacts.h), and the frame count the
    // slot's config says the take has (WavLen).
    struct TakeSighting {
        std::string name;
        std::int64_t size = 0;
        std::int64_t modifiedMs = 0;
        std::int64_t frames = 0;
    };
    // The highest operation sequence ever issued, 0 before the first: what a
    // read that begins now comes after. It never goes down, and beginOp never
    // issues a sequence at or below it — forgotten operations included.
    std::int64_t newestOp();

    // What a real read of a take saw (#141): the file's name, size and stamp
    // on this card's slot, and the hash of the bytes read under them — the
    // check job's read, the player's pass and a normalize's measurement all
    // have all of it.
    // `readBegan` is newestOp() as it stood when the read began; the read is
    // filed after the slot's newest operation at or below it, and every
    // operation that records a body or a take in the slot with a higher
    // sequence makes it history — the order of operations, not of clocks.
    // The same facts read again keep the newest read. Refused: a hash that
    // is not 32 bytes, a negative `readBegan`, and (by the table) a slot
    // outside 1..99.
    void recordSighting(std::int64_t card, int slot, const std::string& name, std::int64_t size,
                        std::int64_t modifiedMs, const std::string& hash, std::int64_t readBegan,
                        std::int64_t nowMs);

    // The hash of the take this card's slot holds, when the history can tell
    // it is the one sighted, from the newest word about the slot:
    //   - a read of these very facts (recordSighting) that no operation
    //     recording a body or a take in the slot came after, by sequence:
    //     its hash — the bytes were read under them;
    //   - otherwise the slot's newest row for the file name on the first
    //     track — the take the LUFS column measures — when it is that last
    //     operation's own, the operation finished (or is the first
    //     sighting, which commits slot by slot), it carries a hash, its size
    //     and its stamp are the entry's, and the body recorded with it (the
    //     newest at or before it) says the same WavLen.
    // All of it or nothing: a row without a hash, with another size or
    // stamp, or without a stamp at all (older than store version 9, or
    // vouched for by such a row) answers nothing and no older row is asked
    // instead; and once anything else about the slot was recorded after it
    // — a clear, a swap, a failed write — the row answers nothing either.
    // Unlike heldBefore, a stampless row gets no allowance here: nothing is
    // at stake but a number on screen, and that number must not be a guess.
    // Scoped to the card: another card's rows are another card's. A read:
    // asking writes nothing.
    std::optional<std::string> hashOfSighting(std::int64_t card, int slot,
                                              const TakeSighting& seen);

    sqlite::Db& db() { return db_; }

    // --- Undo and Redo over the timeline (Undo.h): the store's side ---

    // Ops after the card's last forgotten state, in timeline order: what
    // the undo cursor reads. Older rows remain visible in cardTimeline().
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
        std::int64_t audioBytes;    // takes whose bytes are kept, each once
        std::int64_t otherBytes;    // rows, bodies, indexes
        std::int64_t freeBytes;     // pages the file holds but no longer uses: vacuum() returns them
        std::int64_t diskAvailable; // on the volume the file is on
    };
    Usage usage();

    // Every blob whose bytes are kept, with what holds it: a pinned operation
    // naming it (or a pin on the blob), an operation still pending naming it,
    // or any card's cursor targets naming it as a 'before' — the audio that
    // undo, or redo, would put back (the redo's bytes are what its undo row
    // archived). References count the rows naming the hash, both sides.
    // Snapshot-only takes are marked for release last. A snapshot shared
    // with another operation uses the newest non-snapshot row's date and label.
    std::vector<retention::Blob> keptBlobs(const UndoTargets& targets);

    void pinOp(std::int64_t op, bool pinned);

    // Frees the bytes of these blobs in one transaction: the bytes go, each
    // blobs_meta row is marked released at `nowMs`, and every row that named
    // the take keeps naming it — by name, size and hash — with no bytes behind
    // it. Refused whole, nothing freed, if any of them is not kept or is held
    // (see keptBlobs). Returns the bytes freed.
    std::int64_t releaseBlobs(const std::vector<std::string>& hashes, const UndoTargets& targets,
                              std::int64_t nowMs);

    struct ForgetPlan {
        std::int64_t rowsRemoved = 0; // timeline entries, one per operation
        std::int64_t takesFreed = 0;  // distinct kept blobs losing their last reference
        std::int64_t bytesFreed = 0;
        std::vector<std::int64_t> operations;
        // Pinned operations forgetting takes something of: a state here, or
        // the whole row. One that loses only its badge here is not protected.
        std::vector<std::int64_t> pinned;
        // Cursor targets that lose a state here: the undo on offer goes with
        // the slot. A target this slot was only about keeps its undo.
        std::vector<std::int64_t> undoTargets;
        std::vector<std::string> hashes;
        bool inFlight = false;
        // The Undo boundary will move: an operation loses a state here and
        // keeps one elsewhere, so what is left of it must not be offered as
        // the whole. One forgotten whole moves nothing — a subject it was
        // still about is not a surviving state.
        bool cutsUndo = false;
        bool hasHolds() const { return !pinned.empty() || !undoTargets.empty(); }
        bool operator==(const ForgetPlan&) const = default;
    };
    ForgetPlan planForgetSlot(std::int64_t card, int slot);
    // Replans under the write lock. An optional displayed plan prevents a
    // delayed confirmation from deleting a different set of entries or takes.
    // Vacuum is separate: the worker returns free pages in short slices.
    ForgetPlan forgetSlot(std::int64_t card, int slot, std::int64_t nowMs,
                          bool confirmHolds = false, const ForgetPlan* expected = nullptr);

    // Hands up to `pages` free pages back to the system and returns how many
    // remain. Its own short transaction, a slice at a time, so a worker can
    // stay responsive between slices. The file was created with
    // auto_vacuum=INCREMENTAL for exactly this.
    std::int64_t vacuum(int pages);

    // Every take ever kept — when and how big, released since or
    // not — for the rate the history grows at (retention::forecast).
    std::vector<retention::Write> writes();

    // --- the whole card's timeline (the History window, #73) ---

    // One entry per operation, ordered by time,
    // with every slot the operation touched carrying the same facts
    // slotTimeline gives that slot: one story, two views. `newest` says the
    // operation is the last FINISHED one on that slot, so its state is the
    // one the slot is in — a failed or interrupted write may never have
    // reached the card, so it is not where the slot is, and its state can be
    // offered back like any other. An operation that touched no slot (it
    // failed before the card, or changed only settings) is an entry with no
    // slots — and still with its subjects, when it was about any.
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
        // The slots the operation was about (#144), ascending — beside the
        // ones it touched, often the same ones. A subject carries no facts
        // and no take: a badge on the row, never an offer.
        std::vector<int> subjects;
        // What the operation did to the pedal's own settings, per section:
        // an operation that changed settings and no slot is an entry with
        // no slots and these.
        std::vector<SystemChange> system; // in section order: SETUP, MIDI, CTL
    };
    std::vector<CardEntry> cardTimeline();


    // --- the pedal's own settings in the history (system_changes) ---

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
    // What holds one kept blob, for keptBlobs and for releaseBlobs' refusal.
    struct Holds {
        bool pinned;
        bool undo;
        bool inFlight;
    };
    std::vector<OpSummary> operationsFor(std::int64_t card);
    std::vector<UndoTargets> retentionTargets(const UndoTargets& offered);
    Holds holdsOn(const std::string& hash, const std::vector<UndoTargets>& targets);

    void releaseBytes(const std::string& hash, std::int64_t nowMs);

    std::filesystem::path file_;
    sqlite::Db db_;
    std::optional<std::int64_t> selectedCard_;
};

} // namespace loopercat::history
