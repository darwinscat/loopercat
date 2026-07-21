// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <felitronics/appkit/BrandHeader.h>
#include <felitronics/appkit/VersionBadge.h>

#include "AppSettings.h"
#include "UpdateCheck.h"

//==============================================================================
// The app's single window content: the family's branded header strip on top,
// the version badge (with the opt-in update check) bottom-right, and the
// content area between them — empty until the slot browser lands (Sprint 1).
//==============================================================================
namespace loopercat
{

class MainComponent final : public juce::Component
{
public:
    MainComponent();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    // Declaration order is lifetime order: settings outlives the checker
    // (its Config captures it), the checker outlives the badge.
    AppSettings settings;
    UpdateCheck updateChecker { settings };
    felitronics::appkit::BrandHeader header;
    felitronics::appkit::VersionBadge badge;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace loopercat
