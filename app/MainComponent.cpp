// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"

#include "Strings.h"

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

    juce::String trimmedName(const SlotRow& row)
    {
        return utf8(row.info.name).trimEnd();
    }
} // namespace

MainComponent::MainComponent(std::string explicitVolume)
    : header(BinaryData::catlogo_svg, BinaryData::catlogo_svgSize,
             BinaryData::MichromaRegular_ttf, BinaryData::MichromaRegular_ttfSize,
             "LooperCat", kProductUrl),
      badge(updateChecker, badgeConfig(), "App"),
      worker(std::move(explicitVolume), [this](const PedalSnapshot& s) { applySnapshot(s); })
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
    table.onRowContextMenu = [this](int row, juce::Point<int> at) { showRowMenu(row, at); };
    table.onOneShotToggled = [this](int row) {
        if (pedalBusy || static_cast<std::size_t>(row) >= snapshot.slots.size())
            return;
        const SlotRow& target = snapshot.slots[static_cast<std::size_t>(row)];
        toggleOneShot(target.info.slot, target.info.oneShot);
    };
    table.onRenameCommitted = [this](int row, juce::String newName) {
        if (pedalBusy || static_cast<std::size_t>(row) >= snapshot.slots.size())
            return;
        const int slot = snapshot.slots[static_cast<std::size_t>(row)].info.slot;
        worker.enqueue({ "Rename slot " + juce::String(slot), slot,
                         [name = newName.toStdString(), options = makeWriteOptions(),
                          slot](const volume::fs::path& volumePath) {
                             commands::rename(volumePath, slot, name, options);
                         } });
    };
    table.onWavDropped = [this](int row, juce::String path) {
        if (pedalBusy || static_cast<std::size_t>(row) >= snapshot.slots.size())
            return;
        const SlotRow& target = snapshot.slots[static_cast<std::size_t>(row)];
        pushWav(target.info.slot, path, target.info.hasAudio);
    };
    player.onGear = [this] { openAudioSettings(); };

    banners.onLayoutChange = [this] { resized(); };

    // Wired before start(): the worker reads these from its own thread.
    worker.onBusy = [this](bool busy, int slot) {
        pedalBusy = busy;
        table.setBusySlot(busy ? slot : 0);
        updateStatusText();
    };
    worker.onJobResult = [this](juce::String description, juce::String error) {
        jobError = error.isEmpty() ? juce::String() : description + ": " + error;
        banners.setContent(snapshot.findings, jobError);
    };

    addAndMakeVisible(header);
    addAndMakeVisible(pedalLight); // over the header's right side
    addAndMakeVisible(badge);
    addAndMakeVisible(status);
    addAndMakeVisible(hint);
    addAndMakeVisible(banners);
    addChildComponent(table);  // shown once a pedal is mounted
    addChildComponent(player); // likewise

    setWantsKeyboardFocus(true); // Space toggles playback

    applySnapshot({}); // the no-pedal state, until the first scan lands
    setSize(920, 680);

    worker.start();
}

void MainComponent::refreshNow()
{
    applySnapshot(worker.scanOnce());
}

void MainComponent::selectSlot(int slot)
{
    table.selectRow(slot - 1);
}

bool MainComponent::playerReady() const
{
    return !engine.hasSource() || player.isThumbnailReady();
}

void MainComponent::updateStatusText()
{
    if (snapshot.volume.empty()) {
        status.setText("No looper found", juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kStatusText);
        return;
    }
    if (!snapshot.error.empty()) {
        status.setText(utf8(snapshot.volume + " \xe2\x80\x94 " + snapshot.error),
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kErrorText);
        return;
    }
    int loaded = 0;
    for (const auto& row : snapshot.slots)
        loaded += row.info.hasAudio ? 1 : 0;
    juce::String text = utf8(snapshot.volume + "  \xe2\x80\x94  ") + juce::String(loaded) + " of "
                      + juce::String(snapshot.slots.size()) + " slots hold a loop";
    if (pedalBusy)
        text << juce::String::fromUTF8("  \xc2\xb7  working\xe2\x80\xa6");
    if (deviceError.isNotEmpty())
        text << juce::String::fromUTF8("  \xc2\xb7  audio device: ") << deviceError;
    status.setText(text, juce::dontSendNotification);
    status.setColour(juce::Label::textColourId, kStatusText);
}

void MainComponent::applySnapshot(const PedalSnapshot& latest)
{
    snapshot = latest;
    const bool mounted = !snapshot.volume.empty() && snapshot.error.empty();

    updateStatusText();
    banners.setContent(snapshot.findings, jobError);

    // Drop the player when its file is no longer on the (still-)mounted pedal.
    if (player.currentPath().isNotEmpty()) {
        bool stillThere = false;
        for (const auto& row : snapshot.slots)
            stillThere = stillThere || utf8(row.wavPath) == player.currentPath();
        if (!stillThere)
            player.clear();
    }

    pedalLight.set(mounted, utf8(volume::fs::path(snapshot.volume).filename().string()));

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

    const juce::String path = utf8(row.wavPath);
    if (path != player.currentPath()) {
        const juce::String title = juce::String(row.info.slot).paddedLeft('0', 2) + "  "
                                 + trimmedName(row);
        player.setSlot(juce::File(path), title, row.info.oneShot);
    }
    if (startPlaying && engine.hasSource() && !engine.isPlaying())
        engine.play();
}

// --- mutations ---

commands::WriteOptions MainComponent::makeWriteOptions()
{
    const juce::String stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%dT%H-%M-%S");
    return { .backupRoot = settings.dataDir().getChildFile("backups").getFullPathName().toStdString(),
             .stamp = stamp.toStdString() };
}

void MainComponent::showRowMenu(int rowIndex, juce::Point<int> screenPosition)
{
    if (pedalBusy || rowIndex < 0 || static_cast<std::size_t>(rowIndex) >= snapshot.slots.size())
        return;
    const SlotRow& row = snapshot.slots[static_cast<std::size_t>(rowIndex)];
    const int slot = row.info.slot;
    const bool occupied = row.info.hasAudio;
    const juce::String name = trimmedName(row);

    juce::PopupMenu menu;
    menu.addItem(1, juce::String::fromUTF8("Rename\xe2\x80\xa6"));
    menu.addItem(2, "One Shot", true, row.info.oneShot);
    menu.addItem(3, juce::String::fromUTF8(occupied ? "Replace WAV\xe2\x80\xa6" : "Push WAV here\xe2\x80\xa6"));
    menu.addItem(4, juce::String::fromUTF8("Pull to folder\xe2\x80\xa6"), occupied);
    menu.addSeparator();
    menu.addItem(5, juce::String::fromUTF8("Clear slot\xe2\x80\xa6"));

    const bool oneShotNow = row.info.oneShot;
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetScreenArea({ screenPosition.x, screenPosition.y, 1, 1 }),
        [this, rowIndex, slot, name, occupied, oneShotNow](int choice) {
            switch (choice) {
            case 1: table.startRenameEdit(rowIndex); break; // same in-place editor as double-click
            case 2: toggleOneShot(slot, oneShotNow); break;
            case 3: choosePushWav(slot, occupied); break;
            case 4: pullSlot(slot); break;
            case 5: clearSlot(slot, name); break;
            default: break;
            }
        });
}

void MainComponent::toggleOneShot(int slot, bool currentlyOn)
{
    worker.enqueue({ juce::String(currentlyOn ? "Disable" : "Enable") + " One Shot on slot "
                         + juce::String(slot),
                     slot,
                     [slot, on = !currentlyOn, options = makeWriteOptions()](
                         const volume::fs::path& volumePath) {
                         commands::setOneShot(volumePath, { slot }, on, options);
                     } });
}

void MainComponent::choosePushWav(int slot, bool slotOccupied)
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choose a WAV for slot " + juce::String(slot),
        juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                             [this, slot, slotOccupied](const juce::FileChooser& chooser) {
                                 const juce::File file = chooser.getResult();
                                 if (file == juce::File())
                                     return;
                                 pushWav(slot, file.getFullPathName(), slotOccupied);
                             });
}

void MainComponent::pushWav(int slot, const juce::String& sourcePath, bool slotOccupied)
{
    const auto enqueuePush = [this, slot, sourcePath](bool force) {
        worker.enqueue({ "Push " + juce::File(sourcePath).getFileName() + " to slot "
                             + juce::String(slot),
                         slot,
                         [source = sourcePath.toStdString(), slot, force,
                          options = makeWriteOptions()](const volume::fs::path& volumePath) {
                             commands::push(volumePath, source, slot,
                                            { .force = force, .write = options });
                         } });
    };

    if (!slotOccupied) {
        enqueuePush(false);
        return;
    }
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Replace slot " + juce::String(slot) + "?")
            .withMessage("This slot already holds a loop. The current WAV will be replaced "
                         "(a config backup is taken first).")
            .withButton("Replace")
            .withButton("Cancel"),
        [enqueuePush](int button) {
            if (button == 1)
                enqueuePush(true);
        });
}

void MainComponent::pullSlot(int slot)
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Pull slot " + juce::String(slot) + juce::String::fromUTF8(" to\xe2\x80\xa6"),
        juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
                             [this, slot](const juce::FileChooser& chooser) {
                                 const juce::File dir = chooser.getResult();
                                 if (dir == juce::File())
                                     return;
                                 worker.enqueue(
                                     { "Pull slot " + juce::String(slot), slot,
                                       [slot, dest = dir.getFullPathName().toStdString()](
                                           const volume::fs::path& volumePath) {
                                           commands::pull(volumePath, { slot }, { .dest = dest });
                                       } });
                             });
}

void MainComponent::clearSlot(int slot, const juce::String& name)
{
    const juce::String label = name.isEmpty() ? juce::String(slot)
                                              : juce::String(slot) + " (" + name + ")";
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Clear slot " + label + "?")
            .withMessage("The slot returns to factory state. Its audio moves to the app's "
                         "trash folder first — nothing is destroyed outright.")
            .withButton("Clear")
            .withButton("Cancel"),
        [this, slot](int button) {
            if (button != 1)
                return;
            const auto options = makeWriteOptions();
            worker.enqueue(
                { "Clear slot " + juce::String(slot), slot,
                  [slot, options,
                   trash = settings.dataDir().getChildFile("trash").getFullPathName().toStdString()](
                      const volume::fs::path& volumePath) {
                      commands::clear(volumePath, { slot },
                                      { .trashRoot = trash, .write = options });
                  } });
        });
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
    pedalLight.setBounds(getWidth() - 232, 0, 220, 56);
    status.setBounds(area.removeFromTop(28).reduced(12, 2));
    const int bannerHeight = banners.preferredHeight();
    banners.setBounds(area.removeFromTop(bannerHeight).reduced(12, 0));
    if (bannerHeight > 0)
        area.removeFromTop(6);
    badge.setBounds(getWidth() - 122, getHeight() - 40, 110, 32);
    area.removeFromBottom(44); // the badge strip stays clear
    player.setBounds(area.removeFromBottom(150).reduced(12, 0));
    area.removeFromBottom(8);
    table.setBounds(area.reduced(12, 0));
    hint.setBounds(area);
}

} // namespace loopercat
