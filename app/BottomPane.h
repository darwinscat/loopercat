// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// loopercat::bottompane — which face the window's bottom pane is wearing.
//
// The pane has one place under the slot table and several faces for the
// selected slot: listen to it, set it up, its drums, and what happened to it.
// The strip above it names three of them, in this order.
//
// Rhythm is hidden in 0.9.6 and keeps a face without a tab, so the --rhythm
// snapshot can still draw the pane while nothing a player can click reaches
// it. That is the whole trick of hiding a tab: take its face out of the
// strip's ORDER rather than leave a hole in it, because the window selects
// History by the same number the strip paints it at.
//
// The invariant both sides rely on: the tab at index k shows Face(k), so a
// face at or past tabTitles().size() has no tab at all.
//==============================================================================
namespace loopercat::bottompane
{

enum Face { audio = 0, properties = 1, history = 2, rhythm = 3 };

// The strip's labels, in tab order.
inline juce::StringArray tabTitles() { return { "Audio", "Properties", "History" }; }

} // namespace loopercat::bottompane
