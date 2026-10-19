// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HintDelay.h"
#include "HistoryWindow.h"

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::HistoryWindowHost — the desktop window the History view lives in
// (#73). The view is owned by whoever feeds it; this only frames it: a
// native title bar, and a close button that hides rather than destroys (the
// view and its selection are still there the next time it opens). It answers
// no keys of its own: Undo and Redo lost theirs in 0.9.6 along with the
// menu, here as in the main window.
//
// The hints are the frame's too. A juce::TooltipWindow shows tips only for
// components sharing its own peer, so the main window's never reaches a
// view living in this one (#143): the view's list answers getTooltipForRow,
// and this window is where the answer can appear — after the same rest as
// in the main window (HintDelay.h).
//==============================================================================
namespace loopercat
{

class HistoryWindowHost final : public juce::DocumentWindow
{
public:
    explicit HistoryWindowHost(HistoryWindow& view)
        : juce::DocumentWindow("LooperCat History", juce::Colour(0xff121218),
                               juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&view, true);
        setResizable(true, true);
        setResizeLimits(560, 320, 4096, 4096);
        centreWithSize(getWidth(), getHeight());
    }

    void closeButtonPressed() override { setVisible(false); }

private:
    juce::TooltipWindow hints_ { this, kHintDelayMs };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HistoryWindowHost)
};

} // namespace loopercat
