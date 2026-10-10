// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"
#include <loopercat/CardMarker.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//==============================================================================
// loopercat::history::HistoryRecorder — between the pedal worker and the store.
// It owns the store, the open session and the operations in flight, and turns
// what the core reports (commands::Archive, commands::Journal) into rows.
//
// Worker thread only, every method: the worker opens an operation before its
// job touches the card (PedalWorker::Job::before), the core's hooks call in
// while it runs, and the worker closes it with the job's outcome (::after).
//
// The store opens on first use. If it cannot open — a newer LooperCat's store,
// a damaged file, a full disk — every operation is refused with that reason,
// and nothing is written to the card: the history is the undo, and a write
// without its undo is the thing this whole feature exists to prevent. Browsing
// the card does not go through here and keeps working.
//
// Sessions and timelines follow the marker identity, including when two cards
// mount at the same path. The first sighting is resumed one committed slot at
// a time; foreground writes preserve any affected slot before changing it.
//==============================================================================
namespace loopercat::history
{

class HistoryRecorder
{
public:
    using Clock = std::function<std::int64_t()>; // milliseconds since the epoch

    HistoryRecorder(std::filesystem::path dir, Clock clock);
    ~HistoryRecorder();
    HistoryRecorder(const HistoryRecorder&) = delete;
    HistoryRecorder& operator=(const HistoryRecorder&) = delete;

    void begin(const std::string& opId, const std::string& kind,
               const std::filesystem::path& volume);
    // Maintenance is journaled against the captured card without resolving a volume.
    void beginMaintenance(const std::string& opId, std::int64_t card);
    // The slot an operation that has begun is about (#144), written down
    // before the job touches the card: an operation that then changes
    // nothing — a normalize that finds its slot at target — still keeps its
    // slot in the history. Refused for maintenance: clearing a slot's
    // history is about the history, and a subject would write the slot
    // straight back into what was just forgotten (ForgetSlotJob.h).
    void subject(const std::string& opId, int slot);
    void keepAudio(const std::string& opId, int slot, const std::string& fileName,
                   std::string_view bytes);
    void bodies(const std::string& opId, const std::vector<commands::SlotChange>& changes);
    // A take has landed on the card: its row carries the hash of the bytes
    // the core handed over and the modification time the card's directory
    // entry now shows for the file (#141).
    void landed(const std::string& opId, int slot, const std::string& fileName,
                std::string_view bytes);
    // A loudness reading of bytes the worker has in hand, filed under their
    // content hash (#140) — a fact about bytes, not about an operation, so
    // none needs to have begun. Written while a card's session is open and
    // not otherwise: with no card in front of it the history is not told
    // about bytes it cannot place, and the store is never opened for this
    // alone. Returns whether the reading went in; the store's own refusals
    // (a hash of the wrong length, a value that is not a number) throw.
    bool reading(const std::string& hash, const wav::LoudnessReading& reading);
    // What a real read of a take on `volume` saw (#141): the file's name,
    // size and stamp, and the hash of the bytes read under them — filed for
    // the card whose session is open on that volume, and not otherwise: a
    // read of another card, or of a card no session has opened yet, is not
    // this history's to place. `readBegan` is newestOp() as it stood when
    // the read began, which places the read among the operations
    // (HistoryStore::recordSighting). Returns whether the sighting went in.
    bool sighted(const std::filesystem::path& volume, int slot, const std::string& name,
                 std::int64_t size, std::int64_t modifiedMs, const std::string& hash,
                 std::int64_t readBegan);
    // What a command read inside an operation it is part of (normalize's
    // measurement): the take's facts as one stat now gives them, under the
    // hash of the bytes read, placed just before the operation's own rows
    // so that anything it then records in the slot makes the read history.
    // Nothing when the entry's size is not the bytes'.
    void sightedInOperation(const std::string& opId, int slot, const std::string& fileName,
                            std::int64_t size, const std::string& hash);
    // The newest operation the history has begun, by sequence, for a read
    // about to begin to note as its place (#141): absent until the store has
    // opened. Any thread — a read begins on the player's thread too. It
    // moves only forward, with every operation this recorder begins; a lag
    // behind the store only makes a read look older than it is, so it is
    // set aside sooner, never trusted longer.
    std::optional<std::int64_t> newestOp() const;
    // Told what a command measured on its way (normalize, through
    // withHistory), once the reading is filed: the slot and the reading of
    // the bytes it holds before any write. Set before the worker starts;
    // called on the worker thread.
    std::function<void(int slot, const wav::LoudnessReading& reading)> onMeasured;
    // `error` empty = the job succeeded, and `note` is the line the job wrote
    // about itself ("normalized -3.2 dB", "already at -18.0 LUFS") — the only
    // record of an operation that decided to change nothing, and empty for the
    // ones whose rows already say everything. A failed job keeps its error
    // there instead: the reason outranks the story. An operation that never
    // began (the worker's gate refused the job first) has nothing to close.
    void finish(const std::string& opId, const std::string& error, const std::string& note = {});
    // For an 'undo' or 'redo' operation that has begun: the operation it
    // reverts (#73). Called before the job touches the card, so the row
    // names its target even if the write is then cut off.
    void reverts(const std::string& opId, std::int64_t target);
    // What an operation is about to do to the pedal's own settings, per
    // section, reported before the settings pair is written — the core's
    // journal hook for SYSTEM*.RC0 lands here.
    void systemChanges(const std::string& opId, const std::vector<HistoryStore::SystemChange>& changes);

    struct Snapshot {
        std::int64_t op;
        std::string markerId;
        std::filesystem::path volume;
    };
    std::optional<Snapshot> firstSeen(const std::filesystem::path& volume);
    // Returns the number of attempted slots (99 completes the operation).
    // Failed slots retain their reason and are not retried on reconnect.
    int snapshotStep(const Snapshot& snapshot, int slot);
    void interruptSnapshot(const Snapshot& snapshot, const std::string& reason);
    void disconnect();
    // Resolve the mounted identity before checking a queued history action.
    void selectVolume(const std::filesystem::path& volume) { sessionFor(volume); }
    void preserveSlots(const std::string& opId, const std::vector<int>& slots);

    HistoryStore& store();

    // The connection an operation on this volume would be recorded in right
    // now: the session this run opened for it — or none, when nothing has
    // been written in this run yet, or the last write went to another volume
    // and the next one opens a session of its own. Undo reads it (#73): a
    // target from any other session lies across a connection.
    std::optional<std::int64_t> sessionOn(const std::filesystem::path& volume) const
    {
        if (session_ && sessionVolume_ == volume)
            return session_;
        return std::nullopt;
    }

private:
    // An operation in flight: its row, what kind it is (a swap moves audio
    // between two slots without writing a byte) and the volume it runs on.
    struct Operation {
        std::int64_t row = 0;
        std::string kind;
        std::filesystem::path volume;
    };

    // An operation that has begun and not finished; refused by name otherwise.
    const Operation& operation(const std::string& opId) const;
    std::int64_t opRow(const std::string& opId) const;
    // The status of a baseline operation, or nothing when the row is gone:
    // clearing a slot's history can take the snapshot with it.
    std::optional<std::string> snapshotStatus(std::int64_t op);
    // After a successful operation, write down what each slot it touched now
    // holds, read from the card. Without it a slot's timeline cannot be read
    // on its own: a swap would send the reader into the other slot's rows,
    // and a rename would leave no sign that the slot held a take at all.
    // Hashes are carried only where they are certain — from the slot's own
    // last state, or, for a swap, from the slot it exchanged with, and only
    // when that state's size and stamp are the file's now — or, from a row
    // older than the stamps, on name and size, and then without a stamp of
    // its own (HistoryStore::heldBefore). A file the store has never seen,
    // or has seen change since, is recorded by name and size, with no hash.
    void recordWhatSlotsHold(const Operation& op);
    std::int64_t sessionFor(const std::filesystem::path& volume);

    // The RC-5 has one track per memory; slot_audio.track is there for the
    // pedals that have more.
    static constexpr int kTrack = 1;

    std::filesystem::path dir_;
    Clock clock_;
    std::optional<HistoryStore> store_;
    std::string openError_;
    std::optional<std::int64_t> session_;
    std::filesystem::path sessionVolume_;
    std::string sessionMarker_;
    std::optional<Snapshot> snapshot_;
    std::map<std::string, Operation> ops_;
    // newestOp(): -1 until the store has opened. Written on the worker,
    // read on any thread.
    std::atomic<std::int64_t> newestOp_ { -1 };
    void published(std::int64_t op);
};

// The one wiring from an operation's WriteOptions into the history, shared by
// the app and its tests so the tests exercise what ships: each replaced take
// is kept by the history before the card changes; bodies, landed takes and
// settings sections are recorded under the operation's id; a reading the
// command took on its way is filed under the bytes it measured (#140).
commands::WriteOptions withHistory(const std::shared_ptr<HistoryRecorder>& recorder,
                                   commands::WriteOptions options);

} // namespace loopercat::history
