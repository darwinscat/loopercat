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

    // The operation Cmd-Z would target (#73): the newest finished one the app
    // or the pedal made. A legacy row has no after-state to come back from.
    std::optional<std::int64_t> offeredUndo();

    // Every blob whose bytes are kept, with what holds it: a pinned operation
    // naming it (or a pin on the blob), an operation still pending naming it,
    // or `undoOp` naming it as a 'before' — the audio that undo would put
    // back. References count the rows naming the hash, both sides.
    std::vector<retention::Blob> keptBlobs(std::optional<std::int64_t> undoOp);

    void pinOp(std::int64_t op, bool pinned);

    // Frees the bytes of these blobs in one transaction: the bytes go, each
    // blobs_meta row is marked released at `nowMs`, and every row that named
    // the take keeps naming it — by name, size and hash — with no bytes behind
    // it. Refused whole, nothing freed, if any of them is not kept or is held
    // (see keptBlobs). Returns the bytes freed.
    std::int64_t releaseBlobs(const std::vector<std::string>& hashes,
                              std::optional<std::int64_t> undoOp, std::int64_t nowMs);

    // Hands up to `pages` free pages back to the system and returns how many
    // remain. Its own short transaction, a slice at a time, so a worker can
    // stay responsive between slices. The file was created with
    // auto_vacuum=INCREMENTAL for exactly this.
    std::int64_t vacuum(int pages);

    // Every take and document ever kept — when and how big, released since or
    // not — for the rate the history grows at (retention::forecast).
    std::vector<retention::Write> writes();

private:
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
    Holds holdsOn(const std::string& hash, std::optional<std::int64_t> undoOp);

    std::filesystem::path file_;
    sqlite::Db db_;
};

} // namespace loopercat::history
