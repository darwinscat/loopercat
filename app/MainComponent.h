// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Connect.hpp>

#include <felitronics/appkit/VersionBadge.h>

#include "AppMenu.h"
#include "AppSettings.h"
#include "AudioEngine.h"
#include "BannerStrip.h"
#include "DeviceWatcher.h"
#include "LooperMark.h"
#include "PedalLight.h"
#include "PedalLink.h"
#include "PedalWorker.h"
#include "PlayerPane.h"
#include "QuitGate.h"
#include "SettingsDialog.h"
#include "SlotInspector.h"
#include "SlotTable.h"
#include "TabStrip.h"
#include "Toast.h"
#include "UpdateCheck.h"

//==============================================================================
// The app's single window content: branded header, status strip, the live
// slot browser, and the listening strip (waveform + transport) for the
// selected slot. Space toggles playback; double-click a row to listen.
//==============================================================================
namespace loopercat
{

// The family's version badge draws two lines — the version over the running
// format — and the window's status row has room for one. The chip crops it to
// the line that matters; the click, the update dot and the popover are the
// badge's own, untouched.
class VersionChip final : public juce::Component
{
public:
    explicit VersionChip(juce::Component& badge) : badge_(badge) { addAndMakeVisible(badge_); }

    void resized() override
    {
        // Tall enough that the format line lands past the chip's edge and is
        // clipped away, with the version line centred in what is left.
        badge_.setBounds(0, 0, getWidth(), juce::roundToInt(getHeight() / 0.56f));
    }

private:
    juce::Component& badge_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VersionChip)
};

class MainComponent final : public juce::Component,
                            private juce::Timer // MIDI presence poll + connect supervision clock
{
public:
    // `explicitVolume` pins the pedal path (--volume override); empty = autodetect.
    explicit MainComponent(std::string explicitVolume = {});
    ~MainComponent() override;

    // The headless seams (--snapshot / --select): one synchronous scan+apply,
    // programmatic slot selection, and "is the waveform drawn yet".
    void refreshNow();
    void selectSlot(int slot);
    void showProperties() { bottomTabs.select(kPropertiesTab); } // --properties, for snapshots
    void showAbout(); // the menu About and --about: opens the version badge's popover
    void setMarkers(double inSeconds, double outSeconds) { player.setMarkers(inSeconds, outSeconds); }
    bool playerReady() const;

    // Quit is Disconnect (issue #1): while the volume is held, release it
    // first. Returns false when nothing holds the quit; otherwise starts (or
    // joins) the release and fires `done` exactly once — on the eject
    // completion or at the time bound, whichever lands first.
    bool beginQuitDisconnect(std::function<void()> done);

    // The --cycle seam: press Connect, press Disconnect, and read back what
    // the window would be showing. The two buttons call THESE, so the seam
    // can never drift into testing a copy of the real thing — the failure
    // mode of every verification hook that quietly stops meaning anything.
    // playSlot starts a slot the way a double-click does, so the read-ahead
    // thread holds a WAV open: the state a real Disconnect has to survive.
    void playSlot(int slot) { slotChosen(slot, true); }
    void beginConnect();
    void beginDisconnect();
    std::string lifecycleStateName() const;
    std::string volumePath() const;
    std::vector<std::string> bannerLines() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    static constexpr int kAudioTab = 0, kPropertiesTab = 1; // the bottom pane's two faces

    void applySnapshot(const PedalSnapshot& snapshot);
    void slotChosen(int slot, bool startPlaying);
    void openSettings();
    void updateStatusText();
    juce::String volumeDisplayName() const;
    void updateTableRows();
    void updateToolbar();
    void cleanUpGhostMount();
    void timerCallback() override; // presence poll, attempt ticks, hold expiry, cadence
    void pollMidiPresence();       // the pedal visible outside STORAGE

    // The supervised Connect (issue #2): the attempt machine owns the
    // retry/give-up policy, these own the clock, the MIDI send and the UI.
    void startConnectAttempt();
    void sendEnterStorage();
    void tickConnectAttempt();
    void endConnectAttempt();
    bool connectHoldActive() const;

    void runBackup();
    void runCleanJunk();
    const SlotRow* slotRowFor(int slot) const; // null when unmounted/out of range

    // Mutations: every action becomes a queued worker job with the standard
    // write options (backup root + timestamp under the app data dir).
    void showSlotMenu(int slot, juce::Point<int> screenPosition);
    void showBottomTab(int index);          // Audio (the player) or Properties (the slot)
    void updateInspector();                 // push the selected row into the panel
    void applyColumnPreferences();          // Settings -> Columns, onto the table
    void toggleOneShot(int slot, bool currentlyOn);
    void toggleCountIn(int slot, bool currentlyOn);
    void pushWav(int slot, const juce::String& sourcePath, bool slotOccupied);
    void releasePlayerIfHolding(int slotA, int slotB);
    void choosePushWav(int slot, bool slotOccupied);
    void pullSlot(int slot);
    void clearSlot(int slot, const juce::String& name);
    void downmixSlot(int slot, const juce::String& name, wav::Placement placement);
    commands::WriteOptions makeWriteOptions();

    // Declaration order is lifetime order: settings outlives the checker
    // (its Config captures it), the checker outlives the badge; the engine
    // outlives the pane that drives it; the monitor is last so its delivery
    // dies before anything it touches.
    AppSettings settings;
    UpdateCheck updateChecker { settings };
    ui::LooperBrandHeader header;
    felitronics::appkit::VersionBadge badge;
    VersionChip versionChip { badge }; // after the badge: it parents it
    PedalLight pedalLight;
    juce::Label status;
    juce::Label devMark; // "dev" when this build is ahead of the last release
    juce::Label hint; // the empty-state prompt, shown while no pedal is mounted
    AudioEngine engine;
    BannerStrip banners;
    juce::TextButton connectButton { "Connect" };
    juce::TextButton disconnectButton { "Disconnect" };
    juce::ToggleButton showEmptyToggle { "show empty slots" };
    felitronics::appkit::brand::GearButton settingsButton; // app settings, by the pedal light
    SlotTable table;
    TabStrip bottomTabs { { "Audio", "Properties" } };
    SlotInspector inspector;
    Toast toast;
    PlayerPane player { engine };
    juce::String deviceError;
    int selectedSlot = 0;        // what the Properties tab is showing (0 = nothing)
    bool pedalBusy = false;
    bool ghostCleanupStarted = false; // one cleanup attempt per ghost episode
    bool midiPedalPresent = false;    // the RC-5 as a USB-MIDI device (normal mode)
    connect::Attempt connectAttempt;  // the supervised Connect (issue #2)
    juce::String lastConnectSendError; // last enter-storage send result — the honest give-up
    std::int64_t connectHoldUntilMs = 0; // Connect held while the pedal re-boots its MIDI face
    std::unique_ptr<juce::FileChooser> fileChooser; // the one live async chooser
    PedalSnapshot snapshot;
    QuitGate quitGate; // quit-is-Disconnect (issue #1): decision + exactly-once exit
    // Guards async device-watcher completions: they hop to the message thread
    // and must become no-ops once destruction has begun (the worker may
    // already be gone by the time a DiskArbitration callback lands).
    std::shared_ptr<bool> uiAlive = std::make_shared<bool>(true);
    std::unique_ptr<AppMenu> appMenu; // after the components its actions touch
    app::DeviceWatcher deviceWatcher; // before the worker: its probe runs on the worker thread
    PedalWorker worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace loopercat
