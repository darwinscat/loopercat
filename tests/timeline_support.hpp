// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A timeline row names its takes one per track. The suites written while a
// row could name one take read and build that one take through these: the
// first by track, and on a row being built, the first track's, made on first
// use. A row that names none reads as a take with no name, no hash, not kept.

#pragma once

#include "../app/history/HistoryStore.h"

namespace testkit {

inline loopercat::history::HistoryStore::SlotTake
firstTake(const loopercat::history::HistoryStore::TimelineEntry& entry)
{
    return entry.takes.empty() ? loopercat::history::HistoryStore::SlotTake {} : entry.takes.front();
}

inline loopercat::history::HistoryStore::SlotTake&
takeOf(loopercat::history::HistoryStore::TimelineEntry& entry)
{
    if (entry.takes.empty())
        entry.takes.push_back({ 1, {}, {}, false });
    return entry.takes.front();
}

} // namespace testkit
