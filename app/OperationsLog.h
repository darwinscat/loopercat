// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// loopercat::oplog — the append-only operations journal. One timestamped line
// per operation that changed a player's audio (normalize asked for it first,
// issue #53), so "what did the app do to my file" still has an answer after
// the toast is gone. Plain text at <dataDir>/operations.log, oldest first.
//
// Append is best-effort by design: every writer sits on a path that already
// succeeded at its real work, and a full disk must not turn a finished push
// into an error. All writers run on the single pedal worker thread, so lines
// never interleave.
//==============================================================================
namespace loopercat::oplog
{

inline void append(const juce::File& dataDir, const juce::String& line)
{
    juce::FileOutputStream stream(dataDir.getChildFile("operations.log")); // opens at end-of-file
    if (!stream.openedOk())
        return;
    stream << juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S").toRawUTF8() << "  "
           << line.toRawUTF8() << "\n";
}

} // namespace loopercat::oplog
