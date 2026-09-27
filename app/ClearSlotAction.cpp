// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ClearSlotAction.h"
#include "history/WriteOptionsFactory.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>

namespace loopercat::clearSlotAction
{
namespace {

// Keep the confirmation and its gate together so a test can drive the same
// dialog as the slot menu. No job is prepared or queued before Clear.
void showClearSlotConfirmation(int slot, std::function<void()> clear)
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

} // namespace

void addToMenu(juce::PopupMenu& menu, bool hasTake)
{
    menu.addItem(menuItemId, juce::String::fromUTF8("Clear slot\xe2\x80\xa6"), hasTake);
}

void request(int slot, bool hasTake, std::shared_ptr<history::HistoryRecorder> recorder,
             std::function<void(PedalWorker::Job)> enqueue)
{
    if (!hasTake)
        return;

    showClearSlotConfirmation(slot, [slot, rec = std::move(recorder),
                                     submit = std::move(enqueue)] {
        const auto options = history::makeWriteOptions(rec);
        PedalWorker::Job job {
            "Clear slot " + juce::String(slot), slot,
            [slot, options](const volume::fs::path& volumePath) {
                commands::clear(volumePath, { slot }, { .write = options });
            }
        };
        job.before = [rec, id = options.opId](const volume::fs::path& volumePath) {
            rec->begin(id, "clear", volumePath);
        };
        job.after = [rec, id = options.opId](const std::string& error) {
            rec->finish(id, error);
        };
        submit(std::move(job));
    });
}

} // namespace loopercat::clearSlotAction
