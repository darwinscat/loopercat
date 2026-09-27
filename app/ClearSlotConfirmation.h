// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>

namespace loopercat
{

// Keep the confirmation and its gate together so a test can drive the same
// dialog as the slot menu. No job is prepared or queued before Clear.
inline void showClearSlotConfirmation(int slot, std::function<void()> clear)
{
    auto* dialog = new juce::AlertWindow(
        "Clear slot " + juce::String(slot) + "?",
        juce::String::fromUTF8(
            "Its history is kept — you can restore it from the History tab."),
        juce::MessageBoxIconType::WarningIcon);
    dialog->addButton("Clear", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [confirmed = std::move(clear)](int choice) {
            if (choice == 1)
                confirmed();
        }), true);
}

} // namespace loopercat
