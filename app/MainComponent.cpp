// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"

#include "Strings.h"

#include <loopercat/Wav.hpp>

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
                 .builder = "dev",
                 // The popover mirrors the window header: the ears, not the
                 // family-default orbit the hook falls back to.
                 .drawMark = [](juce::Graphics& g, float cx, float cy, float d) {
                     ui::drawLoopMark(g, cx, cy, d);
                 } };
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

    table.onSlotSelected = [this](int slot) { slotChosen(slot, false); };
    table.onSlotActivated = [this](int slot) { slotChosen(slot, true); };
    table.onSlotContextMenu = [this](int slot, juce::Point<int> at) { showSlotMenu(slot, at); };
    table.onOneShotToggled = [this](int slot) {
        if (const SlotRow* row = pedalBusy ? nullptr : slotRowFor(slot))
            toggleOneShot(slot, row->info.oneShot);
    };
    table.onRenameCommitted = [this](int slot, juce::String newName) {
        if (pedalBusy || slotRowFor(slot) == nullptr)
            return;
        worker.enqueue({ "Rename slot " + juce::String(slot), slot,
                         [name = newName.toStdString(), options = makeWriteOptions(),
                          slot](const volume::fs::path& volumePath) {
                             commands::rename(volumePath, slot, name, options);
                         } });
    };
    table.onWavDropped = [this](int slot, juce::String path) {
        if (const SlotRow* row = pedalBusy ? nullptr : slotRowFor(slot))
            pushWav(slot, path, row->info.hasAudio);
    };
    table.onEmptyWavCellClicked = [this](int slot) {
        if (!pedalBusy && slotRowFor(slot) != nullptr)
            choosePushWav(slot, false);
    };
    player.onGear = [this] { openAudioSettings(); };
    player.onTrim = [this](int slot, juce::int64 inFrame, juce::int64 outFrame) {
        if (pedalBusy || slotRowFor(slot) == nullptr)
            return;
        const double seconds = static_cast<double>(outFrame - inFrame) / wav::kSampleRate;
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle("Trim slot " + juce::String(slot) + "?")
                .withMessage(juce::String::fromUTF8("The loop becomes the selected ")
                             + juce::String(seconds, 1)
                             + juce::String::fromUTF8(" s. The original WAV moves to the app's "
                                                      "trash first \xe2\x80\x94 that is your undo."))
                .withButton("Trim")
                .withButton("Cancel"),
            [this, slot, inFrame, outFrame](int button) {
                if (button != 1)
                    return;
                const auto options = makeWriteOptions();
                worker.enqueue(
                    { "Trim slot " + juce::String(slot), slot,
                      [slot, inFrame, outFrame, options,
                       trash = settings.dataDir().getChildFile("trash").getFullPathName().toStdString()](
                          const volume::fs::path& volumePath) {
                          commands::trim(volumePath, slot, inFrame, outFrame,
                                         { .trashRoot = trash, .write = options });
                      } });
            });
    };

    // The toolbar (reference web UI parity): manual config backup, junk
    // sweep, and the empty-slot view filter (persisted).
    for (auto* button : { &backupButton, &cleanButton, &ejectButton }) {
        button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1e1e26));
        button->setEnabled(false);
    }
    ejectButton.onClick = [this] {
        // Release our own hold on the volume first: the read-ahead thread
        // keeps the slot WAV open, and an open file dissents the unmount.
        engine.stop();
        player.clear();
        worker.requestEject();
    };
    backupButton.onClick = [this] {
        worker.enqueue({ "Backup configs", 0,
                         [options = makeWriteOptions()](const volume::fs::path& volumePath) {
                             commands::backup(volumePath, options.backupRoot, options.stamp);
                         } });
    };
    cleanButton.onClick = [this] {
        worker.enqueue({ "Clean junk", 0, [](const volume::fs::path& volumePath) {
                             volume::sweepJunk(volumePath);
                         } });
    };
    showEmptyToggle.setColour(juce::ToggleButton::textColourId, kStatusText);
    showEmptyToggle.setColour(juce::ToggleButton::tickColourId,
                              felitronics::appkit::brand::violet);
    showEmptyToggle.setToggleState(settings.file() == nullptr
                                       || settings.file()->getBoolValue("showEmptySlots", true),
                                   juce::dontSendNotification);
    showEmptyToggle.onClick = [this] {
        if (auto* file = settings.file()) {
            file->setValue("showEmptySlots", showEmptyToggle.getToggleState());
            file->saveIfNeeded();
        }
        updateTableRows();
    };

    banners.onLayoutChange = [this] { resized(); };

    // Device truth (issue #17). The watcher's async callbacks land on its own
    // dispatch queue; they hop to the message thread under the uiAlive token
    // and only then touch the worker — by destruction time they are no-ops.
    deviceWatcher.onDeviceLost = [this, alive = uiAlive] {
        juce::MessageManager::callAsync([this, alive] {
            if (*alive)
                worker.postDeviceLost();
        });
    };
    deviceWatcher.onDiskAppeared = [this, alive = uiAlive] {
        juce::MessageManager::callAsync([this, alive] {
            if (*alive)
                worker.pokeRescan();
        });
    };
    deviceWatcher.onAutoMountResult = [this, alive = uiAlive](bool ok, std::string message) {
        juce::MessageManager::callAsync([this, alive, ok, note = utf8(message)] {
            if (!*alive)
                return;
            if (ok) {
                toast.show("Mounted " + note);
                worker.pokeRescan();
            } else {
                jobError = "Mount: " + note;
                refreshBanners();
            }
        });
    };
    worker.setBackingProbe([this](const volume::fs::path& path) {
        // MEMORY1.RC0 is the probe target: small, always present on a real
        // pedal, and its uncached readability is the device truth.
        return deviceWatcher.verdict(path, volume::memoryPath(path, 1));
    });
    worker.setEjectStarter([this](const std::string& volumePath) {
        // Worker thread. The completion reports through the message thread.
        deviceWatcher.eject(volumePath, [this, alive = uiAlive](bool unmounted, std::string message) {
            juce::MessageManager::callAsync(
                [this, alive, unmounted, note = utf8(message)] {
                    if (!*alive)
                        return;
                    worker.postEjectFinished(unmounted);
                    if (!unmounted) {
                        jobError = "Eject: " + note;
                        refreshBanners();
                    } else if (note.isNotEmpty()) {
                        toast.show(note);
                    }
                });
        });
    });

    // Wired before start(): the worker reads these from its own thread.
    worker.onBusy = [this](bool busy, int slot) {
        pedalBusy = busy;
        table.setBusySlot(busy ? slot : 0);
        updateStatusText();
        updateToolbar();
    };
    worker.onJobResult = [this](juce::String description, juce::String error) {
        jobError = error.isEmpty() ? juce::String() : description + ": " + error;
        refreshBanners();
        if (error.isEmpty()) {
            juce::String note = description + juce::String::fromUTF8(" \xe2\x80\x94 done");
            const bool wroteToPedal = description.startsWith("Rename")
                                   || description.startsWith("Enable")
                                   || description.startsWith("Disable")
                                   || description.startsWith("Push")
                                   || description.startsWith("Trim")
                                   || description.startsWith("Clear");
            if (description.startsWith("Trim"))
                player.reload(); // same path, new bytes — fresh reader + thumbnail
            if (wroteToPedal)
                note << ". Eject the volume and reboot the pedal to apply.";
            toast.show(note);
        }
    };

    addAndMakeVisible(header);
    addAndMakeVisible(pedalLight); // over the header's right side
    addAndMakeVisible(badge);
    addAndMakeVisible(status);
    addAndMakeVisible(hint);
    addAndMakeVisible(banners);
    addAndMakeVisible(backupButton);
    addAndMakeVisible(cleanButton);
    addAndMakeVisible(ejectButton);
    addAndMakeVisible(showEmptyToggle);
    addChildComponent(table);  // shown once a pedal is mounted
    addChildComponent(player); // likewise
    addChildComponent(toast);  // fades in over everything on job success

    setWantsKeyboardFocus(true); // Space toggles playback

    applySnapshot({}); // the no-pedal state, until the first scan lands
    setSize(920, 680);

    worker.start();
    startTimer(2000); // MIDI presence poll — cheap device-list scan, message thread
}

// The pedal outside STORAGE is still visible — as a USB-MIDI device. Knowing
// the difference between "no pedal at all" and "pedal here, wrong mode" turns
// the empty state from a shrug into an instruction.
void MainComponent::timerCallback()
{
    bool present = false;
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        present = present || device.name.containsIgnoreCase("RC-5");
    if (present == midiPedalPresent)
        return;
    midiPedalPresent = present;
    hint.setText(midiPedalPresent
                     ? juce::String::fromUTF8("RC-5 detected in normal mode \xe2\x80\x94 put the "
                                              "pedal into STORAGE mode to browse loops")
                     : juce::String("Connect your looper via USB and enter STORAGE mode"),
                 juce::dontSendNotification);
    updateStatusText();
}

MainComponent::~MainComponent()
{
    *uiAlive = false; // message thread; queued watcher completions become no-ops
}

void MainComponent::refreshNow()
{
    applySnapshot(worker.scanOnce());
}

void MainComponent::selectSlot(int slot)
{
    table.selectSlot(slot);
}

bool MainComponent::playerReady() const
{
    return !engine.hasSource() || player.isThumbnailReady();
}

const SlotRow* MainComponent::slotRowFor(int slot) const
{
    if (slot < 1 || static_cast<std::size_t>(slot) > snapshot.slots.size())
        return nullptr;
    return &snapshot.slots[static_cast<std::size_t>(slot - 1)];
}

void MainComponent::updateTableRows()
{
    if (showEmptyToggle.getToggleState()) {
        table.setRows(snapshot.slots);
        return;
    }
    std::vector<SlotRow> visible;
    for (const auto& row : snapshot.slots)
        if (row.info.hasAudio || !row.wavFile.empty())
            visible.push_back(row);
    table.setRows(std::move(visible));
}

void MainComponent::updateToolbar()
{
    const bool usable = snapshot.state == lifecycle::State::connected && snapshot.error.empty();
    backupButton.setEnabled(usable && !pedalBusy);
    cleanButton.setEnabled(usable && !pedalBusy);
    ejectButton.setEnabled(usable && !pedalBusy);
}

// The banner strip: lifecycle first (it explains everything below), then the
// doctor findings, then the last failed job.
void MainComponent::refreshBanners()
{
    std::vector<commands::Finding> shown = snapshot.findings;
    switch (snapshot.state) {
    case lifecycle::State::ghost:
        shown.insert(shown.begin(),
                     { commands::Level::error,
                       "Pedal detached without eject \xe2\x80\x94 the volume was serving cached "
                       "data and nothing reached the pedal. Cleaning up the stale mount\xe2\x80\xa6" });
        break;
    case lifecycle::State::ejected:
        shown.insert(shown.begin(),
                     { commands::Level::info,
                       "Volume ejected \xe2\x80\x94 safe to disconnect the pedal." });
        break;
    case lifecycle::State::disconnected:
    case lifecycle::State::connected:
    case lifecycle::State::ejecting:
        break;
    }
    banners.setContent(shown, jobError);
}

// A ghost cannot heal by itself: the stale mount blocks the next attach.
// Force-unmount is safe by construction (nothing flushes to a dead device);
// one attempt per episode, failures land on the banner line.
void MainComponent::cleanUpGhostMount()
{
    deviceWatcher.forceUnmount(
        snapshot.volume, [this, alive = uiAlive](bool ok, std::string message) {
            juce::MessageManager::callAsync([this, alive, ok, note = utf8(message)] {
                if (!*alive)
                    return;
                if (!ok) {
                    jobError = "Ghost cleanup: " + note;
                    refreshBanners();
                }
                worker.pokeRescan();
            });
        });
}

void MainComponent::updateStatusText()
{
    if (snapshot.state == lifecycle::State::ghost) {
        status.setText(utf8(snapshot.volume + " \xe2\x80\x94 device detached (ghost mount)"),
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kErrorText);
        return;
    }
    if (snapshot.state == lifecycle::State::ejecting) {
        status.setText(juce::String::fromUTF8("Ejecting\xe2\x80\xa6"), juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kStatusText);
        return;
    }
    if (snapshot.state == lifecycle::State::ejected) {
        status.setText(juce::String::fromUTF8(
                           "Ejected \xe2\x80\x94 safe to disconnect the pedal"),
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kStatusText);
        return;
    }
    if (snapshot.volume.empty()) {
        status.setText(midiPedalPresent
                           ? juce::String::fromUTF8(
                                 "RC-5 connected in normal mode \xe2\x80\x94 not in STORAGE")
                           : juce::String("No looper found"),
                       juce::dontSendNotification);
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
    if (snapshot.freeBytes > 0)
        text << juce::String::fromUTF8("  \xc2\xb7  ")
             << juce::String(static_cast<double>(snapshot.freeBytes) / 1.0e9, 1) << " GB free";
    if (pedalBusy)
        text << juce::String::fromUTF8("  \xc2\xb7  working\xe2\x80\xa6");
    if (deviceError.isNotEmpty())
        text << juce::String::fromUTF8("  \xc2\xb7  audio device: ") << deviceError;
    status.setText(text, juce::dontSendNotification);
    status.setColour(juce::Label::textColourId, kStatusText);
}

void MainComponent::applySnapshot(const PedalSnapshot& latest)
{
    const lifecycle::State previousState = snapshot.state;
    snapshot = latest;
    // "Mounted" means honestly connected — a ghost lists slots too, but they
    // are page-cache fiction and nothing may play from or write to them.
    const bool mounted = snapshot.state == lifecycle::State::connected && snapshot.error.empty();

    updateStatusText();
    refreshBanners();

    // Anything but connected drops playback: the reader would stream from a
    // dead mount (ghost) or hold the volume open against an eject.
    if (snapshot.state != lifecycle::State::connected) {
        player.clear();
    } else if (player.currentPath().isNotEmpty()) {
        // Drop the player when its file is no longer on the mounted pedal.
        bool stillThere = false;
        for (const auto& row : snapshot.slots)
            stillThere = stillThere || utf8(row.wavPath) == player.currentPath();
        if (!stillThere)
            player.clear();
    }

    if (snapshot.state == lifecycle::State::ghost) {
        if (!ghostCleanupStarted) {
            ghostCleanupStarted = true;
            cleanUpGhostMount();
        }
    } else if (previousState == lifecycle::State::ghost) {
        ghostCleanupStarted = false; // the episode is over
    }

    pedalLight.set(mounted, utf8(volume::fs::path(snapshot.volume).filename().string()));

    updateTableRows();
    updateToolbar();
    table.setVisible(mounted);
    player.setVisible(mounted);
    hint.setVisible(!mounted);
}

void MainComponent::slotChosen(int slot, bool startPlaying)
{
    const SlotRow* found = slotRowFor(slot);
    if (found == nullptr)
        return;
    const SlotRow& row = *found;

    if (!row.info.hasAudio || row.wavPath.empty()) {
        player.clear(); // an empty slot: nothing to listen to
        return;
    }

    const juce::String path = utf8(row.wavPath);
    if (path != player.currentPath()) {
        const juce::String title = juce::String(row.info.slot).paddedLeft('0', 2) + "  "
                                 + trimmedName(row);
        player.setSlot(row.info.slot, juce::File(path), title, row.info.oneShot, row.info.frames);
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

void MainComponent::showSlotMenu(int slot, juce::Point<int> screenPosition)
{
    const SlotRow* found = pedalBusy ? nullptr : slotRowFor(slot);
    if (found == nullptr)
        return;
    const SlotRow& row = *found;
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
        [this, slot, name, occupied, oneShotNow](int choice) {
            switch (choice) {
            case 1: table.startRenameEditForSlot(slot); break; // same in-place editor as double-click
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
    // The reference UI's trash choice, as explicit buttons: the default path
    // keeps a safety copy; permanent deletion says so in plain words.
    auto* dialog = new juce::AlertWindow(
        "Clear slot " + label + "?",
        juce::String::fromUTF8("The slot returns to factory state.\n\n"
                               "\xe2\x80\xa2 Move to trash \xe2\x80\x94 the WAV is kept in the app's "
                               "trash folder on this computer.\n"
                               "\xe2\x80\xa2 Delete permanently \xe2\x80\x94 no copy is kept. "
                               "This cannot be undone."),
        juce::MessageBoxIconType::WarningIcon);
    dialog->addButton("Move to trash", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Delete permanently", 2);
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([this, slot](int choice) {
        if (choice != 1 && choice != 2)
            return;
        const bool useTrash = choice == 1;
        const auto options = makeWriteOptions();
        worker.enqueue(
            { juce::String("Clear slot ") + juce::String(slot), slot,
              [slot, options, useTrash,
               trash = settings.dataDir().getChildFile("trash").getFullPathName().toStdString()](
                  const volume::fs::path& volumePath) {
                  commands::clear(volumePath, { slot },
                                  { .trashRoot = useTrash ? trash : std::string(),
                                    .trash = useTrash,
                                    .write = options });
              } });
    }), true);
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
    auto statusRow = area.removeFromTop(28).reduced(12, 2);
    showEmptyToggle.setBounds(statusRow.removeFromRight(150));
    ejectButton.setBounds(statusRow.removeFromRight(70).reduced(2, 1));
    cleanButton.setBounds(statusRow.removeFromRight(92).reduced(2, 1));
    backupButton.setBounds(statusRow.removeFromRight(78).reduced(2, 1));
    status.setBounds(statusRow);
    const int bannerHeight = banners.preferredHeight();
    banners.setBounds(area.removeFromTop(bannerHeight).reduced(12, 0));
    if (bannerHeight > 0)
        area.removeFromTop(6);
    badge.setBounds(getWidth() - 122, getHeight() - 40, 110, 32);
    area.removeFromBottom(44); // the badge strip stays clear
    toast.setBounds(getWidth() / 2 - 280, getHeight() - 44 - 40, 560, 34);
    player.setBounds(area.removeFromBottom(150).reduced(12, 0));
    area.removeFromBottom(8);
    table.setBounds(area.reduced(12, 0));
    hint.setBounds(area);
}

} // namespace loopercat
