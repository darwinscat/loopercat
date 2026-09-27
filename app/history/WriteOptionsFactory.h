// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "HistoryRecorder.h"
#include "../OperationId.h"

#include <juce_core/juce_core.h>

namespace loopercat::history {

// The app's write wiring, shared with the command integration tests.
inline commands::WriteOptions makeWriteOptions(const std::shared_ptr<HistoryRecorder>& recorder)
{
    const auto label = juce::Time::getCurrentTime().formatted("%Y-%m-%dT%H-%M-%S");
    return withHistory(recorder, { .opId = opid::make(label.toStdString()) });
}

} // namespace loopercat::history
