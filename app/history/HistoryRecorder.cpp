// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "HistoryRecorder.h"
#include "../OperationId.h"

namespace loopercat::history
{

HistoryRecorder::HistoryRecorder(std::filesystem::path dir, Clock clock)
    : dir_(std::move(dir)), clock_(std::move(clock))
{
    if (!clock_)
        throw Error("the history recorder needs a clock");
}

HistoryRecorder::~HistoryRecorder()
{
    if (store_ && session_) {
        try {
            disconnect();
        } catch (const Error&) {
            // The app is going away; the session reads as never closed, which
            // is what it would read after a crash too.
        }
    }
}

HistoryStore& HistoryRecorder::store()
{
    if (store_)
        return *store_;
    if (!openError_.empty())
        throw Error("the history is unavailable: " + openError_);
    try {
        store_.emplace(dir_);
    } catch (const Error& e) {
        openError_ = e.what();
        throw Error("the history is unavailable: " + openError_);
    }
    return *store_;
}

std::int64_t HistoryRecorder::sessionFor(const std::filesystem::path& volume)
{
    // Open first: failure must not even mint a marker on the card.
    auto& history = store();
    auto identity = marker::read(volume);
    if (!identity)
        identity = marker::mint(volume, rc0::familyOf(commands::readMemory(volume)).familyName).card;
    const std::int64_t now = clock_();
    const auto card = history.card(identity->id, identity->model, identity->name, now);
    history.selectCard(card);
    if (session_ && sessionVolume_ == volume && sessionMarker_ == identity->id)
        return *session_;
    disconnect();
    session_ = history.openSession(card, now);
    sessionVolume_ = volume;
    sessionMarker_ = identity->id;
    return *session_;
}

void HistoryRecorder::disconnect()
{
    if (snapshot_)
        interruptSnapshot(*snapshot_, "The card disconnected before its first snapshot finished");
    snapshot_.reset();
    if (session_) {
        store().closeSession(*session_, clock_());
        session_.reset();
    }
    sessionVolume_.clear();
    sessionMarker_.clear();
    if (store_) store_->selectCard(std::nullopt);
}

std::optional<HistoryRecorder::Snapshot> HistoryRecorder::firstSeen(const std::filesystem::path& volume)
{
    const auto session = sessionFor(volume);
    const auto op = store().firstSeen(session, opid::make("first-seen"), clock_());
    if (store().opStatus(op) == "done") {
        snapshot_.reset();
        return std::nullopt;
    }
    snapshot_ = Snapshot { op, sessionMarker_, volume };
    return snapshot_;
}

int HistoryRecorder::snapshotStep(const Snapshot& snapshot, int slot)
{
    const auto identity = marker::read(snapshot.volume);
    if (!identity || identity->id != snapshot.markerId)
        throw Error("the card changed before its first snapshot finished");
    auto slots = store().snapshotSlots(snapshot.op);
    if (std::find(slots.begin(), slots.end(), slot) == slots.end()) {
        try {
            const auto memory = commands::readMemory(snapshot.volume);
            const auto& family = rc0::profileOf(memory);
            std::vector<HistoryStore::SnapshotTake> takes;
            for (int track = 1; track <= family.trackCount; ++track)
                for (const auto& name : volume::listTrackWavs(snapshot.volume, family, slot, track)) {
                    const auto file = volume::trackDir(snapshot.volume, family, slot, track) / name;
                    const auto limit = sqlite3_limit(store().db().raw(), SQLITE_LIMIT_LENGTH, -1);
                    // Leave room for SQLite's record header and hash. Refuse before allocating.
                    if (std::filesystem::file_size(file) > static_cast<std::uintmax_t>(std::max(0, limit - 1024)))
                        throw Error("take " + name + " exceeds the history store's size limit");
                    takes.push_back({ track, name, commands::readFileBytes(file) });
                }
            store().snapshotSlot(snapshot.op, slot, rc0::slotBody(memory, slot), takes, clock_());
        } catch (const std::exception& error) {
            // A disconnected card resumes later. A bad slot is a durable outcome,
            // so subsequent slots and connections are not blocked by the same take.
            if (!std::filesystem::exists(volume::memoryPath(snapshot.volume, 1)))
                throw;
            store().snapshotFailed(snapshot.op, slot, error.what());
        }
        slots.push_back(slot);
    }
    if (slots.size() == 99 && store().opStatus(snapshot.op) == "pending")
        store().finishOp(snapshot.op, OpStatus::done, "");
    return static_cast<int>(slots.size());
}

// A baseline can have been forgotten since its last step, and a missing
// operation is not a failure to either caller: there is simply nothing left to
// close or to preserve. opStatus() throws on it, so both read the row directly.
std::optional<std::string> HistoryRecorder::snapshotStatus(std::int64_t op)
{
    sqlite::Statement row(store().db(), "SELECT status FROM ops WHERE seq = ?1");
    row.bind(1, op);
    if (!row.step())
        return std::nullopt;
    return row.text(0);
}

void HistoryRecorder::interruptSnapshot(const Snapshot& snapshot, const std::string& reason)
{
    if (snapshotStatus(snapshot.op) == "pending")
        store().finishOp(snapshot.op, OpStatus::interrupted, reason);
}

void HistoryRecorder::preserveSlots(const std::string& opId, const std::vector<int>& slots)
{
    const auto found = ops_.find(opId);
    if (found == ops_.end())
        throw Error("operation " + opId + " reported to the history without having begun");
    const auto status = snapshot_ ? snapshotStatus(snapshot_->op) : std::nullopt;
    if (!snapshot_ || snapshot_->volume != found->second.volume
        || !status.has_value() || *status == "done")
        return;
    for (int slot : slots)
        snapshotStep(*snapshot_, slot);
}

void HistoryRecorder::begin(const std::string& opId, const std::string& kind,
                            const std::filesystem::path& volume)
{
    if (ops_.contains(opId))
        throw Error("operation " + opId + " has already begun");
    firstSeen(volume); // establish the baseline before a write can touch a newly minted card
    const std::int64_t session = *session_;
    ops_[opId] = Operation { store().beginOp(session, opId, kind, clock_()), kind, volume };
}

void HistoryRecorder::beginMaintenance(const std::string& opId, std::int64_t card)
{
    if (ops_.contains(opId)) throw Error("operation already begun");
    sqlite::Statement session(store().db(), "SELECT id FROM sessions WHERE card = ?1 ORDER BY id DESC LIMIT 1");
    session.bind(1, card);
    if (!session.step()) throw Error("no history session for this card");
    const auto row = store().beginOp(session.integer(0), opId, "forget-history", clock_());
    ops_[opId] = Operation { row, "forget-history", {} };
}

void HistoryRecorder::subject(const std::string& opId, int slot)
{
    const auto found = ops_.find(opId);
    if (found == ops_.end())
        throw Error("operation " + opId + " reported to the history without having begun");
    if (found->second.kind == "forget-history")
        throw Error("maintenance operation " + opId + " is about no slot");
    store().recordSubject(found->second.row, slot);
}

std::int64_t HistoryRecorder::opRow(const std::string& opId) const
{
    const auto found = ops_.find(opId);
    if (found == ops_.end())
        throw Error("operation " + opId + " reported to the history without having begun");
    return found->second.row;
}

void HistoryRecorder::keepAudio(const std::string& opId, int slot, const std::string& fileName,
                                std::string_view bytes)
{
    store().keepAudio(opRow(opId), slot, kTrack, fileName, bytes, clock_());
}

void HistoryRecorder::bodies(const std::string& opId,
                             const std::vector<commands::SlotChange>& changes)
{
    store().recordBodies(opRow(opId), changes);
}

void HistoryRecorder::landed(const std::string& opId, int slot, const std::string& fileName,
                             std::string_view bytes)
{
    store().recordLanded(opRow(opId), slot, kTrack, fileName, bytes);
}

void HistoryRecorder::recordWhatSlotsHold(const Operation& op)
{
    const std::vector<int> slots = store().touchedSlots(op.row);
    // A swap is the one operation that moves audio without writing it: the
    // two slots exchange folders, so each one's take is the other's last one.
    const bool swapped = op.kind == "swap" && slots.size() == 2;
    for (const int slot : slots) {
        if (store().hasAfterAudio(op.row, slot))
            continue; // the operation wrote a take here and already said so
        const int from = swapped ? (slot == slots.front() ? slots.back() : slots.front()) : slot;
        const std::filesystem::path dir = volume::wavDir(op.volume, slot);
        for (const std::string& name : volume::listSlotWavs(op.volume, slot)) {
            std::error_code ec;
            const auto size = static_cast<std::int64_t>(std::filesystem::file_size(dir / name, ec));
            if (ec)
                throw Error("cannot measure " + (dir / name).string());
            store().recordPresentAudio(op.row, slot, kTrack, name, size,
                                       store().hashHeldBefore(op.row, from, name, size));
        }
    }
}

void HistoryRecorder::reverts(const std::string& opId, std::int64_t target)
{
    store().setReverts(opRow(opId), target);
}

void HistoryRecorder::systemChanges(const std::string& opId,
                                    const std::vector<HistoryStore::SystemChange>& changes)
{
    const std::int64_t row = opRow(opId);
    for (const auto& change : changes)
        store().recordSystemChange(row, change);
}

void HistoryRecorder::finish(const std::string& opId, const std::string& error,
                             const std::string& note)
{
    const auto found = ops_.find(opId);
    if (found == ops_.end())
        return; // never began: the worker refused the job before it reached the card
    const Operation op = found->second;
    ops_.erase(found);
    // Only a finished operation leaves a card whose slots are worth writing
    // down: after a failed one the card is where the failure left it, and the
    // connect-time check is what tells the history about that.
    if (error.empty() && op.kind != "forget-history")
        recordWhatSlotsHold(op);
    store().finishOp(op.row, error.empty() ? OpStatus::done : OpStatus::failed,
                     error.empty() ? note : error);
}

commands::WriteOptions withHistory(const std::shared_ptr<HistoryRecorder>& recorder,
                                   commands::WriteOptions options)
{
    if (recorder == nullptr)
        throw Error("an operation cannot be recorded without a recorder");
    if (options.opId.empty())
        throw Error("an operation cannot be recorded without an id");
    const std::string opId = options.opId;
    options.archive = [recorder, opId](
                          int slot, const std::string& fileName, std::string_view bytes) {
        recorder->keepAudio(opId, slot, fileName, bytes);
    };
    options.journal.bodiesChanging = [recorder, opId](const std::vector<commands::SlotChange>& changes) {
        recorder->bodies(opId, changes);
    };
    options.journal.audioWritten = [recorder, opId](int slot, const std::string& fileName,
                                                    std::string_view bytes) {
        recorder->landed(opId, slot, fileName, bytes);
    };
    options.journal.slotsChanging = [recorder, opId](const std::vector<int>& slots) {
        recorder->preserveSlots(opId, slots);
    };
    // The settings pair, before it is written: each changed section, its
    // text before and after (sysfile::sectionChanges, in the core). A throw
    // here stops the write with the card as it was.
    options.journal.systemChanging = [recorder, opId](const std::vector<commands::SectionChange>& changes) {
        std::vector<HistoryStore::SystemChange> rows;
        rows.reserve(changes.size());
        for (const auto& change : changes)
            rows.push_back({ change.section, change.before, change.after });
        recorder->systemChanges(opId, rows);
    };
    return options;
}

} // namespace loopercat::history
