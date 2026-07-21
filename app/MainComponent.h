// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <felitronics/appkit/BrandHeader.h>
#include <felitronics/appkit/VersionBadge.h>

#include "AppSettings.h"
#include "PedalMonitor.h"
#include "SlotTable.h"
#include "UpdateCheck.h"

//==============================================================================
// The app's single window content: the family's branded header on top, a
// status strip, the slot browser table (live: rows follow mount/unmount and
// on-volume edits), and the version badge bottom-right.
//==============================================================================
namespace loopercat
{

class MainComponent final : public juce::Component
{
public:
    // `explicitVolume` pins the pedal path (--volume override); empty = autodetect.
    explicit MainComponent(std::string explicitVolume = {});

    // One synchronous scan+apply, for the headless --snapshot path (normal
    // operation only ever updates through the monitor's async delivery).
    void refreshNow();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void applySnapshot(const PedalSnapshot& snapshot);

    // Declaration order is lifetime order: settings outlives the checker
    // (its Config captures it), the checker outlives the badge; the monitor
    // is last so its delivery dies before anything it touches.
    AppSettings settings;
    UpdateCheck updateChecker { settings };
    felitronics::appkit::BrandHeader header;
    felitronics::appkit::VersionBadge badge;
    juce::Label status;
    juce::Label hint; // the empty-state prompt, shown while no pedal is mounted
    SlotTable table;
    PedalMonitor monitor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace loopercat
