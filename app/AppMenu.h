// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>

//==============================================================================
// loopercat::AppMenu — the native macOS menu bar. About lives in the app menu
// (the platform-standard home), Maintenance carries the service actions
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

    juce::StringArray getMenuBarNames() override { return { "Maintenance", "Help" }; }

    juce::PopupMenu getMenuForIndex(int, const juce::String& name) override
    {
        juce::PopupMenu menu;
        if (name == "Maintenance") {
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
    }

private:
    enum { kBackup = 1, kCleanJunk, kImportLegacy, kFeedTheCat };

    const Actions actions_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppMenu)
};

} // namespace loopercat
