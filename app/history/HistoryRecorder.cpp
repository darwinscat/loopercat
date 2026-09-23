// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "HistoryRecorder.h"

namespace loopercat::history
{

HistoryRecorder::HistoryRecorder(std::filesystem::path dir, std::string model, Clock clock)
    : dir_(std::move(dir)), model_(std::move(model)), clock_(std::move(clock))
{
    if (!clock_)
        throw Error("the history recorder needs a clock");
}

HistoryRecorder::~HistoryRecorder()
{
    if (store_ && session_) {
        try {
            store_->closeSession(*session_, clock_());
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
    if (session_ && sessionVolume_ == volume)
        return *session_;
    const std::int64_t now = clock_();
    if (session_) {
        store().closeSession(*session_, now);
        session_.reset();
    }
    std::filesystem::path named = volume;
    if (named.filename().empty())
        named = named.parent_path(); // "/Volumes/BOSS RC-5/" names its card too
    const std::string label = sqlite::utf8(named.filename());
    if (label.empty())
        throw Error("cannot name the card mounted at " + volume.string());
    session_ = store().openSession(store().card(model_, label, now), now);
    sessionVolume_ = volume;
    return *session_;
}

void HistoryRecorder::begin(const std::string& opId, const std::string& kind,
                            const std::filesystem::path& volume)
{
    if (ops_.contains(opId))
        throw Error("operation " + opId + " has already begun");
    const std::int64_t session = sessionFor(volume);
    ops_[opId] = store().beginOp(session, opId, kind, clock_());
}

std::int64_t HistoryRecorder::opRow(const std::string& opId) const
{
    const auto found = ops_.find(opId);
    if (found == ops_.end())
        throw Error("operation " + opId + " reported to the history without having begun");
    return found->second;
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

void HistoryRecorder::finish(const std::string& opId, const std::string& error,
                             const std::string& note)
{
    const auto found = ops_.find(opId);
    if (found == ops_.end())
        return; // never began: the worker refused the job before it reached the card
    const std::int64_t row = found->second;
    ops_.erase(found);
    store().finishOp(row, error.empty() ? OpStatus::done : OpStatus::failed,
                     error.empty() ? note : error);
}

commands::WriteOptions withHistory(const std::shared_ptr<HistoryRecorder>& recorder,
                                   commands::WriteOptions options, commands::Archive alsoKeep)
{
    if (recorder == nullptr)
        throw Error("an operation cannot be recorded without a recorder");
    if (options.opId.empty())
        throw Error("an operation cannot be recorded without an id");
    const std::string opId = options.opId;
    options.archive = [recorder, opId, thenKeep = std::move(alsoKeep)](
                          int slot, const std::string& fileName, std::string_view bytes) {
        recorder->keepAudio(opId, slot, fileName, bytes);
        if (thenKeep)
            thenKeep(slot, fileName, bytes);
    };
    options.journal.bodiesChanging = [recorder, opId](const std::vector<commands::SlotChange>& changes) {
        recorder->bodies(opId, changes);
    };
    options.journal.audioWritten = [recorder, opId](int slot, const std::string& fileName,
                                                    std::string_view bytes) {
        recorder->landed(opId, slot, fileName, bytes);
    };
    return options;
}

} // namespace loopercat::history
