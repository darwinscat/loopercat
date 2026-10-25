// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless harness for the Settings dialog's tabs, after the card sweep moved
// into it (0.9.6): the REAL SettingsDialog, selected through the same seams a
// headless run uses, from what the dialog promises:
//
//   - five tabs, in this order, and exactly one of them says Maintenance
//   - picking Maintenance shows the maintenance panel and hides the storage
//     panel; picking History does the reverse. A tab whose index and whose
//     title disagree shows the wrong panel, which is the one bug a panel
//     tested on its own cannot catch
//   - the panels get the dialog's whole area under the strip, and the strip
//     is wide enough for five tabs at the dialog's own width
//   - the sweep the window answers is reachable from the tab: pressing the
//     tab's button calls back
//
// The dialog needs an audio device manager and a settings file, exactly as
// the gear gives it one; the --history-storage seam builds the same dialog the
// same way (app/Main.cpp), so nothing here is a copy of the real thing.

#include "support.hpp"

#include "../app/SettingsDialog.h"

#include <filesystem>
#include <string>

using namespace loopercat;

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
        : path(fs::temp_directory_path()
               / ("loopercat-settings-" + std::to_string(juce::Time::currentTimeMillis())))
    {
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec); // a settings file left open would fail here, and should
    }
};

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceRuntime;

    TempDir home;
    juce::AudioDeviceManager devices;
    AppSettings settings { juce::File(juce::String(home.path.string())) };
    int sweeps = 0;

    SettingsDialog dialog(devices, settings, { true, false, false }, nullptr,
                          importprefs::defaults(), nullptr);
    dialog.maintenance().onCleanJunk = [&] { ++sweeps; };

    // --- five tabs, in order, Maintenance exactly once ---
    const juce::StringArray titles = dialog.tabTitles();
    CHECK_EQ(titles.size(), 5);
    CHECK_EQ(titles[0], juce::String("Audio"));
    CHECK_EQ(titles[1], juce::String("Columns"));
    CHECK_EQ(titles[2], juce::String("Import"));
    CHECK_EQ(titles[3], juce::String("History"));
    CHECK_EQ(titles[4], juce::String("Maintenance"));
    int saysMaintenance = 0;
    for (const auto& title : titles)
        saysMaintenance += title == "Maintenance" ? 1 : 0;
    CHECK_EQ(saysMaintenance, 1);
    // Five tabs fit the dialog the gear opens.
    CHECK(titles.size() * TabStrip::kTabWidth <= dialog.getWidth());

    // --- the two panel tabs show one panel each ---
    dialog.showMaintenance();
    CHECK(dialog.maintenance().isVisible());
    CHECK(!dialog.storage().isVisible());
    dialog.showHistoryStorage();
    CHECK(!dialog.maintenance().isVisible());
    CHECK(dialog.storage().isVisible());
    dialog.showMaintenance();
    CHECK(dialog.maintenance().isVisible());
    CHECK(!dialog.storage().isVisible());

    // --- the panel gets the room under the strip ---
    const auto area = dialog.maintenance().getBounds();
    CHECK(!area.isEmpty());
    CHECK(dialog.getLocalBounds().contains(area));
    CHECK_EQ(area.getWidth(), dialog.getWidth());
    CHECK(area.getHeight() > dialog.getHeight() / 2);

    // --- and the sweep is reachable from the tab ---
    dialog.maintenance().show(
        maintenance::offer({ .connected = true, .busy = false, .family = std::string(profile::kRc5.familyName) }));
    CHECK(dialog.maintenance().cleanEnabled());
    dialog.maintenance().cleanJunk();
    CHECK_EQ(sweeps, 1);

    return testkit::summary("settings_dialog_harness");
}
