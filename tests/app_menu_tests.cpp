// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The app menu after the menu bar went (0.9.6), from what it promises:
//
//   - the menu bar has no names, and no name it used to have gives a menu:
//     not Edit, not Maintenance, not Window, not Help
//   - the app menu holds exactly one item, "About LooperCat", and pressing
//     it opens About
//   - the ids the old items answered to are dead: selecting any of them
//     calls nobody
//   - on macOS the model is the one the OS holds while it lives, and is
//     handed back when it goes
//   - and the app menu the OS was actually HANDED is that one item, with
//     About behind it. The static helper says what would be built; this says
//     what the constructor built, which is the only part a player can press

#include "support.hpp"

#include "../app/AppMenu.h"

#include <vector>

using namespace loopercat;

namespace {

std::vector<juce::PopupMenu::Item> itemsOf(const juce::PopupMenu& menu)
{
    std::vector<juce::PopupMenu::Item> out;
    juce::PopupMenu::MenuItemIterator it(menu);
    while (it.next())
        out.push_back(it.getItem());
    return out;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    int abouts = 0;
    {
        AppMenu menu({ .about = [&] { ++abouts; } });

        // --- no bar, and no menu behind any name the bar used to carry ---
        CHECK_EQ(menu.getMenuBarNames().size(), 0);
        int index = 0;
        for (const char* gone : { "Edit", "Maintenance", "Window", "Help" })
            CHECK_EQ(menu.getMenuForIndex(index++, gone).getNumItems(), 0);
        CHECK_EQ(menu.getMenuForIndex(0, juce::String()).getNumItems(), 0);

        // --- the ids of the menu that went mean nothing now ---
        // kCleanJunk, kFeedTheCat, kUndo, kRedo, kOpenHistory were 1..5, and
        // every other id a stale menu could send is as dead.
        for (int id = 0; id <= 6; ++id)
            menu.menuItemSelected(id, 0);
        CHECK_EQ(abouts, 0);

#if JUCE_MAC
        CHECK(juce::MenuBarModel::getMacMainMenu() == &menu);
        // The extras AppKit holds, read back from JUCE's own handler: a
        // constructor that passed nothing, or passed a menu with no action
        // behind its item, loses About from the real app menu while every
        // other check here still passes.
        const juce::PopupMenu* installed = juce::MenuBarModel::getMacExtraAppleItemsMenu();
        CHECK(installed != nullptr);
        if (installed != nullptr) {
            const auto items = itemsOf(*installed);
            CHECK_EQ(items.size(), std::size_t { 1 });
            if (items.size() == 1) {
                CHECK_EQ(items.front().text, juce::String("About LooperCat"));
                CHECK(items.front().isEnabled);
                CHECK(items.front().action != nullptr);
                if (items.front().action)
                    items.front().action();
                CHECK_EQ(abouts, 1);
            }
        }
#endif
    }
#if JUCE_MAC
    CHECK(juce::MenuBarModel::getMacMainMenu() == nullptr);
    CHECK(juce::MenuBarModel::getMacExtraAppleItemsMenu() == nullptr);
    CHECK_EQ(abouts, 1); // the OS's own item, pressed once, and nothing else
#else
    CHECK_EQ(abouts, 0); // no bar here and no app menu: nothing to press
#endif

    // --- the app menu: About LooperCat, and it opens About ---
    {
        int opened = 0;
        const auto items = itemsOf(AppMenu::appleMenuItems([&] { ++opened; }));
        CHECK_EQ(items.size(), std::size_t { 1 });
        if (items.size() == 1) {
            CHECK_EQ(items.front().text, juce::String("About LooperCat"));
            CHECK(items.front().isEnabled);
            CHECK(items.front().subMenu == nullptr);
            CHECK(items.front().action != nullptr);
            if (items.front().action)
                items.front().action();
            CHECK_EQ(opened, 1);
        }
    }

    // --- an app menu with nobody to call does not fall over ---
    {
        const auto items = itemsOf(AppMenu::appleMenuItems(nullptr));
        CHECK_EQ(items.size(), std::size_t { 1 });
        if (items.size() == 1 && items.front().action)
            items.front().action();
    }

    return testkit::summary("app_menu_tests");
}
