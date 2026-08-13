// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"

#include "Strings.h"
#include "WavImport.h"

#include <loopercat/Wav.hpp>

#include <felitronics/appkit/AudioSettingsPanel.h>

#include <BinaryData.h>

namespace loopercat
{

namespace
{
    constexpr auto kProductUrl = "https://darwinscat.com/loopercat";

    // Which behaviour columns the table shows. One Shot is on out of the box
    // because most players use it; Play Count-In waits to be asked for.
    constexpr auto kOneShotColumnKey = "columnOneShot";
    constexpr auto kCountInColumnKey = "columnPlayCountIn";

    // The window background — the family's near-black stage (the brand mark's
    // dark disc is 0xff0b0b11; the stage sits just above it).
    const juce::Colour kBackground { 0xff121218 };
    const juce::Colour kStatusText { 0xff8a8a92 };
    const juce::Colour kErrorText { 0xffff8a3d }; // brand orange: attention, not alarm

    // One timer, two paces: the MIDI presence poll idles at 2 s; while a
    // connect attempt or the post-disconnect hold is live it runs at
    // supervision pace so the resend clock and the hold expiry stay honest.
    // The bottom pane: the old 150 px of player, plus the tab strip above it,
    // plus the breathing room the version badge used to occupy.
    constexpr int kBottomPaneHeight = 190;

    constexpr int kMidiPollIntervalMs = 2000;
    constexpr int kSuperviseTickMs = 250;

    // How long Connect stays held after a disconnect: the pedal re-boots its
    // MIDI face on leaving STORAGE, and a frame sent into that window is
    // silently lost (issue #2 — a click after a short pause always worked).
    constexpr std::int64_t kReenumerateHoldMs = 2500;

    // The supervision clock: milliseconds, monotonic since app start (the
    // 32-bit counter would wrap under a long-running session).
    std::int64_t nowMs()
    {
        return static_cast<std::int64_t>(juce::Time::getMillisecondCounterHiRes());
    }

    // How long a quit waits for the release before leaving anyway: a busy or
    // wedged volume must not hold the exit hostage (issue #1) — past the
    // bound the pedal simply stays in STORAGE, as an unsupervised quit left
    // it before.
    constexpr int kQuitReleaseBoundMs = 5000;

    felitronics::appkit::VersionBadge::Config badgeConfig()
    {
        return { .productName = "LooperCat",
                 .productUrl = kProductUrl,
                 .gitHash = LOOPERCAT_GIT_HASH,
                 .buildNumber = LOOPERCAT_BUILD_NUMBER,
                 .buildCount = LOOPERCAT_BUILD_COUNT,
                 .gitDirty = LOOPERCAT_GIT_DIRTY,
                 .os = LOOPERCAT_BUILD_OS,
                 .arch = LOOPERCAT_BUILD_ARCH,
                 .builder = LOOPERCAT_BUILDER,
                 // The popover mirrors the window header: the ears, not the
                 // family-default orbit the hook falls back to.
                 .drawMark = [](juce::Graphics& g, float cx, float cy, float d) {
                     ui::drawLoopMark(g, cx, cy, d);
                 } };
    }

    juce::String trimmedName(const SlotRow& row)
    {
        return utf8(row.info.name).trimEnd();
    }

    // The pedal time-stretches when a slot's Tempo leaves its RecTmp; the
    // preview plays the file as recorded and says so instead of implying the
    // tempo change was ignored (issue #29).
    juce::String tempoNoteFor(const catalog::SlotInfo& info)
    {
        if (!info.hasAudio || info.recTempoTenths <= 0
            || info.tempoTenths == info.recTempoTenths)
            return {};
        return "pedal: " + SlotTable::formatTempo(info.tempoTenths)
             + juce::String::fromUTF8(" BPM \xc2\xb7 preview: recorded tempo");
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

    // A build ahead of the last release tag, or with uncommitted changes, is
    // not what anyone downloaded — the corner says so next to the version.
    // So is a build whose provenance could not be established at all (no
    // reachable .git: a source tarball, a packager's tree): unproven is not
    // the same as clean, and only a proven release may go unmarked.
    devMark.setText(LOOPERCAT_BUILD_COUNT > 0 || LOOPERCAT_GIT_DIRTY || !LOOPERCAT_GIT_KNOWN
                        ? "dev"
                        : "",
                    juce::dontSendNotification);
    devMark.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    devMark.setJustificationType(juce::Justification::centredRight);
    devMark.setColour(juce::Label::textColourId, felitronics::appkit::brand::orange);

    status.setFont(juce::FontOptions(12.0f));
    status.setColour(juce::Label::textColourId, kStatusText);

    hint.setText("Connect your looper via USB", juce::dontSendNotification);
    hint.setFont(juce::FontOptions(15.0f));
    hint.setColour(juce::Label::textColourId, kStatusText);
    hint.setJustificationType(juce::Justification::centred);

    const juce::String savedDeviceState =
        settings.file() != nullptr ? settings.file()->getValue(SettingsDialog::kDeviceStateKey)
                                   : juce::String();
    deviceError = engine.initialiseDevice(savedDeviceState);

    table.onSlotSelected = [this](int slot) {
        selectedSlot = slot;
        slotChosen(slot, false);
        updateInspector();
    };
    table.onSlotActivated = [this](int slot) { slotChosen(slot, true); };
    table.onSlotContextMenu = [this](int slot, juce::Point<int> at) { showSlotMenu(slot, at); };
    table.onOneShotToggled = [this](int slot) {
        if (const SlotRow* row = pedalBusy ? nullptr : slotRowFor(slot))
            toggleOneShot(slot, row->info.oneShot);
    };
    table.onCountInToggled = [this](int slot) {
        if (const SlotRow* row = pedalBusy ? nullptr : slotRowFor(slot))
            toggleCountIn(slot, row->info.countIn);
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
    table.onTempoCommitted = [this](int slot, long long tenths) {
        if (pedalBusy || slotRowFor(slot) == nullptr)
            return;
        worker.enqueue({ "Set tempo on slot " + juce::String(slot), slot,
                         [slot, tenths, options = makeWriteOptions()](
                             const volume::fs::path& volumePath) {
                             commands::setTempo(volumePath, slot, tenths, options);
                         } });
    };
    table.onWavDropped = [this](int slot, juce::String path) {
        if (const SlotRow* row = pedalBusy ? nullptr : slotRowFor(slot))
            pushWav(slot, path, row->info.hasAudio);
    };
    table.onSwapRequested = [this](int from, int to) {
        if (pedalBusy || slotRowFor(from) == nullptr || slotRowFor(to) == nullptr)
            return;
        // Grabbing a row selected it, and selection starts reading its whole
        // wav for the waveform — while the FSKit msdos volume serves one
        // request at a time, so the swap's own I/O would sit behind that read
        // until it runs dry (observed live 2026-07-26). Release the bulk
        // read before writing; paced playback reads are harmless.
        if (!player.isThumbnailReady())
            player.clear();
        releasePlayerIfHolding(from, to); // the swap renames their folders (issue #26)
        worker.enqueue({ "Swap slots " + juce::String(from) + " and " + juce::String(to), from,
                         [from, to, options = makeWriteOptions()](
                             const volume::fs::path& volumePath) {
                             commands::swap(volumePath, from, to, options);
                         } });
    };
    table.onEmptyWavCellClicked = [this](int slot) {
        if (!pedalBusy && slotRowFor(slot) != nullptr)
            choosePushWav(slot, false);
    };
    inspector.onRenameCommitted = [this](int slot, juce::String newName) {
        if (table.onRenameCommitted)
            table.onRenameCommitted(slot, newName);
    };
    inspector.onTempoCommitted = [this](int slot, long long tenths) {
        if (table.onTempoCommitted)
            table.onTempoCommitted(slot, tenths);
    };
    inspector.onOneShotToggled = [this](int slot) {
        if (const SlotRow* row = pedalBusy ? nullptr : slotRowFor(slot))
            toggleOneShot(slot, row->info.oneShot);
    };
    inspector.onCountInToggled = [this](int slot) {
        if (const SlotRow* row = pedalBusy ? nullptr : slotRowFor(slot))
            toggleCountIn(slot, row->info.countIn);
    };

    settingsButton.onClick = [this] { openSettings(); };
    bottomTabs.onTabChanged = [this](int index) { showBottomTab(index); };

    player.onVolumeChanged = [this](double percent) {
        if (auto* file = settings.file())
            file->setValue("previewVolume", percent);
    };
    if (auto* file = settings.file())
        player.setVolume(file->getDoubleValue("previewVolume", 100.0));
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
                // Keep the pane's state: the completion path reload()s the
                // trimmed bytes into the same slot view.
                player.releaseFile();
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

    // The toolbar carries only the primary story — Connect / Disconnect and
    // the empty-slot view filter; service actions live in the Maintenance
    // menu, About in the app menu (platform standard).
    for (auto* button : { &connectButton, &disconnectButton }) {
        button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1e1e26));
        button->setEnabled(false);
    }
    connectButton.onClick = [this] { startConnectAttempt(); };
    disconnectButton.onClick = [this] {
        // Release our own hold on the volume first: the read-ahead thread
        // keeps the slot WAV open, and an open file dissents the unmount.
        // The eject completion then walks the pedal out of STORAGE.
        engine.stop();
        player.clear();
        worker.requestEject();
    };
    appMenu = std::make_unique<AppMenu>(AppMenu::Actions {
        .about = [this] { showAbout(); },
        .backup = [this] { runBackup(); },
        .cleanJunk = [this] { runCleanJunk(); },
        .maintenanceEnabled = [this] {
            return snapshot.state == lifecycle::State::connected && snapshot.error.empty()
                && !pedalBusy;
        } });

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
            if (!*alive)
                return;
            // The pedal surrendered the medium — the attempt stops resending;
            // the mount + scan pipeline reports its own failures from here.
            connectAttempt.diskAppeared();
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
                endConnectAttempt(); // the attempt's outcome is this banner
                banners.showError(banners::Source::connection,
                                  "The pedal's card would not mount (" + note
                                      + juce::String::fromUTF8(") \xe2\x80\x94 unplug the USB "
                                                               "cable, then plug it back."));
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
                        juce::String text = juce::String::fromUTF8(
                            "The volume would not eject \xe2\x80\x94 something is still using "
                            "the card. Press Disconnect again in a moment.");
                        if (note.isNotEmpty())
                            text << " (" << note << ")";
                        banners.showError(banners::Source::connection, text);
                        quitGate.finish(); // a refused eject must not hold the exit
                        return;
                    }
                    if (note.isNotEmpty())
                        toast.show(note);
                    // The volume is safely ejected — now walk the pedal out
                    // of STORAGE too, and close the lifecycle: the medium
                    // story is over, which is exactly what deviceLost means.
                    const juce::String exitError = pedallink::requestStorageMode(false);
                    if (exitError.isNotEmpty()) {
                        banners.showError(
                            banners::Source::connection,
                            "Volume ejected, but the exit call did not reach the pedal ("
                                + exitError
                                + juce::String::fromUTF8(") \xe2\x80\x94 leave STORAGE on the "
                                                         "pedal itself."));
                        quitGate.finish(); // the volume is safe — the exit may proceed
                        return;
                    }
                    // Leaving STORAGE re-boots the pedal's MIDI face; hold
                    // Connect until it is honestly back (issue #2) — the
                    // presence poll re-discovers it at supervision pace.
                    midiPedalPresent = false;
                    connectHoldUntilMs = nowMs() + kReenumerateHoldMs;
                    startTimer(kSuperviseTickMs);
                    worker.postDeviceLost();
                    toast.show(juce::String::fromUTF8(
                        "Pedal disconnected \xe2\x80\x94 back on the looper screen"));
                    quitGate.finish(); // the pedal is walking home — quit may proceed
                });
        });
    });

    // Wired before start(): the worker reads these from its own thread.
    worker.onBusy = [this](bool busy, int slot) {
        pedalBusy = busy;
        table.setBusySlot(busy ? slot : 0);
        inspector.setBusy(busy);
        updateStatusText();
        updateToolbar();
    };
    worker.onJobResult = [this](juce::String description, juce::String error) {
        if (error.isNotEmpty()) {
            banners.showError(banners::Source::job, description + ": " + error);
            return;
        }
        banners.clearJobError(); // a later mutation succeeded — the story moved on
        juce::String note = description + juce::String::fromUTF8(" \xe2\x80\x94 done");
        // The pedal re-reads its memory on leaving STORAGE — settings and
        // audio alike (hardware, 2026-08-11: a rename, a tempo change and a
        // trim all reached the pedal with no power cycle). Disconnect is the
        // whole story, so nobody gets sent to the wall plug.
        const bool wroteToPedal = description.startsWith("Rename")
                               || description.startsWith("Enable")
                               || description.startsWith("Disable")
                               || description.startsWith("Push")
                               || description.startsWith("Trim")
                               || description.startsWith("Set tempo")
                               || description.startsWith("Clear")
                               || description.startsWith("Swap");
        if (description.startsWith("Trim"))
            player.reload(); // same path, new bytes — fresh reader + thumbnail
        if (wroteToPedal)
            note << juce::String::fromUTF8(" \xe2\x80\x94 Disconnect to hear it on the pedal.");
        toast.show(note);
    };

    addAndMakeVisible(header);
    addAndMakeVisible(pedalLight); // over the header's right side
    addAndMakeVisible(versionChip);
    addAndMakeVisible(devMark);
    addAndMakeVisible(status);
    addAndMakeVisible(hint);
    addAndMakeVisible(banners);
    addAndMakeVisible(connectButton);
    addAndMakeVisible(disconnectButton);
    addAndMakeVisible(showEmptyToggle);
    addAndMakeVisible(settingsButton);
    addChildComponent(table);  // shown once a pedal is mounted
    addChildComponent(bottomTabs);
    addChildComponent(inspector);
    addChildComponent(player); // likewise
    addChildComponent(toast);  // fades in over everything on job success

    setWantsKeyboardFocus(true); // Space toggles playback

    applyColumnPreferences();
    showBottomTab(kAudioTab);
    applySnapshot({}); // the no-pedal state, until the first scan lands
    setSize(920, 680);

    worker.start();
    startTimer(kMidiPollIntervalMs); // presence poll — cheap device-list scan, message thread
}

void MainComponent::timerCallback()
{
    pollMidiPresence();
    tickConnectAttempt();

    // The hold expires on its own clock; re-enable Connect the moment it
    // does, not at the next presence flip.
    if (connectHoldUntilMs != 0 && nowMs() >= connectHoldUntilMs) {
        connectHoldUntilMs = 0;
        updateToolbar();
    }
    // Supervision pace only while something is being supervised.
    if (!connectAttempt.active() && connectHoldUntilMs == 0
        && getTimerInterval() != kMidiPollIntervalMs)
        startTimer(kMidiPollIntervalMs);
}

// The pedal outside STORAGE is still visible — as a USB-MIDI device. Knowing
// the difference between "no pedal at all" and "pedal here, wrong mode" turns
// the empty state from a shrug into an instruction.
void MainComponent::pollMidiPresence()
{
    bool present = false;
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        present = present || device.name.containsIgnoreCase("RC-5");
    if (present == midiPedalPresent)
        return;
    midiPedalPresent = present;
    hint.setText(midiPedalPresent
                     ? juce::String::fromUTF8("RC-5 detected \xe2\x80\x94 click Connect "
                                              "to browse loops")
                     : juce::String("Connect your looper via USB"),
                 juce::dontSendNotification);
    updateStatusText();
    updateToolbar();
}

// One sysex asks the pedal into STORAGE (issue #22); the attempt machine
// supervises what used to be fire-and-forget (issue #2): a frame sent while
// the pedal's MIDI side is still re-enumerating is silently lost, and a
// pedal playing a loop keeps the medium — both used to mean "Connecting…"
// forever, with nothing said.
void MainComponent::startConnectAttempt()
{
    connectAttempt.begin(nowMs());
    sendEnterStorage();
    toast.show(juce::String::fromUTF8("Connecting to the pedal\xe2\x80\xa6"));
    startTimer(kSuperviseTickMs);
    updateStatusText();
    updateToolbar();
}

// A failed send is not fatal mid-attempt: the lost-frame window is exactly
// why the attempt retries. The budget turns a persistent miss into the
// honest give-up banner.
void MainComponent::sendEnterStorage()
{
    lastConnectSendError = pedallink::requestStorageMode(true);
}

void MainComponent::tickConnectAttempt()
{
    if (!connectAttempt.active())
        return;
    switch (connectAttempt.tick(nowMs())) {
    case connect::Action::none:
        return;
    case connect::Action::sendFrame:
        sendEnterStorage();
        return;
    case connect::Action::giveUp:
        banners.showError(
            banners::Source::connection,
            lastConnectSendError.isEmpty()
                ? juce::String("The pedal did not hand over its card. If a loop is playing "
                               "or recording, stop it, then press Connect again.")
                : "Could not reach the pedal (" + lastConnectSendError
                      + juce::String::fromUTF8(") \xe2\x80\x94 check the USB cable, "
                                               "then try again."));
        updateStatusText();
        updateToolbar();
        return;
    }
}

// The attempt resolved outside the machine: the volume is honestly up, or
// the mount reported its own failure. Idempotent — connected snapshots keep
// arriving. The timer falls back to poll pace on its next tick.
void MainComponent::endConnectAttempt()
{
    if (!connectAttempt.active())
        return;
    connectAttempt.finish();
    updateStatusText();
    updateToolbar();
}

bool MainComponent::connectHoldActive() const
{
    return connectHoldUntilMs != 0 && nowMs() < connectHoldUntilMs;
}

MainComponent::~MainComponent()
{
    *uiAlive = false; // message thread; queued watcher completions become no-ops
}

// Quit is Disconnect (issue #1). A mutation in flight still finishes first:
// the eject is queued behind it on the worker, and the worker's shutdown
// join is generous — the time bound only abandons the WAIT, never the write.
bool MainComponent::beginQuitDisconnect(std::function<void()> done)
{
    switch (quitGate.request(snapshot.state, std::move(done))) {
    case QuitGate::Plan::quitNow:
        return false;
    case QuitGate::Plan::alreadyPending:
        return true;
    case QuitGate::Plan::startDisconnect:
        // The Disconnect button's own path: drop our hold on the volume (the
        // read-ahead thread keeps the slot WAV open), then eject; the eject
        // completion walks the pedal out of STORAGE and finishes the gate.
        engine.stop();
        player.clear();
        worker.requestEject();
        break;
    case QuitGate::Plan::joinEject:
        break; // the in-flight eject's completion resolves the gate
    }
    juce::Timer::callAfterDelay(kQuitReleaseBoundMs, [this, alive = uiAlive] {
        if (*alive)
            quitGate.finish();
    });
    return true;
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

// The panel always shows the slot the table has selected, re-read from the
// newest snapshot: a finished write must change what the switches say.
void MainComponent::updateInspector()
{
    inspector.setSlot(selectedSlot > 0 ? slotRowFor(selectedSlot) : nullptr);
}

// The bottom pane has two faces for the selected slot: listen to it (the
// player) or set it up (its properties). One pane, so the table never moves.
void MainComponent::showBottomTab(int index)
{
    const bool mounted = table.isVisible();
    player.setVisible(mounted && index == kAudioTab);
    inspector.setVisible(mounted && index == kPropertiesTab);
    resized();
}

// Settings -> Columns onto the table. The pedal's own facts always show; these
// two are the ones a player opts into.
void MainComponent::applyColumnPreferences()
{
    auto* file = settings.file();
    table.setOptionalColumns(
        file == nullptr || file->getBoolValue(kOneShotColumnKey, true),
        file != nullptr && file->getBoolValue(kCountInColumnKey, false));
}

void MainComponent::updateToolbar()
{
    const bool usable = snapshot.state == lifecycle::State::connected && snapshot.error.empty();
    disconnectButton.setEnabled(usable && !pedalBusy);
    // Connect: the pedal shows its MIDI face, no honest volume is up, no
    // attempt is already running, and the post-disconnect hold has passed
    // (the re-enumerating MIDI side eats frames — issue #2).
    connectButton.setEnabled(midiPedalPresent && !pedalBusy
                             && snapshot.state == lifecycle::State::disconnected
                             && !connectAttempt.active() && !connectHoldActive());
    if (appMenu != nullptr)
        appMenu->menuItemsChanged(); // the Maintenance items follow the same gate
}

void MainComponent::runBackup()
{
    worker.enqueue({ "Backup configs", 0,
                     [options = makeWriteOptions()](const volume::fs::path& volumePath) {
                         commands::backup(volumePath, options.backupRoot, options.stamp);
                     } });
}

void MainComponent::runCleanJunk()
{
    worker.enqueue({ "Clean junk", 0, [](const volume::fs::path& volumePath) {
                         // Mutations treat a sweep survivor as a warning; this
                         // action's one job IS the sweep, so a survivor is the
                         // job failing and says so.
                         const volume::SweepResult sweep = volume::sweepJunk(volumePath);
                         if (!sweep.failed.empty())
                             throw Error("cannot remove junk file "
                                         + sweep.failed.front().string());
                     } });
}

void MainComponent::showAbout()
{
    // The About popover is the badge's click handler; appkit keeps its
    // showPopup() private, so the menu routes through the same public click
    // path the mouse takes. TODO(appkit): expose showPopup() and drop the
    // synthetic event.
    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();
    const juce::MouseEvent event(source, {}, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &badge, &badge, now, {}, now, 1, false);
    badge.mouseUp(event);
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
                    banners.showError(banners::Source::connection,
                                      "Could not clear the stale mount (" + note
                                          + juce::String::fromUTF8(
                                                ") \xe2\x80\x94 unplug the pedal's USB cable, "
                                                "then plug it back."));
                }
                worker.pokeRescan();
            });
        });
}

// The volume as a human names it: Explorer says "BOSS RC-5 (D:)", and a bare
// "D:\" says nothing about whose card is up. Mac mount paths already carry
// the label as their last segment, so only Windows needs the lookup.
juce::String MainComponent::volumeDisplayName() const
{
    const juce::String path = utf8(snapshot.volume);
#if JUCE_WINDOWS
    const juce::String label = juce::File(path).getVolumeLabel();
    if (label.isNotEmpty())
        return label + " (" + path.trimCharactersAtEnd("\\") + ")";
#endif
    return path;
}

void MainComponent::updateStatusText()
{
    if (snapshot.state == lifecycle::State::ghost) {
        status.setText(volumeDisplayName() + juce::String::fromUTF8(" \xe2\x80\x94 device detached (ghost mount)"),
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
    if (connectAttempt.active() && snapshot.volume.empty()) {
        status.setText(juce::String::fromUTF8("Connecting to the pedal\xe2\x80\xa6"),
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kStatusText);
        return;
    }
    if (snapshot.volume.empty()) {
        status.setText(midiPedalPresent
                           ? juce::String::fromUTF8(
                                 "RC-5 on USB \xe2\x80\x94 ready to connect")
                           : juce::String("No looper found"),
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kStatusText);
        return;
    }
    if (!snapshot.error.empty()) {
        status.setText(volumeDisplayName() + juce::String::fromUTF8(" \xe2\x80\x94 ") + utf8(snapshot.error),
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kErrorText);
        return;
    }
    int loaded = 0;
    for (const auto& row : snapshot.slots)
        loaded += row.info.hasAudio ? 1 : 0;
    juce::String text = volumeDisplayName() + juce::String::fromUTF8("  \xe2\x80\x94  ") + juce::String(loaded) + " of "
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

    // The volume is up — the connect attempt (if one ran) met its goal. This
    // also settles a by-hand STORAGE entry racing a clicked Connect.
    if (snapshot.state == lifecycle::State::connected)
        endConnectAttempt();

    updateStatusText();
    // The strip's model applies its recovery policy here too: an honest
    // (re)mount clears the connection-error lane (issue #3).
    banners.scan(snapshot.state, snapshot.findings);

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
        else if (const SlotRow* held = slotRowFor(player.currentSlot()))
            player.setTempoNote(tempoNoteFor(held->info)); // a Set tempo just landed (issue #29)
    }

    if (snapshot.state == lifecycle::State::ghost) {
        if (!ghostCleanupStarted) {
            ghostCleanupStarted = true;
            cleanUpGhostMount();
        }
    } else if (previousState == lifecycle::State::ghost) {
        ghostCleanupStarted = false; // the episode is over
    }

#if JUCE_WINDOWS
    // "D:\" has no filename to show — the light wears the volume label.
    pedalLight.set(mounted, juce::File(utf8(snapshot.volume)).getVolumeLabel());
#else
    pedalLight.set(mounted, utf8(volume::fs::path(snapshot.volume).filename().string()));
#endif

    updateTableRows();
    updateToolbar();
    table.setVisible(mounted);
    bottomTabs.setVisible(mounted);
    hint.setVisible(!mounted);
    showBottomTab(bottomTabs.selected()); // the pane follows the pedal in and out
    if (!mounted)
        selectedSlot = 0;
    updateInspector();
}

void MainComponent::slotChosen(int slot, bool startPlaying)
{
    const SlotRow* found = slotRowFor(slot);
    if (found == nullptr)
        return;
    const SlotRow& row = *found;

    // Deliberately not gated on WavStat: the database lags behind the folder
    // until the pedal's boot-time indexing, and listening to the file is a
    // read-only act — a WAV that is there is a WAV you can hear.
    if (row.wavPath.empty()) {
        player.clear(); // an empty slot: nothing to listen to
        return;
    }

    const juce::String path = utf8(row.wavPath);
    if (path != player.currentPath()) {
        const juce::String title = juce::String(row.info.slot).paddedLeft('0', 2) + "  "
                                 + trimmedName(row);
        player.setSlot(row.info.slot, juce::File(path), title, row.info.oneShot, row.info.frames);
    }
    player.setTempoNote(tempoNoteFor(row.info));
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

    // Operations only: the things done TO a slot. Its settings live in the
    // panel now (and the two lamp columns still flip on click), so this menu
    // stopped being a second copy of them.
    juce::PopupMenu menu;
    menu.addItem(3, juce::String::fromUTF8(occupied ? "Replace WAV\xe2\x80\xa6" : "Push WAV here\xe2\x80\xa6"));
    menu.addItem(4, juce::String::fromUTF8("Pull to folder\xe2\x80\xa6"), occupied);
    menu.addSeparator();
    menu.addItem(5, juce::String::fromUTF8("Clear slot\xe2\x80\xa6"));

    // Nothing a user relies on may simply vanish: the first time this menu
    // opens without its settings, it says where they went.
    if (auto* file = settings.file(); file != nullptr && !file->getBoolValue("settingsMovedNotice")) {
        file->setValue("settingsMovedNotice", true);
        file->saveIfNeeded();
        toast.show(juce::String::fromUTF8(
            "Name, tempo, One Shot and Play Count-In moved to Slot settings (\xe2\x8c\x98I) "
            "\xe2\x80\x94 the One Shot and Play Count-In cells still switch them."));
    }

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetScreenArea({ screenPosition.x, screenPosition.y, 1, 1 }),
        [this, slot, name, occupied](int choice) {
            switch (choice) {
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

void MainComponent::toggleCountIn(int slot, bool currentlyOn)
{
    worker.enqueue({ juce::String(currentlyOn ? "Disable" : "Enable") + " Play Count-In on slot "
                         + juce::String(slot),
                     slot,
                     [slot, on = !currentlyOn, options = makeWriteOptions()](
                         const volume::fs::path& volumePath) {
                         commands::setCountIn(volumePath, { slot }, on, options);
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

// Windows locks open files: a mutation that renames, deletes or rewrites a
// slot's WAV cannot proceed while the preview holds it open — the app blocks
// its own write with "Access is denied" (observed live on hardware,
// issue #26). macOS never minded, so release on every platform alike.
void MainComponent::releasePlayerIfHolding(int slotA, int slotB)
{
    const int held = player.currentSlot();
    if (held == slotA || held == slotB)
        player.clear();
}

void MainComponent::pushWav(int slot, const juce::String& sourcePath, bool slotOccupied)
{
    const auto enqueuePush = [this, slot, sourcePath](bool force) {
        releasePlayerIfHolding(slot, slot); // a replace rewrites the WAV under preview (issue #26)
        worker.enqueue({ "Push " + juce::File(sourcePath).getFileName() + " to slot "
                             + juce::String(slot),
                         slot,
                         [source = sourcePath, slot, force,
                          options = makeWriteOptions(),
                          importTmp = settings.dataDir().getChildFile("import-tmp"),
                          trash = settings.dataDir().getChildFile("trash").getFullPathName().toStdString()](
                             const volume::fs::path& volumePath) {
                             // DAW exports arrive as anything — convert off the
                             // message thread, on this worker, before the push
                             // (issue #20). The temp conversion dies with the job.
                             wavimport::Prepared prepared;
                             const juce::Result ok =
                                 wavimport::prepare(juce::File(source), importTmp, prepared);
                             if (ok.failed())
                                 throw Error(ok.getErrorMessage().toStdString());
                             try {
                                 commands::push(volumePath,
                                                prepared.file.getFullPathName().toStdString(),
                                                slot,
                                                { .force = force, .trashRoot = trash,
                                                  .write = options });
                             } catch (...) {
                                 if (prepared.converted)
                                     prepared.file.deleteFile();
                                 throw;
                             }
                             if (prepared.converted)
                                 prepared.file.deleteFile();
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
            .withMessage(juce::String::fromUTF8(
                "This slot already holds a loop. The current WAV moves to the app's trash "
                "first \xe2\x80\x94 that is your undo."))
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
        releasePlayerIfHolding(slot, slot); // the clear moves or deletes its WAV (issue #26)
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

void MainComponent::openSettings()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new SettingsDialog(
        engine.deviceManager(), settings,
        { settings.file() == nullptr || settings.file()->getBoolValue(kOneShotColumnKey, true),
          settings.file() != nullptr && settings.file()->getBoolValue(kCountInColumnKey, false) },
        [this](SettingsDialog::Columns columns) {
            if (auto* file = settings.file()) {
                file->setValue(kOneShotColumnKey, columns.oneShot);
                file->setValue(kCountInColumnKey, columns.countIn);
                file->saveIfNeeded();
            }
            applyColumnPreferences();
        }));
    options.dialogTitle = "Settings";
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
    if (key == juce::KeyPress('i', juce::ModifierKeys::commandModifier, 0)
        && table.isVisible()) {
        bottomTabs.select(bottomTabs.selected() == kPropertiesTab ? kAudioTab : kPropertiesTab);
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
    // One line, right to left: the version, the gear, the view filter, then
    // the pedal buttons; the status text takes whatever is left and elides.
    auto statusRow = area.removeFromTop(28).reduced(12, 2);
    versionChip.setBounds(statusRow.removeFromRight(52)); // the text's own width
    devMark.setBounds(statusRow.removeFromRight(26));
    statusRow.removeFromRight(6);
    settingsButton.setBounds(statusRow.removeFromRight(22));
    statusRow.removeFromRight(8);
    showEmptyToggle.setBounds(statusRow.removeFromRight(134));
    disconnectButton.setBounds(statusRow.removeFromRight(94).reduced(2, 1));
    connectButton.setBounds(statusRow.removeFromRight(76).reduced(2, 1));
    status.setBounds(statusRow);
    const int bannerHeight = banners.preferredHeight();
    banners.setBounds(area.removeFromTop(bannerHeight).reduced(12, 0));
    if (bannerHeight > 0)
        area.removeFromTop(6);
    // The version moved up into the status row, so the strip it used to
    // reserve at the bottom goes to the pane that needed it: the waveform is
    // back to its old height with the tab strip on top of it.
    toast.setBounds(getWidth() / 2 - 280, getHeight() - kBottomPaneHeight - 42, 560, 34);
    auto bottom = area.removeFromBottom(kBottomPaneHeight).reduced(12, 8);
    bottomTabs.setBounds(bottom.removeFromTop(26));
    player.setBounds(bottom);
    inspector.setBounds(bottom);
    area.removeFromBottom(8);
    table.setBounds(area.reduced(12, 0));
    hint.setBounds(area);
}

} // namespace loopercat
