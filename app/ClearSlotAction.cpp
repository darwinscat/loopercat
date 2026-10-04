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

void showClearSlotConfirmation(int slot, std::function<void(int)> decided)
{
    auto* dialog = new juce::AlertWindow(
        "Clear slot " + juce::String(slot) + "?",
        juce::String::fromUTF8(
            "Its history is kept — you can restore it from the History tab."),
        juce::MessageBoxIconType::WarningIcon);
    // Every destructive dialog answers the same way: Cancel is the default, on
    // Return and on Escape, and it is the button the keyboard starts on. One
    // key must not mean "yes, wipe it" here and "no, leave it" next door.
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::returnKey),
                                   juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->addButton("Clear", 1);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(std::move(decided)), true);
    dialog->getButton(0)->grabKeyboardFocus();
}

} // namespace

void addToMenu(juce::PopupMenu& menu, bool hasTake)
{
    menu.addItem(menuItemId, juce::String::fromUTF8("Clear slot\xe2\x80\xa6"), hasTake);
}

void request(int slot, bool hasTake, std::shared_ptr<history::HistoryRecorder> recorder,
             std::function<void(PedalWorker::Job)> enqueue, AskConfirmation ask)
{
    if (!hasTake)
        return;

    if (!ask)
        ask = showClearSlotConfirmation;
    ask(slot, [slot, rec = std::move(recorder), submit = std::move(enqueue)](int choice) {
        // No job is prepared or queued before an explicit Clear decision.
        if (choice != 1)
            return;
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
        job.after = [rec, id = options.opId](const JobOutcome& outcome) {
            rec->finish(id, outcome.error());
        };
        submit(std::move(job));
    });
}

} // namespace loopercat::clearSlotAction
