// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"

#include <BinaryData.h>

namespace loopercat
{

namespace
{
    constexpr auto kProductUrl = "https://darwinscat.com/loopercat";

    // The window background — the family's near-black stage (the brand mark's
    // dark disc is 0xff0b0b11; the stage sits just above it).
    const juce::Colour kBackground { 0xff121218 };
    const juce::Colour kStatusText { 0xff8a8a92 };
    const juce::Colour kErrorText { 0xffff8a3d }; // brand orange: attention, not alarm

    felitronics::appkit::VersionBadge::Config badgeConfig()
    {
        return { .productName = "LooperCat",
                 .productUrl = kProductUrl,
                 .gitHash = LOOPERCAT_GIT_HASH,
                 .buildNumber = LOOPERCAT_BUILD_NUMBER,
                 .gitDirty = LOOPERCAT_GIT_DIRTY,
                 .os = LOOPERCAT_BUILD_OS,
                 .arch = LOOPERCAT_BUILD_ARCH,
                 .builder = "dev" };
    }
} // namespace

MainComponent::MainComponent(std::string explicitVolume)
    : header(BinaryData::catlogo_svg, BinaryData::catlogo_svgSize,
             BinaryData::MichromaRegular_ttf, BinaryData::MichromaRegular_ttfSize,
             "LooperCat", kProductUrl),
      badge(updateChecker, badgeConfig(), "App"),
      monitor(std::move(explicitVolume), [this](const PedalSnapshot& s) { applySnapshot(s); })
{
    badge.setBrandTypeface(juce::Typeface::createSystemTypefaceFor(
        BinaryData::MichromaRegular_ttf, BinaryData::MichromaRegular_ttfSize));

    status.setFont(juce::FontOptions(12.0f));
    status.setColour(juce::Label::textColourId, kStatusText);

    hint.setText("Connect your looper via USB and enter STORAGE mode",
                 juce::dontSendNotification);
    hint.setFont(juce::FontOptions(15.0f));
    hint.setColour(juce::Label::textColourId, kStatusText);
    hint.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(header);
    addAndMakeVisible(badge);
    addAndMakeVisible(status);
    addAndMakeVisible(hint);
    addChildComponent(table); // shown once a pedal is mounted

    applySnapshot({}); // the no-pedal state, until the first scan lands
    setSize(920, 620);

    monitor.start();
}

void MainComponent::refreshNow()
{
    applySnapshot(monitor.scanOnce());
}

void MainComponent::applySnapshot(const PedalSnapshot& snapshot)
{
    const bool mounted = !snapshot.volume.empty() && snapshot.error.empty();

    if (snapshot.volume.empty()) {
        status.setText("No looper found", juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kStatusText);
    } else if (!snapshot.error.empty()) {
        status.setText(juce::String(snapshot.volume) + " — " + snapshot.error,
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kErrorText);
    } else {
        int loaded = 0;
        for (const auto& row : snapshot.slots)
            loaded += row.info.hasAudio ? 1 : 0;
        status.setText(juce::String(snapshot.volume) + "  —  " + juce::String(loaded) + " of "
                           + juce::String(snapshot.slots.size()) + " slots hold a loop",
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kStatusText);
    }

    table.setRows(snapshot.slots);
    table.setVisible(mounted);
    hint.setVisible(!mounted);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    header.setBounds(area.removeFromTop(56));
    header.clickRight = juce::roundToInt(header.contentRight());
    status.setBounds(area.removeFromTop(28).reduced(12, 2));
    badge.setBounds(getWidth() - 122, getHeight() - 40, 110, 32);
    area.removeFromBottom(44); // the badge strip stays clear
    table.setBounds(area.reduced(12, 0));
    hint.setBounds(area);
}

} // namespace loopercat
