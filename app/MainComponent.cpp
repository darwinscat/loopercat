// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"

#include <felitronics/appkit/AudioSettingsPanel.h>

#include <BinaryData.h>

namespace loopercat
{

namespace
{
    constexpr auto kProductUrl = "https://darwinscat.com/loopercat";
    constexpr auto kDeviceStateKey = "audioDeviceState";

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

    // The audio-settings dialog content; persists the device choice when the
    // dialog goes away (family settings file, one XML-string key).
    struct AudioSettingsHolder final : juce::Component
    {
        AudioSettingsHolder(juce::AudioDeviceManager& dm, AppSettings& s)
            : panel(dm, { .minInputs = 0, .maxInputs = 0, .minOutputs = 2, .maxOutputs = 2 }),
              settings(s)
        {
            addAndMakeVisible(panel);
            setSize(440, 260);
        }

        ~AudioSettingsHolder() override
        {
            if (auto* file = settings.file()) {
                file->setValue(kDeviceStateKey, panel.saveState());
                file->saveIfNeeded();
            }
        }

        void resized() override { panel.setBounds(getLocalBounds().reduced(8)); }

        felitronics::appkit::AudioSettingsPanel panel;
        AppSettings& settings;
    };
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

    const juce::String savedDeviceState =
        settings.file() != nullptr ? settings.file()->getValue(kDeviceStateKey) : juce::String();
    deviceError = engine.initialiseDevice(savedDeviceState);

    table.onSlotSelected = [this](int row) { slotChosen(row, false); };
    table.onSlotActivated = [this](int row) { slotChosen(row, true); };
    player.onGear = [this] { openAudioSettings(); };

    addAndMakeVisible(header);
    addAndMakeVisible(badge);
    addAndMakeVisible(status);
    addAndMakeVisible(hint);
    addChildComponent(table);  // shown once a pedal is mounted
    addChildComponent(player); // likewise

    setWantsKeyboardFocus(true); // Space toggles playback

    applySnapshot({}); // the no-pedal state, until the first scan lands
    setSize(920, 680);

    monitor.start();
}

void MainComponent::refreshNow()
{
    applySnapshot(monitor.scanOnce());
}

void MainComponent::selectSlot(int slot)
{
    table.selectRow(slot - 1);
}

bool MainComponent::playerReady() const
{
    return !engine.hasSource() || player.isThumbnailReady();
}

void MainComponent::applySnapshot(const PedalSnapshot& latest)
{
    snapshot = latest;
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
        juce::String text = juce::String(snapshot.volume) + "  —  " + juce::String(loaded)
                          + " of " + juce::String(snapshot.slots.size()) + " slots hold a loop";
        if (deviceError.isNotEmpty())
            text << "  ·  audio device: " << deviceError;
        status.setText(text, juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kStatusText);
    }

    // Drop the player when its file is no longer on the (still-)mounted pedal.
    if (player.currentPath().isNotEmpty()) {
        bool stillThere = false;
        for (const auto& row : snapshot.slots)
            stillThere = stillThere || juce::String(row.wavPath) == player.currentPath();
        if (!stillThere)
            player.clear();
    }

    table.setRows(snapshot.slots);
    table.setVisible(mounted);
    player.setVisible(mounted);
    hint.setVisible(!mounted);
}

void MainComponent::slotChosen(int rowIndex, bool startPlaying)
{
    if (rowIndex < 0 || static_cast<std::size_t>(rowIndex) >= snapshot.slots.size())
        return;
    const SlotRow& row = snapshot.slots[static_cast<std::size_t>(rowIndex)];

    if (!row.info.hasAudio || row.wavPath.empty()) {
        player.clear(); // an empty slot: nothing to listen to
        return;
    }

    const juce::String path(row.wavPath);
    if (path != player.currentPath()) {
        const juce::String title = juce::String(row.info.slot).paddedLeft('0', 2) + "  "
                                 + juce::String(row.info.name).trimEnd();
        player.setSlot(juce::File(path), title, row.info.oneShot);
    }
    if (startPlaying && engine.hasSource() && !engine.isPlaying())
        engine.play();
}

void MainComponent::openAudioSettings()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new AudioSettingsHolder(engine.deviceManager(), settings));
    options.dialogTitle = "Audio output";
    options.dialogBackgroundColour = kBackground;
    options.escapeKeyTriggersCloseButton = true;
    options.resizable = false;
    options.launchAsync();
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey && engine.hasSource()) {
        engine.togglePlay();
        return true;
    }
    return false;
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
    player.setBounds(area.removeFromBottom(150).reduced(12, 0));
    area.removeFromBottom(8);
    table.setBounds(area.reduced(12, 0));
    hint.setBounds(area);
}

} // namespace loopercat
