// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <felitronics/appkit/BrandHeader.h>
#include <felitronics/appkit/VersionBadge.h>

#include "AppSettings.h"
#include "AudioEngine.h"
#include "PedalMonitor.h"
#include "PlayerPane.h"
#include "SlotTable.h"
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

    // The headless seams (--snapshot / --select): one synchronous scan+apply,
    // programmatic slot selection, and "is the waveform drawn yet".
    void refreshNow();
    void selectSlot(int slot);
    bool playerReady() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void applySnapshot(const PedalSnapshot& snapshot);
    void slotChosen(int rowIndex, bool startPlaying);
    void openAudioSettings();

    // Declaration order is lifetime order: settings outlives the checker
    // (its Config captures it), the checker outlives the badge; the engine
    // outlives the pane that drives it; the monitor is last so its delivery
    // dies before anything it touches.
    AppSettings settings;
    UpdateCheck updateChecker { settings };
    felitronics::appkit::BrandHeader header;
    felitronics::appkit::VersionBadge badge;
    juce::Label status;
    juce::Label hint; // the empty-state prompt, shown while no pedal is mounted
    AudioEngine engine;
    SlotTable table;
    PlayerPane player { engine };
    juce::String deviceError;
    PedalSnapshot snapshot;
    PedalMonitor monitor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace loopercat
