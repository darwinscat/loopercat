// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <felitronics/appkit/VersionBadge.h>

#include "AppSettings.h"
#include "AudioEngine.h"
#include "BannerStrip.h"
#include "DeviceWatcher.h"
#include "LooperMark.h"
#include "PedalLight.h"
#include "PedalWorker.h"
#include "PlayerPane.h"
#include "SlotTable.h"
#include "Toast.h"
#include "UpdateCheck.h"

//==============================================================================
// The app's single window content: branded header, status strip, the live
// slot browser, and the listening strip (waveform + transport) for the
// selected slot. Space toggles playback; double-click a row to listen.
//==============================================================================
namespace loopercat
{

class MainComponent final : public juce::Component
{
public:
    // `explicitVolume` pins the pedal path (--volume override); empty = autodetect.
    explicit MainComponent(std::string explicitVolume = {});
    ~MainComponent() override;

    // The headless seams (--snapshot / --select): one synchronous scan+apply,
    // programmatic slot selection, and "is the waveform drawn yet".
    void refreshNow();
    void selectSlot(int slot);
    void setMarkers(double inSeconds, double outSeconds) { player.setMarkers(inSeconds, outSeconds); }
    bool playerReady() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void applySnapshot(const PedalSnapshot& snapshot);
    void slotChosen(int slot, bool startPlaying);
    void openAudioSettings();
    void updateStatusText();
    void updateTableRows();
    void updateToolbar();
    void refreshBanners(); // doctor findings + the lifecycle line + last job error
    void cleanUpGhostMount();
    const SlotRow* slotRowFor(int slot) const; // null when unmounted/out of range

    // Mutations: every action becomes a queued worker job with the standard
    // write options (backup root + timestamp under the app data dir).
    void showSlotMenu(int slot, juce::Point<int> screenPosition);
    void toggleOneShot(int slot, bool currentlyOn);
    void pushWav(int slot, const juce::String& sourcePath, bool slotOccupied);
    void choosePushWav(int slot, bool slotOccupied);
    void pullSlot(int slot);
    void clearSlot(int slot, const juce::String& name);
    commands::WriteOptions makeWriteOptions();

    // Declaration order is lifetime order: settings outlives the checker
    // (its Config captures it), the checker outlives the badge; the engine
    // outlives the pane that drives it; the monitor is last so its delivery
    // dies before anything it touches.
    AppSettings settings;
    UpdateCheck updateChecker { settings };
    ui::LooperBrandHeader header;
    felitronics::appkit::VersionBadge badge;
    PedalLight pedalLight;
    juce::Label status;
    juce::Label hint; // the empty-state prompt, shown while no pedal is mounted
    AudioEngine engine;
    BannerStrip banners;
    juce::TextButton backupButton { "Backup" };
    juce::TextButton cleanButton { "Clean junk" };
    juce::TextButton ejectButton { "Eject" };
    juce::ToggleButton showEmptyToggle { "show empty slots" };
    SlotTable table;
    Toast toast;
    PlayerPane player { engine };
    juce::String deviceError;
    juce::String jobError; // the last failed mutation, until dismissed/superseded
    bool pedalBusy = false;
    bool ghostCleanupStarted = false; // one cleanup attempt per ghost episode
    std::unique_ptr<juce::FileChooser> fileChooser; // the one live async chooser
    PedalSnapshot snapshot;
    // Guards async device-watcher completions: they hop to the message thread
    // and must become no-ops once destruction has begun (the worker may
    // already be gone by the time a DiskArbitration callback lands).
    std::shared_ptr<bool> uiAlive = std::make_shared<bool>(true);
    app::DeviceWatcher deviceWatcher; // before the worker: its probe runs on the worker thread
    PedalWorker worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace loopercat
