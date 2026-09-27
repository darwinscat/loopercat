// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "support.hpp"
#include "../app/HistoryPane.h"
int main()
{
    juce::ScopedJuceInitialiser_GUI runtime;
    loopercat::HistoryPane pane;
    int calls = 0;
    pane.onClearHistory = [&](int slot) { CHECK_EQ(slot, 4); ++calls; };
    CHECK_EQ(pane.clearHistoryText().toStdString(), std::string("Clear history of this slot\xe2\x80\xa6"));
    CHECK(!pane.clearHistoryEnabled()); pane.clearHistory(); CHECK_EQ(calls, 0);
    pane.setRows({{"now", "Renamed", "", "", false, false, 1}}, 4);
    CHECK(pane.clearHistoryEnabled()); pane.clearHistory(); CHECK_EQ(calls, 1);
    pane.setBusy(true); CHECK(!pane.clearHistoryEnabled()); pane.clearHistory(); CHECK_EQ(calls, 1);
    pane.setBusy(false); CHECK(pane.clearHistoryEnabled());
    pane.setRows({}, 4); CHECK(!pane.clearHistoryEnabled());
    pane.clear(); pane.clearHistory(); CHECK_EQ(calls, 1);
    return testkit::summary("history_pane_tests");
}
