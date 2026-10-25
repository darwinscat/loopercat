// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>

//==============================================================================
// loopercat::AppMenu — the macOS app menu, and nothing else.
//
// The menu bar went in 0.9.6: Edit's Undo and Redo are hidden with their keys,
// Maintenance's junk sweep moved to a Settings tab, Window had only the
// card's whole history to open and that window is hidden too, and Help held
// one link — the family tip jar, which the version badge's About popover has
// carried on every platform all along. Windows and Linux never had a bar
// (#25); now nobody does.
//
// What is left is the item macOS puts in the app menu by platform convention,
// About LooperCat, beside the Hide and Quit that AppKit adds itself. JUCE
// hangs those extras off a MenuBarModel, so this is still one — with a menu
// bar of no names — and it is still the one owner of the Mac main menu: the
// destructor hands it back.
//==============================================================================
namespace loopercat {

class AppMenu final : public juce::MenuBarModel
{
public:
    struct Actions {
        std::function<void()> about;
    };

    // The app menu's own items, exactly as the constructor hands them to
    // JUCE. A static function because that is the only way to read them back:
    // what AppKit does with the menu afterwards is not ours to ask about.
    static juce::PopupMenu appleMenuItems(std::function<void()> about)
    {
        juce::PopupMenu menu;
        menu.addItem("About LooperCat", [open = std::move(about)] {
            if (open)
                open();
        });
        return menu;
    }

    // The actions are not kept: the only one there is goes into the About
    // item, and AppKit holds that item from here on. A member nobody reads
    // is also a warning on every compiler that looks for one.
    explicit AppMenu(Actions actions)
    {
#if JUCE_MAC
        // setMacMainMenu takes a copy of the extras; the local one is enough.
        const juce::PopupMenu extras = appleMenuItems(std::move(actions.about));
        juce::MenuBarModel::setMacMainMenu(this, &extras);
#else
        juce::ignoreUnused(actions); // no menu bar off macOS, and never was (#25)
#endif
    }

    ~AppMenu() override
    {
#if JUCE_MAC
        if (juce::MenuBarModel::getMacMainMenu() == this)
            juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    }

    juce::StringArray getMenuBarNames() override { return {}; }

    juce::PopupMenu getMenuForIndex(int, const juce::String&) override { return {}; }

    // No bar, no items, no ids: the About item carries its own action, so
    // nothing arrives here. An id that used to mean something now means
    // nothing, which is what a stale menu must do.
    void menuItemSelected(int, int) override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppMenu)
};

} // namespace loopercat
