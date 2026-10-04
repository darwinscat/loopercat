// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

//==============================================================================
// loopercat::kHintDelayMs — how long the pointer rests on something before
// its hint shows. One number for every window of the app: the main window's
// hints (the player's loudness readout had the first) and the History
// window's, which lives in a desktop window of its own with its own
// juce::TooltipWindow — so the two feel alike.
//==============================================================================
namespace loopercat
{

constexpr int kHintDelayMs = 600;

} // namespace loopercat
