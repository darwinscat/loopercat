// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>

//==============================================================================
// loopercat::AppMenu — the native macOS menu bar. About lives in the app menu
// (the platform-standard home), Edit carries Undo and Redo over the history
// (#73) — each item names what it would put back, "Undo trim of slot 14" —
// Window opens the card's whole history, Maintenance carries the service actions
// (config backup, junk sweep) that would otherwise crowd the toolbar — the
// toolbar keeps only the primary Connect / Disconnect story — and Help holds
// "Feed the Cat", the family tip jar. Windows/Linux have no menu bar; there
// the tip jar lives in the version badge's About popover.
//==============================================================================
namespace loopercat {

class AppMenu final : public juce::MenuBarModel
{
public:
    struct Actions {
        std::function<void()> about;
        std::function<void()> backup;
        std::function<void()> cleanJunk;
        // The folders from before the history (#72): the store is on this
        // computer, so the item needs no pedal and is never greyed out.
        std::function<void()> importLegacy;
        std::function<void()> feedTheCat;         // Help → the family tip jar, in the browser
        std::function<bool()> maintenanceEnabled; // pedal connected and idle
        std::function<bool()> cleanJunkEnabled;   // and the card is one this app writes to
        // Edit → Undo / Redo (#73): the words are read when the menu opens,
        // from what the history offers right now; greyed out while the
        // pedal is busy or away, or with nothing to put back.
        std::function<void()> undo;
        std::function<void()> redo;
        std::function<juce::String()> undoText;
        std::function<juce::String()> redoText;
        std::function<bool()> undoEnabled;
        std::function<bool()> redoEnabled;
        // Window → History: the whole card's timeline. The store is on this
        // computer, so like the import it needs no pedal.
        std::function<void()> openHistory;
    };

    explicit AppMenu(Actions actions) : actions_(std::move(actions))
    {
#if JUCE_MAC
        juce::PopupMenu appMenuExtras;
        appMenuExtras.addItem("About LooperCat", [about = actions_.about] {
            if (about)
                about();
        });
        juce::MenuBarModel::setMacMainMenu(this, &appMenuExtras);
#endif
    }

    ~AppMenu() override
    {
#if JUCE_MAC
        if (juce::MenuBarModel::getMacMainMenu() == this)
            juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    }

    juce::StringArray getMenuBarNames() override { return { "Edit", "Maintenance", "Window", "Help" }; }

    juce::PopupMenu getMenuForIndex(int, const juce::String& name) override
    {
        juce::PopupMenu menu;
        if (name == "Edit") {
            const auto item = [](int id, const std::function<juce::String()>& text,
                                 const std::function<bool()>& enabled, const char* keys) {
                juce::PopupMenu::Item entry(text ? text() : juce::String());
                entry.itemID = id;
                entry.isEnabled = enabled && enabled();
                entry.shortcutKeyDescription = juce::String::fromUTF8(keys);
                return entry;
            };
            menu.addItem(item(kUndo, actions_.undoText, actions_.undoEnabled, "\xe2\x8c\x98Z"));
            menu.addItem(item(kRedo, actions_.redoText, actions_.redoEnabled,
                              "\xe2\x87\xa7\xe2\x8c\x98Z"));
        } else if (name == "Window") {
            menu.addItem(kOpenHistory, "History", true);
        } else if (name == "Maintenance") {
            const bool enabled = actions_.maintenanceEnabled && actions_.maintenanceEnabled();
            menu.addItem(kBackup, "Backup configs", enabled);
            // The sweep writes to the card: a card of a model this app only
            // reads keeps its junk, and the item says so by staying grey.
            menu.addItem(kCleanJunk, "Clean junk from the pedal",
                         enabled && actions_.cleanJunkEnabled && actions_.cleanJunkEnabled());
            menu.addSeparator();
            menu.addItem(kImportLegacy, "Import the folders from before the history", true);
        } else if (name == "Help") {
            menu.addItem(kFeedTheCat, "Feed the Cat");
        }
        return menu;
    }

    void menuItemSelected(int itemId, int) override
    {
        if (itemId == kBackup && actions_.backup)
            actions_.backup();
        else if (itemId == kCleanJunk && actions_.cleanJunk)
            actions_.cleanJunk();
        else if (itemId == kImportLegacy && actions_.importLegacy)
            actions_.importLegacy();
        else if (itemId == kFeedTheCat && actions_.feedTheCat)
            actions_.feedTheCat();
        else if (itemId == kUndo && actions_.undo)
            actions_.undo();
        else if (itemId == kRedo && actions_.redo)
            actions_.redo();
        else if (itemId == kOpenHistory && actions_.openHistory)
            actions_.openHistory();
    }

private:
    enum { kBackup = 1, kCleanJunk, kImportLegacy, kFeedTheCat, kUndo, kRedo, kOpenHistory };

    const Actions actions_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppMenu)
};

} // namespace loopercat
