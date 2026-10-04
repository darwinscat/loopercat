// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"

#include "ClearSlotAction.h"
#include "ConnectGate.h"

#include "history/WriteOptionsFactory.h"
#include "history/HistoryRecorder.h"
#include "history/ForgetSlotJob.h"
#include "ClearSlotHistoryAction.h"
#include "history/SlotRows.h"
#include "OperationsLog.h"
#include "PedalPortName.h"
#include "Strings.h"
#include "WavImport.h"

#include "CardPermissions.h"

#include <loopercat/DeviceProfile.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/Wav.hpp>

#include <felitronics/appkit/AudioSettingsPanel.h>
#include <felitronics/appkit/Brand.h> // feedTheCatUrl — the family tip jar

#include <BinaryData.h>

#include <cmath>

namespace loopercat
{

namespace
{
    constexpr auto kProductUrl = "https://darwinscat.com/loopercat";

    // What the version badge calls this binary's wrapper — a plugin says VST3 or AU here; an app
    // says App. The badge shows it under the version and signs the family tip jar with it.
    constexpr auto kBadgeFormat = "App";

    // Which behaviour columns the table shows. One Shot is on out of the box
    // because most players use it; Play Count-In waits to be asked for.
    constexpr auto kOneShotColumnKey = "columnOneShot";
    constexpr auto kCountInColumnKey = "columnPlayCountIn";
    // The LUFS column (issue #61) is off until asked for: it stays empty
    // until a measure or a check fills it, and an empty column is noise.
    constexpr auto kLoudnessColumnKey = "columnLoudness";

    // Normalize-on-upload (issue #53): OFF by default so every existing
    // workflow keeps the byte-exact pass-through; -18 LUFS is ReplayGain
    // 2.0's reference — the modern spelling of the "89 dB" the request
    // arrived in.
    constexpr auto kNormalizeOnUploadKey = "normalizeOnUpload";
    constexpr auto kNormalizeTargetLufsKey = "normalizeTargetLufs";
    constexpr double kDefaultTargetLufs = -18.0;

    // The history's storage limit (issue #74): the bytes of takes the app
    // keeps before it starts offering the oldest for release. 5 GB out of
    // the box (retention::kDefaultLimit); nothing goes without a press.
    constexpr auto kHistoryLimitKey = "historyLimitBytes";
    // The pedal book (issue #98): endpoint -> the card it carried and its
    // name, in the book's own text form (PedalBook.h).
    constexpr auto kPedalBookKey = "pedalBook";

    // A pedal in one word for the log: its family and the OS's endpoint id.
    juce::String pedalTag(const pedallink::Pedal& pedal)
    {
        return juce::String(std::string(pedal.family())) + "@" + pedal.endpoint.identifier;
    }
    // A release hands the file's free pages back a slice at a time — 16 KB
    // pages, so 256 of them is 4 MB per transaction.
    constexpr int kVacuumSlicePages = 256;

    // The story a normalization tells the toast and the operations log.
    juce::String describeNormalize(const wavimport::NormalizeOutcome& outcome, double targetLufs)
    {
        const auto lufs = [](double v) { return juce::String(v, 1); };
        if (outcome.damaged)
            return "not normalized: " + juce::String(outcome.wildSamples)
                 + " impossible sample value(s), the audio looks damaged";
        if (!outcome.measurable)
            return "not normalized: too short or too quiet to measure";
        if (outcome.untouched)
            return "already at " + lufs(targetLufs) + " LUFS (measured "
                 + lufs(outcome.measuredLufs) + "), file untouched";
        juce::String story = "normalized " + juce::String(outcome.gainDb >= 0.0 ? "+" : "")
                           + juce::String(outcome.gainDb, 1) + " dB ("
                           + lufs(outcome.measuredLufs)
                           + juce::String::fromUTF8(" \xe2\x86\x92 ")
                           + lufs(outcome.measuredLufs + outcome.gainDb) + " LUFS)";
        if (outcome.cappedByPeak)
            story << ", boost capped at the -1 dBTP peak ceiling";
        return story;
    }

    // The same story for the on-card command (issue #53) — its no-write
    // outcomes are answers, and the toast must not imply a rewrite.
    juce::String describeNormalize(const commands::NormalizeResult& result, double targetLufs)
    {
        const auto lufs = [](double v) { return juce::String(v, 1); };
        if (!result.applied)
            return result.cappedByPeak
                     ? "already peaking at the -1 dBTP ceiling (measured "
                           + lufs(result.measuredLufs) + " LUFS), nothing to give it, file untouched"
                     : "already at " + lufs(targetLufs) + " LUFS (measured "
                           + lufs(result.measuredLufs) + "), file untouched";
        juce::String story = juce::String(result.gainDb >= 0.0 ? "+" : "")
                           + juce::String(result.gainDb, 1) + " dB ("
                           + lufs(result.measuredLufs)
                           + juce::String::fromUTF8(" \xe2\x86\x92 ")
                           + lufs(result.measuredLufs + result.gainDb) + " LUFS)";
        if (result.cappedByPeak)
            story << ", boost capped at the -1 dBTP peak ceiling";
        return story;
    }

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
                 .licence = "AGPL-3.0-or-later",
                 // The dependency table: each row's version, whether the build took the
                 // pinned release or a sibling checkout, and the checkout's commit when
                 // it did — stamped by CMake, which is the one party that knows.
                 .dependencies = { { .label = "felitronics-core",
                                     .version = LOOPERCAT_DEP_FCORE_VERSION,
                                     .ownerRepo = "darwinscat/felitronics-core",
                                     .commit = LOOPERCAT_DEP_FCORE_COMMIT,
                                     .state = LOOPERCAT_DEP_FCORE_STATE },
                                   { .label = "felitronics-toml",
                                     .version = LOOPERCAT_DEP_TOML_VERSION,
                                     .ownerRepo = "darwinscat/felitronics-toml",
                                     .commit = LOOPERCAT_DEP_TOML_COMMIT,
                                     .state = LOOPERCAT_DEP_TOML_STATE },
                                   { .label = "felitronics-appkit",
                                     .version = LOOPERCAT_DEP_APPKIT_VERSION,
                                     .ownerRepo = "darwinscat/felitronics-appkit",
                                     .commit = LOOPERCAT_DEP_APPKIT_COMMIT,
                                     .state = LOOPERCAT_DEP_APPKIT_STATE },
                                   { .label = "JUCE",
                                     .version = LOOPERCAT_DEP_JUCE_VERSION,
                                     .ownerRepo = "juce-framework/JUCE",
                                     .state = "pin" },
                                   // minimp3 has no releases: the commit IS the version, and the
                                   // version column — which links to a release tag — stays blank
                                   // rather than pointing at a tag that does not exist.
                                   { .label = "minimp3",
                                     .ownerRepo = "lieff/minimp3",
                                     .commit = LOOPERCAT_DEP_MINIMP3_COMMIT,
                                     .state = "pin" },
                                   // SQLite lives at sqlite.org, not on GitHub: no ownerRepo,
                                   // so the row is plain text rather than a link to a mirror.
                                   { .label = "SQLite",
                                     .version = LOOPERCAT_DEP_SQLITE_VERSION,
                                     .state = "pin" } },
                 // The popover mirrors the window header: the ears, not the
                 // family-default orbit the hook falls back to.
                 .drawMark = [](juce::Graphics& g, float cx, float cy, float d) {
                     ui::drawLoopMark(g, cx, cy, d);
                 },
                 // The trademark sentence the README carries, as the window's small print.
                 .notice = juce::String::fromUTF8(BinaryData::notice_txt, BinaryData::notice_txtSize)
                               .trim() };
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

MainComponent::MainComponent(std::string explicitVolume, juce::File dataOverride)
    : settings(dataOverride),
      header(BinaryData::catlogo_svg, BinaryData::catlogo_svgSize,
             BinaryData::MichromaRegular_ttf, BinaryData::MichromaRegular_ttfSize,
             "LooperCat", kProductUrl),
      badge(updateChecker, badgeConfig(), kBadgeFormat),
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
    // What the card may be asked, from its model (CardPermissions.h): the
    // table and the studio offer only that, and the core refuses the rest.
    table.permissions = [this] { return CardPermissions::of(snapshot.family); };
    inspector.permissions = [this] { return CardPermissions::of(snapshot.family); };
    rhythmPane.permissions = [this] { return CardPermissions::of(snapshot.family); };
    player.permissions = [this] { return CardPermissions::of(snapshot.family); };
    table.onSlotContextMenu = [this](int slot, juce::Point<int> at) { showSlotMenu(slot, at); };
    table.onSlotsContextMenu = [this](std::vector<int> slots, juce::Point<int> at) {
        showSlotsMenu(std::move(slots), at);
    };
    table.onOneShotToggled = [this](int slot) {
        if (const SlotRow* row = pedalBusy ? nullptr : slotRowFor(slot))
            toggleOneShot(slot, row->info.oneShot);
    };
    table.onCountInToggled = [this](int slot) {
        if (const SlotRow* row = pedalBusy ? nullptr : slotRowFor(slot))
            toggleCountIn(slot, row->info.countIn);
    };
    table.onRenameCommitted = [this](int slot, juce::String newName) {
        if (pedalBusy || slotRowFor(slot) == nullptr
            || !CardPermissions::of(snapshot.family).rename)
            return;
        const auto options = makeWriteOptions();
        worker.enqueue(recorded("rename", options,
                                { "Rename slot " + juce::String(slot), slot,
                                  [name = newName.toStdString(), options,
                                   slot](const volume::fs::path& volumePath) {
                                      commands::rename(volumePath, slot, name, options);
                                  } }));
    };
    table.onTempoCommitted = [this](int slot, long long tenths) {
        if (pedalBusy || slotRowFor(slot) == nullptr || !CardPermissions::of(snapshot.family).tempo)
            return;
        const auto options = makeWriteOptions();
        worker.enqueue(recorded("tempo", options,
                                { "Set tempo on slot " + juce::String(slot), slot,
                                  [slot, tenths, options](const volume::fs::path& volumePath) {
                                      commands::setTempo(volumePath, slot, tenths, options);
                                  } }));
    };
    table.onAudioDropped = [this](int slot, juce::String path) {
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
        const auto options = makeWriteOptions();
        worker.enqueue(recorded("swap", options,
                                { "Swap slots " + juce::String(from) + " and " + juce::String(to),
                                  from,
                                  [from, to, options](const volume::fs::path& volumePath) {
                                      commands::swap(volumePath, from, to, options);
                                  } },
                                { to })); // the job names one slot for its busy row; the swap is about both
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
    rhythmPane.onEdit = [this](int slot, usecases::rhythm::Edits edits) {
        if (!pedalBusy && slotRowFor(slot) != nullptr)
            editRhythm(slot, std::move(edits));
    };
    inspector.onPlayStopEdited = [this](int slot, usecases::playstop::Edits edits) {
        if (!pedalBusy && slotRowFor(slot) != nullptr)
            editPlayStop(slot, std::move(edits));
    };
    table.onLoudnessCellDoubleClicked = [this](int slot) { measureSlotLoudness(slot); };
    // The player read the file for its waveform anyway; the meter rode along.
    player.onLoudnessRead = [this](int slot, const wav::LoudnessReading& reading) {
        applyLoudnessReport(slot, describeReading(reading, currentTargetLufs()), 0);
    };
    player.onNormalize = [this](int slot) {
        if (const SlotRow* row = pedalBusy ? nullptr : slotRowFor(slot))
            normalizeSlot(slot, trimmedName(*row));
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
                             + juce::String::fromUTF8(" s. The original WAV is kept in the "
                                                      "history \xe2\x80\x94 that is your undo."))
                .withButton("Trim")
                .withButton("Cancel"),
            [this, slot, inFrame, outFrame](int button) {
                if (button != 1)
                    return;
                const auto options = makeWriteOptions();
                // Keep the pane's state: the completion path reload()s the
                // trimmed bytes into the same slot view.
                player.releaseFile();
                auto note = std::make_shared<juce::String>();
                worker.enqueue(recorded(
                    "trim", options,
                    { "Trim slot " + juce::String(slot), slot,
                      [slot, inFrame, outFrame, options, note](const volume::fs::path& volumePath) {
                          const auto trimmed =
                              commands::trim(volumePath, slot, inFrame, outFrame, { .write = options });
                          // A trim that replaced a note-value length with a bar
                          // count says so (issue #92) — toast and row alike.
                          if (trimmed.noteLengthReplaced)
                              *note = juce::String(history::story::kNoteLengthReplaced.data(),
                                                   history::story::kNoteLengthReplaced.size());
                      },
                      note }));
            });
    };

    // The toolbar carries only the primary story — Connect / Disconnect and
    // the empty-slot view filter; service actions live in the Maintenance
    // menu, About in the app menu (platform standard).
    for (auto* button : { &connectButton, &disconnectButton }) {
        button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1e1e26));
        button->setEnabled(false);
    }
    connectButton.onClick = [this] { beginConnect(); };
    disconnectButton.onClick = [this] { beginDisconnect(); };
    appMenu = std::make_unique<AppMenu>(AppMenu::Actions {
        .about = [this] { showAbout(); },
        .cleanJunk = [this] { runCleanJunk(); },
        .feedTheCat = [] {
            // Signed like the badge's own link: which app, which machine, which wrapper.
            felitronics::appkit::brand::feedTheCatLink("LooperCat",
                                                       felitronics::appkit::brand::feedTheCatUrl,
                                                       kBadgeFormat)
                .launchInDefaultBrowser();
        },
        .maintenanceEnabled = [this] {
            return snapshot.state == lifecycle::State::connected && snapshot.error.empty()
                && !pedalBusy;
        },
        .cleanJunkEnabled = [this] { return CardPermissions::of(snapshot.family).anyWrite(); },
        .undo = [this] { pressUndo(false); },
        .redo = [this] { pressUndo(true); },
        .undoText = [this] { return undoMenuText(false); },
        .redoText = [this] { return undoMenuText(true); },
        .undoEnabled = [this] { return undoEnabled(false); },
        .redoEnabled = [this] { return undoEnabled(true); },
        .openHistory = [this] { openHistoryWindow(); } });

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
            trace(connectAttempt.active() ? "connect: a disk appeared \xe2\x80\x94 the pedal heard us"
                                          : "connect: a disk appeared with no attempt running (storage mode by hand?)");
            connectAttempt.diskAppeared();
            worker.pokeRescan();
        });
    };
    deviceWatcher.onAutoMountResult = [this, alive = uiAlive](bool ok, std::string message) {
        juce::MessageManager::callAsync([this, alive, ok, note = utf8(message)] {
            if (!*alive)
                return;
            if (ok) {
                trace("connect: mounted " + note);
                toast.show("Mounted " + note);
                worker.pokeRescan();
            } else {
                trace("connect: the card would not mount: " + note);
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
                    trace(unmounted ? "disconnect: ejected" + (note.isNotEmpty() ? " (" + note + ")" : juce::String())
                                    : "disconnect: eject refused" + (note.isNotEmpty() ? " (" + note + ")" : juce::String()));
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
                    // The same pedal Connect chose walks out (issue #98); a
                    // volume that was mounted by hand has no chosen pedal,
                    // and then the first RC-5 on the bus is all there is.
                    const juce::String exitError = connectTarget
                        ? pedallink::requestStorageMode(false, *connectTarget)
                        : pedallink::requestStorageMode(false);
                    trace("disconnect: exit-storage frame \xe2\x86\x92 "
                          + (connectTarget ? pedalTag(*connectTarget) : juce::String("the first pedal on the bus"))
                          + ": " + (exitError.isEmpty() ? juce::String("sent") : "NOT sent (" + exitError + ")"));
                    connectTarget.reset();
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
    worker.onBusy = [this](bool busy, int slot, bool background) {
        table.setBusySlot(busy ? slot : 0);
        if (background)
            return; // a read-only check pulses its row and locks nothing (issue #61)
        pedalBusy = busy;
        inspector.setBusy(busy);
        rhythmPane.setBusy(busy);
        historyView.setBusy(busy);
        history.setBusy(busy || clearingHistory);
        updateStatusText();
        updateToolbar();
        if (!busy) {
            restoreListening(); // the job's own rescan applied while busy — see restoreListening
            // A job may have written a row: what Undo offers and what the
            // History window shows are read again, off the worker.
            refreshUndoOffer();
            if (historyHost != nullptr && historyHost->isVisible())
                feedHistoryWindow();
        }
    };
    worker.onJobResult = [this](juce::String description, juce::String error, int batch,
                                int slot) {
        // Credited by the id the worker hands back — never by parsing text.
        const bool inBatch = batchId != 0 && batch == batchId;
        const bool inCheck = checkId != 0 && batch == checkId;
        if (historyEditPending && description == historyEditDescription)
            settleHistoryEdit(error.isEmpty() ? description + juce::String::fromUTF8(" \xe2\x80\x94 done")
                                              : description + ": " + error);
        if (error.isNotEmpty()) {
            banners.showError(banners::Source::job, description + ": " + error);
            if (description.startsWith("Check slot") && slot > 0) {
                player.clearLoudness(slot);       // a failed read must not stay "measuring…"
                table.clearPendingLoudness(slot); // …nor its cell "…"
            }
            if (inBatch) {
                ++batchFailed;
                batchOverlay.setDone(batchDone + batchFailed);
                if (batchDone + batchFailed >= batchTotal)
                    endNormalizeBatch();
            }
            if (inCheck) {
                ++checkFailed;
                if (checkDone + checkFailed >= checkTotal)
                    finishLoudnessCheck();
                else
                    updateStatusText();
            }
            return;
        }
        banners.clearJobError(); // a later mutation succeeded — the story moved on
        // The pedal re-reads its memory on leaving STORAGE — settings and
        // audio alike (hardware, 2026-08-11: a rename, a tempo change and a
        // trim all reached the pedal with no power cycle). Disconnect is the
        // whole story, so nobody gets sent to the wall plug.
        const bool wroteToPedal = (description.startsWith("Rename")
                                   || description.startsWith("Enable")
                                   || description.startsWith("Disable")
                                   || description.startsWith("Push")
                                   || description.startsWith("Trim")
                                   || description.startsWith("Downmix")
                                   || description.startsWith("Normalize")
                                   || description.startsWith("Set tempo")
                                   || description.startsWith("Clear")
                                   || description.startsWith("Swap")
                                   || description.startsWith("Restore")
                                   || description.startsWith("Undo")
                                   || description.startsWith("Redo"))
                               // A normalize that found nothing to change wrote
                               // nothing — its note says so, and the toast must
                               // not send anyone to Disconnect for it. Only for
                               // Normalize: a Push saying "file untouched" still
                               // pushed that file onto the card.
                               && !(description.startsWith("Normalize")
                                    && description.contains("file untouched"));
        if (description.startsWith("Trim"))
            player.reload(); // same path, new bytes — fresh reader + thumbnail
        // A reading belongs to the audio, not the memory: a rename, a tempo
        // or a flag leaves it standing. Only a rewrite of the WAV itself
        // moves the audio on — push, trim, downmix, a normalize that applied,
        // clear; a swap moves two slots' files and the job names one, so it
        // clears them all.
        const bool changedAudio = description.startsWith("Push")
                               || description.startsWith("Trim")
                               || description.startsWith("Downmix")
                               || (description.startsWith("Normalize") && wroteToPedal)
                               || description.startsWith("Clear")
                               || description.startsWith("Swap")
                               || description.startsWith("Restore")
                               || description.startsWith("Undo")
                               || description.startsWith("Redo");
        if (changedAudio) {
            // A restore, an undo or a redo may move takes between slots (a
            // swap put back) or touch several: every reading goes, as for a swap.
            if (description.startsWith("Swap") || description.startsWith("Restore")
                || description.startsWith("Undo") || description.startsWith("Redo")) {
                table.clearAllLoudness();
                player.clearLoudness();
            } else if (slot > 0) {
                table.clearLoudness(slot);
                player.clearLoudness(slot);
            }
        }
        if (inCheck) {
            // The reading already landed in the column (applyLoudnessReport
            // ran before this result was delivered); count it, stay quiet.
            ++checkDone;
            if (checkDone + checkFailed >= checkTotal)
                finishLoudnessCheck();
            else
                updateStatusText();
            return;
        }
        if (inBatch) {
            // Per-job toasts stay quiet under the overlay (they would replace
            // each other unread anyway) — the batch summary speaks once, and
            // operations.log already holds every line.
            ++batchDone;
            if (description.contains("file untouched"))
                ++batchUntouched;
            batchOverlay.setDone(batchDone + batchFailed);
            if (batchDone + batchFailed >= batchTotal)
                endNormalizeBatch();
            return;
        }
        juce::String note = description + juce::String::fromUTF8(" \xe2\x80\x94 done");
        if (wroteToPedal)
            note << juce::String::fromUTF8(" \xe2\x80\x94 Disconnect to hear it on the pedal.");
        toast.show(note);
    };

    addAndMakeVisible(header);
    addAndMakeVisible(pedalLight); // over the header's right side
    pedalLight.onClick = [this] { renamePedal(); };
    // The pedal book is a convenience, never the truth: a book that cannot
    // be read is said out loud and started afresh, so a stale cache can
    // never stop Connect.
    if (auto* file = settings.file()) {
        try {
            pedalBook = pedalbook::Book::parse(file->getValue(kPedalBookKey).toStdString());
        } catch (const Error& e) {
            oplog::append(settings.dataDir(), "pedal book unreadable, starting afresh: "
                                                  + juce::String::fromUTF8(e.what()));
            toast.show(juce::String::fromUTF8("The list of known pedals could not be read "
                                              "\xe2\x80\x94 starting afresh (see operations.log)"));
        }
    }
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
    addChildComponent(rhythmPane);
    addChildComponent(history);
    history.onClearHistory = [this](int slot) { clearSlotHistory(slot); };
    history.onPlay = [this](std::int64_t op) { playFromHistory(op); };
    history.onRestore = [this](std::int64_t op) { restoreFromHistory(op); };
    // The History window (#73): the view knows nothing of the store; its
    // four callbacks come back here, and every one of them ends in a re-read.
    historyView.onPlay = [this](std::int64_t op) { playFromWindow(op); };
    historyView.onExportTake = [this](std::int64_t op) { exportFromWindow(op); };
    historyView.onRestore = [this](std::int64_t op) { restoreFromWindow(op); };
    historyView.onPin = [this](std::int64_t op, bool pinned) { pinFromWindow(op, pinned); };
    // The speed bump in the app: a dialog, the confirming button first.
    askFirst = [](const juce::String& title, const juce::String& message, const juce::String& confirm,
                  std::function<void(bool)> answer) {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::WarningIcon)
                                         .withTitle(title)
                                         .withMessage(message)
                                         .withButton(confirm)
                                         .withButton("Cancel"),
                                     [answer](int button) { answer(button == 1); });
    };
    addChildComponent(player); // likewise
    addChildComponent(toast);  // fades in over everything on job success
    addChildComponent(batchOverlay); // over even that: the batch takeover (issue #61)
    batchOverlay.onCancel = [this] {
        if (batchId == 0)
            return;
        batchDropped = worker.cancelPending(batchId);
        batchTotal -= batchDropped; // the dropped tail will never report back
        batchOverlay.setCancelling();
        batchOverlay.setDone(batchDone + batchFailed); // re-caption against the new total
        if (batchDone + batchFailed >= batchTotal)
            endNormalizeBatch(); // nothing in flight — the batch is over now
    };

    setWantsKeyboardFocus(true); // Space toggles playback

    applyColumnPreferences();
    showBottomTab(kAudioTab);
    applySnapshot({}); // the no-pedal state, until the first scan lands
    setSize(920, 680);

    worker.start();
    refreshUndoOffer(); // the Edit menu names its targets from the first open
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
// the empty state from a shrug into an instruction. Another RC model on the
// bus is named for what it is: greeting an RC-500 as "RC-5 detected" offered
// a Connect the pedal never answers.
void MainComponent::pollMidiPresence()
{
    // Every pedal the profile table knows, with the table's read gate
    // deciding whether Connect is offered (PedalPresence.h); an RC model the
    // table does not know is named for what it is, and spoken to by nobody.
    const auto devices = juce::MidiInput::getAvailableDevices();
    std::vector<presence::Seen> seen;
    for (const auto& pedal : pedallink::familyAmong(devices))
        seen.push_back(presence::seen(*pedal.profile));
    juce::String other;
    if (seen.empty())
        for (const auto& device : devices)
            if (const auto model = portname::announcedModel(device.name.toStdString());
                model && other.isEmpty())
                other = juce::String(*model);
    const presence::Verdict words = presence::describe(seen);
    if (words.connectable == midiPedalPresent && words.hint == presenceWords.hint
        && other == otherLooperOnBus)
        return;
    midiPedalPresent = words.connectable;
    presenceWords = words;
    otherLooperOnBus = other;
    hint.setText(otherLooperOnBus.isNotEmpty() && !midiPedalPresent
                     ? otherLooperOnBus
                           + juce::String::fromUTF8(" detected \xe2\x80\x94 ")
                           + utf8(profile::onlySpeaks())
                     : utf8(presenceWords.hint),
                 juce::dontSendNotification);
    updateStatusText();
    updateToolbar();
}

// One sysex asks the pedal into STORAGE (issue #22); the attempt machine
// supervises what used to be fire-and-forget (issue #2): a frame sent while
// the pedal's MIDI side is still re-enumerating is silently lost, and a
// pedal playing a loop keeps the medium — both used to mean "Connecting…"
// forever, with nothing said.
// --- the --cycle seam -------------------------------------------------------
// Both buttons route through these, so a headless verification run exercises
// the same code a finger does.

// One line per step of a connection in operations.log, so "Connect did
// nothing" has an answer after the toast is gone: which pedals were on the
// bus, which was chosen, what its register said, every frame and its send
// result, the disk appearing, the give-up, the walk out. The log's appends
// are one write each, so a line from here never splits one of the worker's.
void MainComponent::trace(const juce::String& line)
{
    oplog::append(settings.dataDir(), line);
}

void MainComponent::beginConnect()
{
    // The pedal Connect goes to is chosen here, once, and remembered: the
    // frames of this attempt and the walk out at Disconnect go to the same
    // endpoint (issue #98). One pedal: nothing to ask. More than one: ask,
    // by the name on its card when the book has met the endpoint, and by
    // model and endpoint id when it has not — never by a made-up name.
    // Only pedals whose card this build reads are offered: entering storage
    // mode is a write to the pedal, and a card the app would then refuse is
    // a trip for nothing (PedalPresence.h draws the same line).
    std::vector<pedallink::Pedal> pedals;
    juce::String bus;
    for (const auto& pedal : pedallink::findFamily()) {
        bus << (bus.isEmpty() ? "" : ", ") << pedalTag(pedal)
            << (pedal.profile->allows(profile::Operation::read) ? "" : " (card not readable)");
        if (pedal.profile->allows(profile::Operation::read))
            pedals.push_back(pedal);
    }
    trace("connect: on the bus: " + (bus.isEmpty() ? juce::String("nothing this app speaks to") : bus));
    if (pedals.empty()) {
        trace("connect: no readable pedal to connect to");
        banners.showError(banners::Source::connection,
                          juce::String::fromUTF8("No looper this app can read is on USB "
                                                 "\xe2\x80\x94 plug the pedal in, then press Connect."));
        updateStatusText();
        updateToolbar();
        return;
    }
    if (pedals.size() == 1) {
        connectTarget = pedals.front();
        trace("connect: the one pedal, " + pedalTag(*connectTarget));
        askPedalBeforeConnect(*connectTarget);
        return;
    }
    juce::PopupMenu which;
    const auto labels = pedalChoiceLabels(pedals);
    for (std::size_t i = 0; i < pedals.size(); ++i)
        which.addItem(static_cast<int>(i) + 1, utf8(labels[i]));
    trace("connect: asked which of " + juce::String(static_cast<int>(pedals.size())));
    juce::Component::SafePointer<MainComponent> safe(this);
    which.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&connectButton),
                        [safe, pedals, labels](int chosen) {
                            if (safe == nullptr)
                                return;
                            if (chosen <= 0) {
                                safe->trace("connect: the choice was dismissed, nothing sent");
                                return; // dismissed: no pedal was asked anything
                            }
                            const auto index = static_cast<std::size_t>(chosen) - 1;
                            safe->connectTarget = pedals[index];
                            safe->trace("connect: chose \xe2\x80\x9c" + utf8(labels[index]) + "\xe2\x80\x9d = "
                                        + pedalTag(pedals[index]));
                            safe->askPedalBeforeConnect(*safe->connectTarget);
                        });
}

// The lines of the "which pedal?" menu, in bus order. Two pedals the book
// knows by the same name (a card copied between them, or the same default
// name twice) are told apart by their endpoint, so no line is ambiguous.
std::vector<std::string>
MainComponent::pedalChoiceLabels(const std::vector<pedallink::Pedal>& pedals) const
{
    std::vector<std::string> labels;
    for (const auto& pedal : pedals) {
        const std::string endpoint = pedal.endpoint.identifier.toStdString();
        labels.push_back(pedalbook::choiceLabel(pedal.family(), endpoint, pedalBook.find(endpoint)));
    }
    for (std::size_t i = 0; i < labels.size(); ++i)
        for (std::size_t j = 0; j < labels.size(); ++j)
            if (i != j && labels[i] == labels[j])
                labels[i] += " (endpoint " + pedals[i].endpoint.identifier.toStdString() + ")";
    return labels;
}

// Before the first frame the pedal is asked whether it can hand over its
// card at all (issue #85): the storage register says 02 while a loop plays
// or an unsaved take sits in the current memory, and then the enter-storage
// frame is refused on the spot — no resend budget changes that, only the
// musician can. The exchange blocks for up to the query timeout, so it runs
// on the worker like every other wait on the pedal; the answer comes back
// to the message thread and the gate (ConnectGate.h) decides. For the
// player Connect is under way from the click: the status says so and the
// button is off, even though no frame has gone out yet.
void MainComponent::askPedalBeforeConnect(pedallink::Pedal pedal)
{
    connectQueryPending = true;
    toast.show(juce::String::fromUTF8("Connecting to the pedal\xe2\x80\xa6"));
    updateStatusText();
    updateToolbar();
    juce::Component::SafePointer<MainComponent> safe(this);
    worker.enqueue({ "Ask the pedal before connecting",
                     0,
                     [pedal, safe, alive = uiAlive, logDir = settings.dataDir()](const volume::fs::path&) {
                         std::optional<storage::State> answer;
                         juce::String failure;
                         try {
                             switch (pedallink::readStorageState(pedal)) {
                             case pedallink::StorageQuery::idle: answer = storage::State::idle; break;
                             case pedallink::StorageQuery::inStorage: answer = storage::State::inStorage; break;
                             case pedallink::StorageQuery::busy: answer = storage::State::busy; break;
                             case pedallink::StorageQuery::noAnswer: break;
                             }
                         } catch (const Error& e) {
                             // An exchange that failed is silence with a reason:
                             // the attempt runs as it always did, the reason is logged.
                             failure = juce::String::fromUTF8(e.what());
                         }
                         oplog::append(logDir,
                                       "connect: " + pedalTag(pedal) + " register says "
                                           + juce::String(answer ? storage::describe(*answer) : "nothing")
                                           + (failure.isNotEmpty() ? " (" + failure + ")" : juce::String()));
                         juce::MessageManager::callAsync([safe, alive, answer] {
                             if (*alive && safe != nullptr)
                                 safe->gateConnect(answer);
                         });
                     },
                     nullptr,
                     0,
                     false,    // not background: the player pressed Connect and waits
                     true,     // quiet: the gate's outcome is what the window shows
                     false }); // and it needs no card — the card is the thing being asked about
}

void MainComponent::gateConnect(std::optional<storage::State> answer)
{
    connectQueryPending = false;
    const connectgate::Decision decision = connectgate::decide(answer);
    switch (decision.verdict) {
    case connectgate::Verdict::refuse:
        // Nothing is sent, nothing is retried: the sentence says what to do.
        trace("connect: refused, nothing sent \xe2\x80\x94 " + utf8(decision.reason));
        connectTarget.reset();
        banners.showError(banners::Source::connection, utf8(decision.reason));
        break;
    case connectgate::Verdict::alreadyInStorage:
        // The pedal is offering its medium already: no frame, just look for it.
        trace("connect: already in storage mode, no frame \xe2\x80\x94 looking for the card");
        toast.show(juce::String::fromUTF8(
            "The pedal is in storage mode already \xe2\x80\x94 looking for its card\xe2\x80\xa6"));
        worker.pokeRescan();
        break;
    case connectgate::Verdict::sendFrame:
        trace("connect: attempt begins");
        startConnectAttempt();
        return; // the attempt refreshed status and toolbar itself
    }
    updateStatusText();
    updateToolbar();
}

void MainComponent::beginDisconnect()
{
    trace("disconnect: requested for " + utf8(snapshot.volume));
    // Release our own hold on the volume first: the read-ahead thread keeps
    // the slot WAV open, and an open file dissents the unmount. The eject
    // completion then walks the pedal out of STORAGE.
    engine.stop();
    player.clear();
    worker.requestEject();
}

std::string MainComponent::lifecycleStateName() const
{
    return lifecycle::stateName(snapshot.state);
}

std::string MainComponent::volumePath() const
{
    return snapshot.volume;
}

std::vector<std::string> MainComponent::bannerLines() const
{
    std::vector<std::string> out;
    for (const banners::Line& line : banners.lines())
        out.push_back((line.level == commands::Level::error ? "ERROR " : "info  ") + line.text);
    return out;
}

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
    lastConnectSendError = connectTarget ? pedallink::requestStorageMode(true, *connectTarget)
                                         : pedallink::requestStorageMode(true);
    trace("connect: enter-storage frame \xe2\x86\x92 "
          + (connectTarget ? pedalTag(*connectTarget) : juce::String("the first pedal on the bus")) + ": "
          + (lastConnectSendError.isEmpty() ? juce::String("sent") : "NOT sent (" + lastConnectSendError + ")"));
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
        trace("connect: gave up \xe2\x80\x94 no disk appeared after the resend budget"
              + (lastConnectSendError.isEmpty() ? juce::String() : "; last send error: " + lastConnectSendError));
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

// The --push seam's whole verdict: after a push into the selected slot the
// player must be holding THAT slot with its waveform drawn — playerReady()
// cannot say this, because an empty player counts as "ready" there.
bool MainComponent::listeningTo(int slot) const
{
    return player.currentSlot() == slot && player.isThumbnailReady();
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
    for (const auto& row : snapshot.slots) {
        bool holdsLoop = !row.wavFile.empty();
        for (const auto& track : row.info.tracks)
            holdsLoop = holdsLoop || track.hasAudio; // any track's take keeps the row
        if (holdsLoop)
            visible.push_back(row);
    }
    table.setRows(std::move(visible));
}

// The panel always shows the slot the table has selected, re-read from the
// newest snapshot: a finished write must change what the switches say.
void MainComponent::updateInspector()
{
    inspector.setSlot(selectedSlot > 0 ? slotRowFor(selectedSlot) : nullptr);
    rhythmPane.setSlot(selectedSlot > 0 ? slotRowFor(selectedSlot) : nullptr);
    if (history.isVisible())
        updateHistory();
}

// The selected slot's timeline, read on the worker like everything else that
// touches the store — but without the card: the history is on this computer,
// and a tab that went blank whenever the pedal was busy would be useless at
// exactly the moment a player wants to look something up.
void MainComponent::updateHistory()
{
    if (selectedSlot <= 0) {
        history.clear();
        return;
    }
    juce::Component::SafePointer<MainComponent> safe(this);
    const int slot = selectedSlot;
    worker.enqueue({ "Read the history of slot " + juce::String(slot),
                     0,
                     [rec = recorder, slot, safe, alive = uiAlive](const volume::fs::path&) {
                         std::vector<HistoryPane::Row> rows;
                         auto entries = history::rows::forSlot(rec->store().slotTimeline(slot));
                         for (const auto& row : entries) {
                             const juce::Time when(row.at);
                             const bool today = when.getDayOfYear()
                                 == juce::Time::getCurrentTime().getDayOfYear();
                             rows.push_back({ when.formatted(today ? "%H:%M" : "%d %b %H:%M"),
                                              row.line.action, row.line.detail, row.state,
                                              row.line.audio, row.playable, row.restorable, row.op });
                         }
                         juce::MessageManager::callAsync(
                             [safe, rows, loaded = std::move(entries), slot, alive, loadedCard = rec->store().selectedCard()]() mutable {
                                 if (*alive && safe != nullptr && safe->selectedSlot == slot) {
                                     safe->historyCard = loadedCard;
                                     safe->historyEntries = std::move(loaded);
                                     safe->applyHistoryRows(std::move(rows), slot);
                                 }
                             });
                     },
                     nullptr,
                     0,
                     true,      // background: a player did not sit down to wait for it
                     true,      // quiet: only a failure is worth saying out loud
                     false }); // and it needs no card
}

namespace {
void confirmClearHistory(clearhistory::Question question, clearhistory::Answer answer)
{
    auto* dialog = new juce::AlertWindow(juce::String(question.title), juce::String(question.message),
                                        juce::MessageBoxIconType::WarningIcon);
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::returnKey),
                                  juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->addButton("Clear history", 1);
    dialog->getButton(1)->setColour(juce::TextButton::buttonColourId, juce::Colour(0xffa52d38));
    dialog->getButton(1)->setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [onAnswer = std::move(answer)](int result) { onAnswer(result == 1); }), true);
    dialog->getButton(0)->grabKeyboardFocus();
}
} // namespace

void MainComponent::clearSlotHistory(int slot)
{
    if (clearingHistory || historyEditPending || pedalBusy || !historyCard || slot != selectedSlot) return;
    clearingHistory = true;
    history.setBusy(true);
    const auto cardId = *historyCard;
    juce::Component::SafePointer<MainComponent> safe(this);
    auto settled = [safe, alive = uiAlive] {
        juce::MessageManager::callAsync([safe, alive] {
            if (!*alive || safe == nullptr) return;
            safe->clearingHistory = false;
            safe->history.setBusy(safe->pedalBusy);
            safe->updateHistory();
            safe->feedHistoryWindow();
            safe->refreshUndoOffer();
        });
    };
    PedalWorker::Job read {
        "Review the history of slot " + juce::String(slot), 0,
        [rec = recorder, cardId, slot, safe, alive = uiAlive, settled](const volume::fs::path&) {
            const auto plan = rec->store().planForgetSlot(cardId, slot);
            if (plan.inFlight) throw Error("the slot's history is still being recorded; try again when it finishes");
            juce::MessageManager::callAsync([rec, cardId, slot, safe, alive, settled, plan] {
                if (!*alive || safe == nullptr) return;
                clearhistory::ask(slot, plan, confirmClearHistory,
                    [rec, cardId, slot, safe, alive, settled, plan](bool confirmed) {
                        if (!*alive || safe == nullptr) return;
                        if (!confirmed) { settled(); return; }
                        safe->worker.enqueue(history::forgetSlotJob(rec, cardId, slot, plan,
                                                                    plan.hasHolds(), settled));
                    });
            });
        }, nullptr, 0, true, true, false
    };
    read.after = [settled](const std::string& error) { if (!error.empty()) settled(); };
    worker.enqueue(std::move(read));
}

// Listening to a take the store kept: the bytes become a file on this
// computer and the player opens it. Nothing is read from the pedal, so this
// works while the pedal is busy — "which take was the good one" is the whole
// reason the takes are kept.
void MainComponent::playFromHistory(std::int64_t op)
{
    const auto found = std::find_if(historyEntries.begin(), historyEntries.end(),
                                    [op](const history::rows::Row& row) { return row.op == op; });
    if (found == historyEntries.end() || found->takeHash.empty())
        return;
    const int slot = selectedSlot;
    playArchivedTake(slot, found->takeHash,
                     juce::String(slot) + juce::String::fromUTF8(" \xc2\xb7 ")
                         + juce::String(found->line.action)
                         + juce::String::fromUTF8(" \xc2\xb7 from the history"));
}

void MainComponent::playArchivedTake(int slot, std::string hash, juce::String title)
{
    juce::Component::SafePointer<MainComponent> safe(this);
    worker.enqueue({ "Play an archived take from slot " + juce::String(slot),
                     0,
                     [rec = recorder, aud = audition, takeHash = std::move(hash), slot, title, safe,
                      alive = uiAlive](const volume::fs::path&) {
                         const auto file = aud->materialize(rec->store(), takeHash);
                         if (!file)
                             throw Error("that take is no longer kept in the history");
                         const std::string bytes = commands::readFileBytes(*file);
                         const long long frames =
                             wav::readWavInfo(
                                 wav::BytesView(reinterpret_cast<const unsigned char*>(bytes.data()),
                                                bytes.size()))
                                 .frames;
                         const juce::File opened(juce::String(file->string()));
                         juce::MessageManager::callAsync([safe, opened, title, slot, frames, alive] {
                             if (*alive && safe != nullptr)
                                 safe->player.setSlot(slot, opened, title, false, frames);
                         });
                     },
                     nullptr, 0, true, true, false });
}

// Putting a recorded state back: the slot's body and its take, as one unit,
// through the core primitive — never through push, which would recompute the
// tempo the state carries. It is an operation like any other, so it is
// recorded, and can be undone in turn.
void MainComponent::restoreFromHistory(std::int64_t op)
{
    const auto found = std::find_if(historyEntries.begin(), historyEntries.end(),
                                    [op](const history::rows::Row& row) { return row.op == op; });
    if (found == historyEntries.end() || !found->restorable)
        return;
    const int slot = selectedSlot;
    const auto options = makeWriteOptions();
    releasePlayerIfHolding(slot, slot); // the restore rewrites the slot's audio (issue #26)
    worker.enqueue(recorded(
        "restore", options,
        { "Restore slot " + juce::String(slot) + " to " + found->line.action, slot,
          [rec = recorder, op, slot, options](const volume::fs::path& volumePath) {
              commands::SlotState state;
              for (const auto& entry : rec->store().slotTimeline(slot)) {
                  if (entry.op != op)
                      continue;
                  if (!entry.afterBody)
                      throw Error("that row recorded no state to go back to");
                  state.body = *entry.afterBody;
                  if (entry.takeHash) {
                      const auto bytes = rec->store().takeBytes(*entry.takeHash);
                      if (!bytes)
                          throw Error("the take of that state is no longer kept");
                      state.take = commands::Take { entry.takeName, *bytes };
                  }
              }
              if (state.body.empty())
                  throw Error("that row is not in this slot's history any more");
              commands::restore(volumePath, slot, state, options);
          } }));
}

void MainComponent::applyHistoryRows(std::vector<HistoryPane::Row> rows, int slot)
{
    if (slot != selectedSlot)
        return; // the player moved on while the worker was reading
    historyRows = static_cast<int>(rows.size());
    history.setRows(std::move(rows), slot);
}

// The bottom pane has four faces for the selected slot: listen to it (the
// player), set it up (its properties), its drums (the rhythm), and what
// happened to it (the history). One pane, so the table never moves.
void MainComponent::showBottomTab(int index)
{
    const bool mounted = table.isVisible();
    player.setVisible(mounted && index == kAudioTab);
    inspector.setVisible(mounted && index == kPropertiesTab);
    rhythmPane.setVisible(mounted && index == kRhythmTab);
    history.setVisible(mounted && index == kHistoryTab);
    if (mounted && index == kHistoryTab)
        updateHistory();
    resized();
}

// Settings -> Columns onto the table. The pedal's own facts always show; these
// two are the ones a player opts into.
void MainComponent::applyColumnPreferences()
{
    auto* file = settings.file();
    table.setOptionalColumns(
        file == nullptr || file->getBoolValue(kOneShotColumnKey, true),
        file != nullptr && file->getBoolValue(kCountInColumnKey, false),
        file != nullptr && file->getBoolValue(kLoudnessColumnKey, false));
}

void MainComponent::updateToolbar()
{
    const bool usable = snapshot.state == lifecycle::State::connected && snapshot.error.empty();
    disconnectButton.setEnabled(usable && !pedalBusy);
    // Connect: the pedal shows its MIDI face, no honest volume is up, no
    // attempt is already running, and the post-disconnect hold has passed
    // (the re-enumerating MIDI side eats frames — issue #2).
    connectButton.setEnabled(midiPedalPresent && !pedalBusy && !connectQueryPending
                             && snapshot.state == lifecycle::State::disconnected
                             && !connectAttempt.active() && !connectHoldActive());
    if (appMenu != nullptr)
        appMenu->menuItemsChanged(); // the Maintenance items follow the same gate
}

// The card's identity, from the card itself (issues #98, #99). The marker at
// the volume root, loopercat.toml, carries the history id and the name the
// player gave the pedal; a card without one gets one here, named
// after its model until the player renames it. On the worker, like every
// touch of the card: minting is a write, and it sweeps the sidecar macOS
// plants beside it. Quiet on success — the corner is the report — and a
// marker this build cannot read (foreign, damaged, newer) is a job error
// that reaches the toast with its reason, while the corner keeps the
// volume's label.
void MainComponent::readCardName()
{
    juce::Component::SafePointer<MainComponent> safe(this);
    PedalWorker::Job job {
        "Read the card's name",
        0,
        [safe, rec = recorder, generation = cardGeneration, alive = uiAlive](const volume::fs::path& volumePath) {
            std::optional<marker::Card> found = marker::read(volumePath);
            bool minted = false;
            std::string sweepNote;
            if (!found) {
                const std::string text = commands::readMemory(volumePath);
                const marker::Written written =
                    marker::mint(volumePath, rc0::familyOf(text).familyName);
                found = written.card;
                minted = true;
                if (!written.sweep.failed.empty())
                    sweepNote = "a sidecar would not delete: " + written.sweep.failed.front().string();
            }
            juce::MessageManager::callAsync([safe, alive, generation, c = *found, minted, sweepNote] {
                if (*alive && safe != nullptr && generation == safe->cardGeneration)
                    safe->cardNamed(c, minted, sweepNote);
            });
            const auto baseline = rec->firstSeen(volumePath);
            juce::MessageManager::callAsync([safe, alive, generation, baseline] {
                if (!*alive || safe == nullptr || generation != safe->cardGeneration) return;
                safe->firstSeenSettled = !baseline;
                if (baseline) {
                    safe->firstSeenRun = std::make_shared<history::FirstSeenRun>(*baseline);
                    safe->snapshotNext(safe->firstSeenRun, 1);
                }
                safe->updateHistory();
                safe->feedHistoryWindow();
            });
        },
        nullptr,
        0,
        false, // not background: a mint writes the card
        true,  // quiet: the corner is the report; a failure still speaks
        true   // the card is the point
    };
    // Whatever happened, the seam must not wait forever.
    job.after = [safe, generation = cardGeneration, alive = uiAlive](const std::string& error) {
        if (error.empty()) return;
        juce::MessageManager::callAsync([safe, alive, generation, error] {
            if (*alive && safe != nullptr && generation == safe->cardGeneration) {
                safe->cardNameSettled = true;
                safe->firstSeenSettled = true;
                safe->firstSeenProblem = error;
            }
        });
    };
    worker.enqueue(std::move(job));
}

void MainComponent::snapshotNext(const std::shared_ptr<history::FirstSeenRun>& run, int slot)
{
    juce::Component::SafePointer<MainComponent> safe(this);
    worker.enqueue(history::firstSeenJob(recorder, run, slot,
        [safe, run, slot, alive = uiAlive](int count, const std::string& error) {
            juce::MessageManager::callAsync([safe, run, slot, count, error, alive] {
                if (!*alive || safe == nullptr || run->cancelled) return;
                safe->firstSeenCount = count;
                safe->firstSeenProblem = error;
                safe->firstSeenSettled = !error.empty() || count == 99;
                safe->updateStatusText();
                if (safe->firstSeenSettled) {
                    safe->updateHistory();
                    safe->feedHistoryWindow();
                } else {
                    safe->snapshotNext(run, slot + 1);
                }
            });
        }));
}

void MainComponent::cardNamed(marker::Card named, bool minted, std::string sweepNote)
{
    if (snapshot.volume.empty() || snapshot.volume != cardNameVolume)
        return; // the pedal went away while the card was being read
    card = std::move(named);
    cardNameSettled = true;
    pedalLight.set(true, utf8(card->name));
    if (minted)
        toast.show(juce::String::fromUTF8("This card is now known as \xe2\x80\x9c") + utf8(card->name)
                   + juce::String::fromUTF8("\xe2\x80\x9d \xe2\x80\x94 click the name to change it"));
    if (!sweepNote.empty())
        banners.showError(banners::Source::connection, utf8(sweepNote));
    // The endpoint Connect chose carried this card: the book learns it, so
    // the next Connect can ask by name before the card is readable.
    if (connectTarget) {
        pedalBook.remember({ connectTarget->endpoint.identifier.toStdString(), card->id, card->name,
                             card->model, static_cast<std::int64_t>(juce::Time::currentTimeMillis()) });
        savePedalBook();
    }
}

// Rename the pedal: the marker's name changes, nothing else on the card
// does (issue #99). Asked in a small dialog on the name itself; written on
// the worker; said out loud when done.
void MainComponent::renamePedal()
{
    if (!card || snapshot.volume.empty() || pedalBusy)
        return;
    auto* ask = new juce::AlertWindow("Name this pedal",
                                      "The name is written on the card and shown in the corner. "
                                      "Loops, settings and history are untouched.",
                                      juce::MessageBoxIconType::NoIcon);
    ask->addTextEditor("name", utf8(card->name));
    ask->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    ask->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    ask->enterModalState(true, juce::ModalCallbackFunction::create([safe, ask](int result) {
        if (result != 1 || safe == nullptr || !safe->card)
            return;
        const std::string newName = ask->getTextEditorContents("name").trim().toStdString();
        if (newName.empty() || newName == safe->card->name)
            return;
        auto* self = safe.getComponent();
        const std::string cardId = self->card->id;
        juce::Component::SafePointer<MainComponent> again(self);
        self->worker.enqueue({ "Rename the pedal to \xe2\x80\x9c" + utf8(newName) + "\xe2\x80\x9d",
                               0,
                               [newName, again, rec = self->recorder, alive = self->uiAlive](const volume::fs::path& volumePath) {
                                   const marker::Written written = marker::rename(volumePath, newName);
                                   rec->selectVolume(volumePath); // update the stored name under the same id
                                   juce::MessageManager::callAsync([again, alive, c = written.card] {
                                       if (*alive && again != nullptr)
                                           again->cardNamed(c, false, {});
                                   });
                               },
                               nullptr, 0, false, false, true });
        self->pedalBook.renamed(cardId, newName);
        self->savePedalBook();
    }), true);
}

void MainComponent::savePedalBook()
{
    if (auto* file = settings.file()) {
        file->setValue(kPedalBookKey, juce::String::fromUTF8(pedalBook.serialize().c_str()));
        file->saveIfNeeded();
    }
}

void MainComponent::runCleanJunk()
{
    worker.enqueue({ "Clean junk", 0,
                     [family = snapshot.family](const volume::fs::path& volumePath) {
                         // The sweep writes to the card. A card of a model this
                         // app only reads is refused the way the core refuses
                         // every write to it (DeviceProfile.hpp), in its words.
                         if (!CardPermissions::of(family).anyWrite())
                             throw Error("clean junk refused on an \"" + family
                                         + "\" card \xe2\x80\x94 " + profile::onlySpeaks());
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
    // The About popover is the badge's own; appkit v0.11.3 made showPopup()
    // public so a menu can cast it without faking a click.
    badge.showPopup();
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
    if ((connectAttempt.active() || connectQueryPending) && snapshot.volume.empty()) {
        status.setText(juce::String::fromUTF8("Connecting to the pedal\xe2\x80\xa6"),
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kStatusText);
        return;
    }
    if (snapshot.volume.empty()) {
        status.setText(otherLooperOnBus.isNotEmpty() && !midiPedalPresent
                           ? otherLooperOnBus
                                 + juce::String::fromUTF8(" on USB \xe2\x80\x94 not spoken to")
                           : utf8(presenceWords.status),
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
    // A memory holds a loop when any of its tracks does — on the RC-5 that
    // is its one track, as ever.
    if (firstSeenRun && !firstSeenRun->cancelled && !firstSeenSettled) {
        status.setText("Saving card first seen: " + juce::String(firstSeenCount) + " of 99 slots",
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kStatusText);
        return;
    }
    int loaded = 0;
    for (const auto& row : snapshot.slots)
        for (const auto& track : row.info.tracks)
            if (track.hasAudio) {
                ++loaded;
                break;
            }
    juce::String text = volumeDisplayName() + juce::String::fromUTF8("  \xe2\x80\x94  ");
    // Another model's card is read and never written; the status says which
    // model, by the card's own name, and that it is read-only.
    if (!CardPermissions::of(snapshot.family).anyWrite())
        text << utf8(snapshot.family) << juce::String::fromUTF8(" card, read-only  \xc2\xb7  ");
    text << juce::String(loaded) << " of " << juce::String(snapshot.slots.size())
         << " slots hold a loop";
    if (snapshot.freeBytes > 0)
        text << juce::String::fromUTF8("  \xc2\xb7  ")
             << juce::String(static_cast<double>(snapshot.freeBytes) / 1.0e9, 1) << " GB free";
    if (pedalBusy)
        text << juce::String::fromUTF8("  \xc2\xb7  working\xe2\x80\xa6");
    if (checkId != 0)
        text << juce::String::fromUTF8("  \xc2\xb7  checking loudness ")
             << juce::String(checkDone + checkFailed) << " / " << juce::String(checkTotal)
             << (checkStopping ? juce::String::fromUTF8(", stopping\xe2\x80\xa6") : juce::String());
    if (deviceError.isNotEmpty())
        text << juce::String::fromUTF8("  \xc2\xb7  audio device: ") << deviceError;
    status.setText(text, juce::dontSendNotification);
    status.setColour(juce::Label::textColourId, kStatusText);
}

void MainComponent::applySnapshot(const PedalSnapshot& latest)
{
    const lifecycle::State previousState = snapshot.state;
    const bool anotherVolume = latest.volume != snapshot.volume;
    if (anotherVolume)
        table.clearAllLoudness(); // another card is another set of files; readings do not travel
    snapshot = latest;
    if (anotherVolume)
        refreshUndoOffer(); // what Undo offers is read for the card in front of it
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
        for (const auto& row : snapshot.slots) {
            stillThere = stillThere || utf8(row.wavPath) == player.currentPath();
            for (const auto& trackPath : row.trackPaths) // a mix's identity is its first take
                stillThere = stillThere
                          || (!trackPath.empty() && utf8(trackPath) == player.currentPath());
        }
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

    // The corner wears the card's own name (issue #99) once the marker has
    // been read; until then, the volume's label. One read per mount: the
    // volume path changing is what makes it a new card.
    if (!mounted) {
        if (!cardNameVolume.empty()) {
            ++cardGeneration;
            if (firstSeenRun) firstSeenRun->cancelled = true;
            worker.enqueue({ "Close the card's history session", 0,
                             [rec = recorder](const volume::fs::path&) { rec->disconnect(); },
                             nullptr, 0, false, true, false });
        }
        firstSeenSettled = false;
        card.reset();
        cardNameVolume.clear();
        cardNameSettled = false;
    } else if (snapshot.volume != cardNameVolume) {
        ++cardGeneration;
        if (firstSeenRun) firstSeenRun->cancelled = true;
        firstSeenSettled = false;
        firstSeenProblem.clear();
        firstSeenCount = 0;
        card.reset();
        cardNameVolume = snapshot.volume;
        cardNameSettled = false;
        trace("connect: card up at " + utf8(snapshot.volume) + " (" + utf8(snapshot.family) + ")");
        readCardName();
    }
    if (card) {
        pedalLight.set(true, utf8(card->name));
    } else {
#if JUCE_WINDOWS
        // "D:\" has no filename to show — the light wears the volume label.
        pedalLight.set(mounted, juce::File(utf8(snapshot.volume)).getVolumeLabel());
#else
        pedalLight.set(mounted, utf8(volume::fs::path(snapshot.volume).filename().string()));
#endif
    }

    updateTableRows();
    updateToolbar();
    table.setVisible(mounted);
    bottomTabs.setVisible(mounted);
    hint.setVisible(!mounted);
    showBottomTab(bottomTabs.selected()); // the pane follows the pedal in and out
    if (!mounted)
        selectedSlot = 0;
    updateInspector();

    restoreListening();
}

// A mutation releases the preview of the slot it rewrites (issue #26 —
// Windows will not let a held file be replaced), and the job itself never
// reloads it: the row refreshed but the player said "Select a slot to
// listen" until the user clicked away and back. The invariant — the selected
// occupied slot is the one in the player — restores at BOTH ends of a
// mutation's tail, because the worker's delivery order is snapshot → result
// → busy(false) (PedalWorker::run): at snapshot-apply time the job still
// counts as busy and the guard below rightly stays hands-off, so the busy
// drop is the hook that actually fires after a job; the snapshot-apply hook
// covers mounts and idle rescans. slotChosen is idempotent (same path →
// no-op), and an unmounted or empty selection is a no-op/clear by its own
// rules.
void MainComponent::restoreListening()
{
    if (!pedalBusy && selectedSlot > 0)
        slotChosen(selectedSlot, false);
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
    if (row.wavPath.empty() && row.info.tracks.size() <= 1) {
        player.clear(); // an empty slot: nothing to listen to
        return;
    }

    const juce::String title = juce::String(row.info.slot).paddedLeft('0', 2) + "  "
                             + trimmedName(row);
    if (row.info.tracks.size() > 1) {
        // A multi-track memory plays as its mix, every take at its own level
        // (TRACK<n>/PlyLvl, 100 = unity); the pane's identity is the first
        // take it holds, so a memory whose take sits on track 2 alone is a
        // memory that plays track 2.
        std::vector<PlayerPane::TrackFile> tracks;
        juce::String firstPath;
        for (std::size_t i = 0; i < row.info.tracks.size(); ++i) {
            const std::string& trackPath =
                i < row.trackPaths.size() ? row.trackPaths[i] : std::string();
            tracks.push_back({ trackPath.empty() ? juce::File() : juce::File(utf8(trackPath)),
                               static_cast<float>(row.info.tracks[i].level) / 100.0f });
            if (firstPath.isEmpty() && !trackPath.empty())
                firstPath = utf8(trackPath);
        }
        if (firstPath.isEmpty()) {
            player.clear(); // no take on any track: nothing to listen to
            return;
        }
        if (firstPath != player.currentPath())
            player.setTracks(row.info.slot, tracks, title, row.info.oneShot);
    } else {
        const juce::String path = utf8(row.wavPath);
        if (path != player.currentPath())
            player.setSlot(row.info.slot, juce::File(path), title, row.info.oneShot,
                           row.info.frames);
    }
    player.setTempoNote(tempoNoteFor(row.info));
    // The loudness readout follows the loaded loop: whatever the column knows
    // about it — a check's answer, a read in flight — shows in the player row.
    if (const SlotTable::LoudnessCell* cell = table.loudnessFor(row.info.slot)) {
        if (cell->pending)
            player.setLoudnessPending(row.info.slot);
        else
            player.setLoudness(row.info.slot, cell->detail, cell->attention, cell->damaged,
                               cell->tooltip);
    }
    if (startPlaying && engine.hasSource() && !engine.isPlaying())
        engine.play();
}

// --- mutations ---

commands::WriteOptions MainComponent::makeWriteOptions()
{
    return history::makeWriteOptions(recorder);
}

// The operation opens in the history once the worker has let the job through
// and before it touches the card, and closes with the job's outcome.
PedalWorker::Job MainComponent::recorded(const char* kind, const commands::WriteOptions& options,
                                         PedalWorker::Job job, std::vector<int> alsoAbout)
{
    // The slots the operation is about — the job's own, and any the caller
    // adds — go down right after it opens, before the card is touched, so an
    // operation that then changes nothing (a normalize that finds its slot
    // at target) still keeps its slot in the history (#144).
    std::vector<int> about;
    if (job.slot > 0)
        about.push_back(job.slot);
    about.insert(about.end(), alsoAbout.begin(), alsoAbout.end());
    job.before = [rec = recorder, id = options.opId, k = std::string(kind), about](
                     const volume::fs::path& volumePath) {
        rec->begin(id, k, volumePath);
        for (const int slot : about)
            rec->subject(id, slot);
    };
    // The job's own line goes into the history with it. Without it an
    // operation that wrote nothing — a normalize that found the slot already
    // at target — leaves a row that says only "normalize", and the reason
    // lives nowhere but a toast that is already gone.
    job.after = [rec = recorder, id = options.opId, note = job.note](const std::string& error) {
        rec->finish(id, error, note != nullptr ? note->toStdString() : std::string());
    };
    return job;
}

void MainComponent::showSlotMenu(int slot, juce::Point<int> screenPosition)
{
    const SlotRow* found = pedalBusy ? nullptr : slotRowFor(slot);
    if (found == nullptr)
        return;
    const SlotRow& row = *found;
    const bool occupied = row.info.hasAudio;
    const juce::String name = trimmedName(row);

    // A card of another model is read, never written (DeviceProfile.hpp):
    // the menu offers nothing that would change it, and says so. Pull stays
    // closed too until it learns tracks — a two-track memory pulled as one
    // folder would be half a memory.
    if (!CardPermissions::of(snapshot.family).anyWrite()) {
        juce::PopupMenu readOnly;
        readOnly.addItem(4, juce::String::fromUTF8("Pull to folder\xe2\x80\xa6"), false);
        readOnly.addSeparator();
        readOnly.addItem(12, juce::String(CardPermissions::writesOnlyTo()), false);
        readOnly.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
                                   { screenPosition.x, screenPosition.y, 1, 1 }),
                               [](int) {});
        return;
    }

    // Operations only: the things done TO a slot. Its settings live in the
    // panel now (and the two lamp columns still flip on click), so this menu
    // stopped being a second copy of them.
    juce::PopupMenu menu;
    menu.addItem(3, juce::String::fromUTF8(occupied ? "Replace audio\xe2\x80\xa6" : "Push audio here\xe2\x80\xa6"));
    menu.addItem(4, juce::String::fromUTF8("Pull to folder\xe2\x80\xa6"), occupied);
    juce::PopupMenu downmix;
    downmix.addItem(6, juce::String::fromUTF8("Both outputs\xe2\x80\xa6"));
    downmix.addItem(7, juce::String::fromUTF8("OUTPUT A only\xe2\x80\xa6"));
    downmix.addItem(8, juce::String::fromUTF8("OUTPUT B only\xe2\x80\xa6"));
    menu.addSubMenu(juce::String::fromUTF8("Downmix to mono"), downmix, occupied);
    // The label names the target so the choice is informed before the dialog:
    // the number comes from Settings -> Import, shared with normalize-on-upload.
    const double normalizeTarget = settings.file() != nullptr
        ? settings.file()->getDoubleValue(kNormalizeTargetLufsKey, kDefaultTargetLufs)
        : kDefaultTargetLufs;
    menu.addItem(9,
                 "Normalize to " + SettingsDialog::formatLufs(normalizeTarget)
                     + juce::String::fromUTF8(" LUFS\xe2\x80\xa6"),
                 occupied);
    // The read-only counterpart (issue #61), and the way to stop a running
    // background check from wherever the player happens to right-click.
    if (checkId != 0)
        menu.addItem(11, "Stop loudness check");
    else
        menu.addItem(10, "Check loudness", occupied);
    menu.addSeparator();
    clearSlotAction::addToMenu(menu, occupied);

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
            case clearSlotAction::menuItemId: clearSlot(slot); break;
            case 6: downmixSlot(slot, name, wav::Placement::BothOutputs); break;
            case 7: downmixSlot(slot, name, wav::Placement::OutputAOnly); break;
            case 8: downmixSlot(slot, name, wav::Placement::OutputBOnly); break;
            case 9: normalizeSlot(slot, name); break;
            case 10: measureSlotLoudness(slot); break;
            case 11: stopLoudnessCheck(); break;
            default: break;
            }
        });
}

void MainComponent::toggleOneShot(int slot, bool currentlyOn)
{
    if (!CardPermissions::of(snapshot.family).oneShot)
        return; // a card this app only reads: the pill is a lamp
    const auto options = makeWriteOptions();
    worker.enqueue(recorded("oneshot", options,
                            { juce::String(currentlyOn ? "Disable" : "Enable")
                                  + " One Shot on slot " + juce::String(slot),
                              slot,
                              [slot, on = !currentlyOn, options](const volume::fs::path& volumePath) {
                                  commands::setOneShot(volumePath, { slot }, on, options);
                              } }));
}

void MainComponent::toggleCountIn(int slot, bool currentlyOn)
{
    if (!CardPermissions::of(snapshot.family).countIn)
        return;
    const auto options = makeWriteOptions();
    worker.enqueue(recorded("countin", options,
                            { juce::String(currentlyOn ? "Disable" : "Enable")
                                  + " Play Count-In on slot " + juce::String(slot),
                              slot,
                              [slot, on = !currentlyOn, options](const volume::fs::path& volumePath) {
                                  commands::setCountIn(volumePath, { slot }, on, options);
                              } }));
}

// One job per change on the Rhythm card: the switch, or one of the RHYTHM
// screen's fields. The change rides as the job's note, in the card's own
// words (usecases::rhythm::describe): the banner reads "Rhythm on slot 7 —
// kit Jazz", and the history row keeps the same words, not "field 5 = 2".
void MainComponent::editRhythm(int slot, usecases::rhythm::Edits edits)
{
    if (!CardPermissions::of(snapshot.family).rhythm)
        return;
    const auto options = makeWriteOptions();
    worker.enqueue(recorded("rhythm", options,
                            { "Rhythm on slot " + juce::String(slot),
                              slot,
                              [slot, edits, options](const volume::fs::path& volumePath) {
                                  commands::setRhythm(volumePath, slot, edits, options);
                              },
                              std::make_shared<juce::String>(
                                  utf8(usecases::rhythm::describe(edits))) }));
}

// One job per change on the Start & Stop card, the twin of editRhythm: the
// change rides as the job's note in the card's own words ("stop LOOP END").
void MainComponent::editPlayStop(int slot, usecases::playstop::Edits edits)
{
    if (!CardPermissions::of(snapshot.family).playStop)
        return;
    const auto options = makeWriteOptions();
    worker.enqueue(recorded("playstop", options,
                            { "Start & Stop on slot " + juce::String(slot),
                              slot,
                              [slot, edits, options](const volume::fs::path& volumePath) {
                                  commands::setPlayStop(volumePath, slot, edits, options);
                              },
                              std::make_shared<juce::String>(
                                  utf8(usecases::playstop::describe(edits))) }));
}

void MainComponent::choosePushWav(int slot, bool slotOccupied)
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choose audio for slot " + juce::String(slot),
        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        "*.wav;*.mp3;*.aiff;*.aif;*.flac;*.ogg");
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
        // The normalize preference is read HERE, when the player acts — a
        // settings change mid-queue must not rewrite jobs already promised.
        std::optional<double> normalizeTarget;
        if (auto* file = settings.file();
            file != nullptr && file->getBoolValue(kNormalizeOnUploadKey, false))
            normalizeTarget = file->getDoubleValue(kNormalizeTargetLufsKey, kDefaultTargetLufs);
        auto note = std::make_shared<juce::String>();
        const auto options = makeWriteOptions();
        worker.enqueue(recorded("push", options, { "Push " + juce::File(sourcePath).getFileName() + " to slot "
                             + juce::String(slot),
                         slot,
                         [source = sourcePath, slot, force, normalizeTarget, note,
                          options,
                          importTmp = settings.dataDir().getChildFile("import-tmp"),
                          logDir = settings.dataDir()](
                             const volume::fs::path& volumePath) {
                             // DAW exports arrive as anything — convert off the
                             // message thread, on this worker, before the push
                             // (issue #20). The temp conversion dies with the job.
                             wavimport::Prepared prepared;
                             const juce::Result ok =
                                 wavimport::prepare(juce::File(source), importTmp, prepared,
                                                    { .normalizeTargetLufs = normalizeTarget });
                             if (ok.failed())
                                 throw Error(ok.getErrorMessage().toStdString());
                             commands::PushResult pushed;
                             try {
                                 pushed = commands::push(volumePath,
                                                         prepared.file.getFullPathName().toStdString(),
                                                         slot,
                                                         { .force = force, .write = options });
                             } catch (...) {
                                 if (prepared.converted)
                                     prepared.file.deleteFile();
                                 throw;
                             }
                             if (prepared.converted)
                                 prepared.file.deleteFile();
                             if (prepared.normalize.has_value() && normalizeTarget.has_value()) {
                                 *note = describeNormalize(*prepared.normalize, *normalizeTarget);
                                 oplog::append(logDir,
                                               "push " + juce::File(source).getFileName()
                                                   + " to slot " + juce::String(slot) + ": "
                                                   + *note);
                             }
                             // The slot's length was a note value and this take
                             // replaced it with a bar count (issue #92): said out
                             // loud, in the toast and in the row, never in silence.
                             if (pushed.noteLengthReplaced) {
                                 const juce::String fact(history::story::kNoteLengthReplaced.data(),
                                                         history::story::kNoteLengthReplaced.size());
                                 *note << (note->isEmpty() ? "" : "; ") << fact;
                                 oplog::append(logDir,
                                               "push " + juce::File(source).getFileName()
                                                   + " to slot " + juce::String(slot) + ": " + fact);
                             }
                         },
                         note }));
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
                "This slot already holds a loop. The current WAV is kept in the "
                "history \xe2\x80\x94 that is your undo."))
            .withButton("Replace")
            .withButton("Cancel"),
        [enqueuePush](int button) {
            if (button == 1)
                enqueuePush(true);
        });
}

// Folding is the app's half of "put this loop on one output jack" (issue
// #43): the pedal's own Pan does the placing, but it cannot fold a file it
// was handed, and a loop whose channels already match lands whole on either
// jack however Pan turns out to be implemented. Destructive by nature — the
// two channels stop being separable — so it asks first and keeps the stereo
// original in the history, exactly like a replace.
void MainComponent::downmixSlot(int slot, const juce::String& name, wav::Placement placement)
{
    const juce::String label = name.isEmpty() ? juce::String(slot)
                                              : juce::String(slot) + " (" + name + ")";
    const juce::String where(wav::placementName(placement));

    // The fold itself is one sentence; where the result goes is the part a
    // user is deciding, so each placement says what the OTHER jack does.
    const juce::String consequence =
        placement == wav::Placement::BothOutputs
            ? juce::String::fromUTF8(
                  "Left and right are averaged into one signal, and both channels then carry "
                  "it \xe2\x80\x94 the loop stops being stereo.")
            : juce::String::fromUTF8("Left and right are averaged into one signal, which goes to ")
                  + (placement == wav::Placement::OutputAOnly ? "OUTPUT A" : "OUTPUT B")
                  + juce::String::fromUTF8(" alone. ")
                  + (placement == wav::Placement::OutputAOnly ? "OUTPUT B" : "OUTPUT A")
                  + juce::String::fromUTF8(
                      " stays silent for this loop, so that jack is free for your instrument.");

    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Downmix slot " + label + " to mono, " + where + "?")
            .withMessage(consequence
                         + juce::String::fromUTF8(
                             "\n\nThe current WAV is kept in the history \xe2\x80\x94 "
                             "that is your undo."))
            .withButton("Downmix")
            .withButton("Cancel"),
        [this, slot, placement, where](int button) {
            if (button != 1)
                return;
            releasePlayerIfHolding(slot, slot); // the fold rewrites the WAV under preview (issue #26)
            const auto options = makeWriteOptions();
            worker.enqueue(recorded(
                "downmix", options,
                { "Downmix slot " + juce::String(slot) + " to mono, " + where, slot,
                  [slot, placement, options](const volume::fs::path& volumePath) {
                      commands::downmixToMono(volumePath, slot,
                                              { .placement = placement, .write = options });
                  } }));
        });
}

// Levelling a loop that is already on the card (issue #53) — the on-card
// sibling of normalize-on-upload, for setlists assembled before the option
// existed. Destructive in the same sense as the fold (the original loudness
// stops being recoverable from the file), so it asks first and keeps the
// original in the history. The target is the shared one from Settings → Import.
void MainComponent::normalizeSlot(int slot, const juce::String& name)
{
    const double target = settings.file() != nullptr
        ? settings.file()->getDoubleValue(kNormalizeTargetLufsKey, kDefaultTargetLufs)
        : kDefaultTargetLufs;
    const juce::String label = name.isEmpty() ? juce::String(slot)
                                              : juce::String(slot) + " (" + name + ")";
    const juce::String targetText = SettingsDialog::formatLufs(target) + " LUFS";

    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Normalize slot " + label + " to " + targetText + "?")
            .withMessage(juce::String::fromUTF8(
                             "One constant gain lands the whole loop at the target loudness "
                             "\xe2\x80\x94 nothing else about the sound changes.\n\n"
                             "The current WAV is kept in the history \xe2\x80\x94 "
                             "that is your undo."))
            .withButton("Normalize")
            .withButton("Cancel"),
        [this, slot, target](int button) {
            if (button == 1)
                enqueueNormalize(slot, target);
        });
}

// One normalize job for the worker queue — the shared tail of the single-slot
// dialog and the bulk apply. Per-slot jobs on purpose: the row's busy pulse
// and error isolation come free, and one stubborn slot cannot stop the rest.
// Batch jobs carry the batch id (for crediting and cancel) and feed the
// overlay's current-file bar through `filePermille` (issue #61).
void MainComponent::enqueueNormalize(int slot, double target, int batch,
                                     std::shared_ptr<std::atomic<int>> filePermille)
{
    releasePlayerIfHolding(slot, slot); // the rewrite happens under preview (issue #26)
    auto note = std::make_shared<juce::String>();
    const auto options = makeWriteOptions();
    worker.enqueue(recorded(
        "normalize", options,
        { "Normalize slot " + juce::String(slot), slot,
          [slot, target, note, filePermille, options,
           logDir = settings.dataDir()](
              const volume::fs::path& volumePath) {
              if (filePermille != nullptr)
                  filePermille->store(0); // this job's file starts from zero
              const commands::NormalizeResult result = commands::normalize(
                  volumePath, slot,
                  { .targetLufs = target, .write = options,
                    .progress = filePermille != nullptr
                        ? std::function<void(double)>([filePermille](double v) {
                              filePermille->store(static_cast<int>(v * 1000.0));
                          })
                        : std::function<void(double)>() });
              *note = describeNormalize(result, target);
              oplog::append(logDir, "normalize slot " + juce::String(slot) + ": " + *note);
          },
          note, batch }));
}

// Batch takeover (issue #61): the overlay swallows every click until the last
// result lands, so nothing can jump onto a slot mid-rewrite. The player is
// silenced up front — not per job — because a batch and a preview have no
// honest way to coexist.
void MainComponent::startNormalizeBatch(const std::vector<int>& slots, double target,
                                        const juce::String& targetText)
{
    player.clear();
    batchId = ++batchCounter;
    batchTotal = static_cast<int>(slots.size());
    batchDone = batchFailed = batchUntouched = batchDropped = 0;
    batchFilePermille = std::make_shared<std::atomic<int>>(0);
    for (const int slot : slots)
        enqueueNormalize(slot, target, batchId, batchFilePermille);
    batchOverlay.begin("Normalizing " + juce::String(batchTotal) + " slots to " + targetText,
                       batchTotal, batchFilePermille);
}

void MainComponent::endNormalizeBatch()
{
    batchOverlay.end();
    batchId = 0;
    // One summary instead of a toast per slot; every line is in operations.log.
    const int normalized = batchDone - batchUntouched;
    juce::String story = "Batch done: " + juce::String(normalized)
                       + (normalized == 1 ? " slot" : " slots") + " normalized";
    if (batchUntouched > 0)
        story << ", " << juce::String(batchUntouched) << " already at target";
    if (batchFailed > 0)
        story << ", " << juce::String(batchFailed) << " failed";
    if (batchDropped > 0)
        story << ", " << juce::String(batchDropped) << " cancelled before starting";
    if (normalized > 0)
        story << juce::String::fromUTF8(" \xe2\x80\x94 Disconnect to hear it on the pedal.");
    toast.show(story);
}

// The Measure button (issue #53): the inspector's loudness row answers only
// when asked, because the answer costs reading the whole WAV off the card. It
// runs as a worker job like every mutation — same busy pulse, same error
// banner, and serialized against rewrites so it can never read a half-written
// take — but it writes nothing: no archive, no journal line, no Disconnect hint.
double MainComponent::currentTargetLufs()
{
    auto* file = settings.file();
    return file != nullptr ? file->getDoubleValue(kNormalizeTargetLufsKey, kDefaultTargetLufs)
                           : kDefaultTargetLufs;
}

// One reading, two audiences: the inspector wants the sentence, the column
// wants the number — and both want to be told when the number is not one.
MainComponent::LoudnessReport MainComponent::describeReading(const wav::LoudnessReading& reading,
                                                             double targetLufs)
{
    const juce::String target = SettingsDialog::formatLufs(targetLufs);
    if (reading.wildSamples > 0) {
        // The number the meter would print here is real — and meaningless:
        // 2.4e38 is not a loudness, it is a foreign header read as float
        // (the 2026-09-02 recovered card). The row gets a sign and a word;
        // the hint gets the story.
        const juce::String what = "damaged audio: " + juce::String(reading.wildSamples)
                                + " impossible sample value(s)";
        return { "damaged", "damaged audio", what,
                 "This file contains bytes that are not sound. Re-push the loop from its "
                 "original; Normalize will not touch it.",
                 true, true };
    }
    if (!reading.integratedLufs.has_value())
        return { "n/a", "silent or too short to measure", "silent or too short to measure",
                 "Nothing to measure: silence, or under 400 ms of audio.", false, false };
    const double lufs = *reading.integratedLufs;
    const double wanted = targetLufs - lufs;
    const bool offTarget = std::abs(wanted) >= loudness::kAlreadyAtTargetLu;
    // Attention means "Normalize would change this" — so the readout runs
    // the command's own gain rule. A quiet loop whose peaks already touch the
    // ceiling is off target and yet has nothing to gain (field report: a slot
    // painted orange that the command then rightly refused); it reads grey
    // with the reason, and a partial boost says how much is actually there.
    const double gain = !offTarget ? 0.0
                      : std::isfinite(reading.truePeakDb)
                          ? loudness::normalizeGainDb(lufs, targetLufs, reading.truePeakDb,
                                                      loudness::kPeakCeilingDb)
                          : wanted;
    const bool wouldChange = offTarget && std::abs(gain) > 1.0e-9;
    const bool capped = wanted > 0.0 && gain + 1.0e-9 < wanted;
    juce::String row = juce::String(lufs, 1) + juce::String::fromUTF8(" LUFS \xc2\xb7 ");
    if (!offTarget)
        row << "at target " << target;
    else {
        row << juce::String(std::abs(wanted), 1) << " dB " << (wanted > 0.0 ? "below" : "above")
            << " target " << target;
        if (!wouldChange)
            row << juce::String::fromUTF8(" \xc2\xb7 peak-limited, nothing to gain");
        else if (capped)
            row << juce::String::fromUTF8(" \xc2\xb7 only +") << juce::String(gain, 1)
                << " dB possible";
    }
    const juce::String peak = juce::String(reading.truePeakDb, 1) + " dBTP";
    juce::String tip = "Peak " + peak + juce::String::fromUTF8(" \xc2\xb7 target ") + target + " LUFS.";
    if (offTarget && !wouldChange)
        tip << " Cannot be raised without clipping.";
    else if (capped)
        tip << " Only +" << juce::String(gain, 1) << " dB fits without clipping.";
    return { juce::String(lufs, 1), row, row + ", peak " + peak, tip, wouldChange, false };
}

void MainComponent::enqueueLoudnessRead(int slot, double target, int batch)
{
    // Every loudness read is background work: read-only, never a reason to
    // lock the UI. The cell says "…" until the answer lands, and the one in
    // flight breathes (SlotTable::paintCell).
    table.setLoudness(slot, { juce::String::fromUTF8("\xe2\x80\xa6"), false, true });
    player.setLoudnessPending(slot); // ignored unless that slot is loaded
    auto note = std::make_shared<juce::String>();
    juce::Component::SafePointer<MainComponent> safe(this);
    worker.enqueue(
        { "Check slot " + juce::String(slot) + " loudness",
          slot,
          [slot, target, batch, note, safe](const volume::fs::path& volumePath) {
              const std::vector<std::string> files = volume::listSlotWavs(volumePath, slot);
              if (files.empty())
                  throw Error("slot " + std::to_string(slot) + " has no audio to measure");
              const std::string raw =
                  commands::readFileBytes(volume::wavDir(volumePath, slot) / files.front());
              const wav::LoudnessReading reading = wav::measureLoudness(wav::BytesView(
                  reinterpret_cast<const unsigned char*>(raw.data()), raw.size()));
              const LoudnessReport report = describeReading(reading, target);
              *note = report.noteText;
              juce::MessageManager::callAsync([safe, slot, report, batch] {
                  if (safe != nullptr)
                      safe->applyLoudnessReport(slot, report, batch);
              });
          },
          note, batch, /*background=*/true });
}

void MainComponent::applyLoudnessReport(int slot, const LoudnessReport& report, int batch)
{
    table.setLoudness(slot, { report.cellText, report.attention, false, report.rowText,
                              report.damaged, report.tooltipText });
    player.setLoudness(slot, report.rowText, report.attention, report.damaged,
                       report.tooltipText); // ignored unless that slot is loaded
    if (checkId != 0 && batch == checkId) {
        if (report.attention)
            ++checkAttention;
        if (report.damaged)
            ++checkDamaged;
    }
}

// One slot's read (issue #53) on demand: the menu's Check loudness, a
// double-click on the LUFS dash. A worker job like every mutation — same row
// pulse, same error banner, serialized against rewrites so it can never read
// a half-written take — but it writes nothing: no archive, no journal line, no
// Disconnect hint, no lock. (The loaded slot needs none of this: the player's
// own read pass meters it along with the waveform.)
void MainComponent::measureSlotLoudness(int slot)
{
    enqueueLoudnessRead(slot, currentTargetLufs(), 0);
}

// The background loudness check (issue #61): the same read over a selection,
// as background jobs — a mutation the player asks for meanwhile jumps ahead,
// nothing locks, rows pulse as they are read and the LUFS column fills in.
// Read-only, so no dialog: the cost is time, and Esc gives it back.
void MainComponent::startLoudnessCheck(const std::vector<int>& slots)
{
    if (slots.empty() || checkId != 0)
        return;
    const double target = currentTargetLufs();
    checkId = ++batchCounter;
    checkTotal = static_cast<int>(slots.size());
    checkDone = checkFailed = checkAttention = checkDamaged = 0;
    checkStopping = false;
    checkSlots = slots;
    for (const int slot : slots)
        enqueueLoudnessRead(slot, target, checkId);
    toast.show("Checking the loudness of " + juce::String(checkTotal)
               + (checkTotal == 1 ? " slot" : " slots")
               + juce::String::fromUTF8(" in the background \xe2\x80\x94 Esc stops it."));
    updateStatusText();
}

void MainComponent::stopLoudnessCheck()
{
    if (checkId == 0)
        return;
    checkTotal -= worker.cancelPending(checkId); // the dropped tail never reports back
    for (const int slot : checkSlots)
        table.clearPendingLoudness(slot); // …so its "…" cells go back to the dash
    checkStopping = true;
    if (checkDone + checkFailed >= checkTotal)
        finishLoudnessCheck(); // nothing in flight — over now
    else
        updateStatusText(); // the slot in flight finishes, then the summary
}

void MainComponent::finishLoudnessCheck()
{
    juce::String story = juce::String(checkStopping ? "Loudness check stopped: "
                                                    : "Loudness check done: ")
                       + juce::String(checkDone) + " measured";
    const int offTarget = checkAttention - checkDamaged;
    if (offTarget > 0)
        story << ", " << juce::String(offTarget) << " off target";
    if (checkDamaged > 0)
        story << ", " << juce::String(checkDamaged) << " damaged";
    if (checkFailed > 0)
        story << ", " << juce::String(checkFailed) << " failed";
    toast.show(story);
    checkId = 0;
    checkStopping = false;
    updateStatusText();
}

// The selection menu (issue #53): right-click inside a 2+ row selection.
// One entry today — Normalize is the first operation safe enough to run over
// a whole selection; the menu grows as
// operations earn their way in.
void MainComponent::showSlotsMenu(std::vector<int> slots, juce::Point<int> screenPosition)
{
    if (pedalBusy)
        return;
    // A card this app only reads: the selection menu has nothing to offer
    // but the reason (the loudness check reads, but its answer is a
    // normalize this card cannot take).
    if (!CardPermissions::of(snapshot.family).normalize) {
        juce::PopupMenu readOnly;
        readOnly.addItem(12, juce::String(CardPermissions::writesOnlyTo()), false);
        readOnly.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
                                   { screenPosition.x, screenPosition.y, 1, 1 }),
                               [](int) {});
        return;
    }
    std::vector<int> occupied;
    for (const int slot : slots)
        if (const SlotRow* row = slotRowFor(slot); row != nullptr && row->info.hasAudio)
            occupied.push_back(slot);

    const double target = settings.file() != nullptr
        ? settings.file()->getDoubleValue(kNormalizeTargetLufsKey, kDefaultTargetLufs)
        : kDefaultTargetLufs;
    const juce::String targetText = SettingsDialog::formatLufs(target) + " LUFS";

    juce::PopupMenu menu;
    const juce::String count =
        juce::String(occupied.size()) + (occupied.size() == 1 ? " slot" : " slots");
    // Look before you leap: the read-only check leads, the rewrite follows.
    if (checkId != 0)
        menu.addItem(3, "Stop loudness check");
    else
        menu.addItem(2, "Check loudness (" + count + ")", !occupied.empty());
    menu.addItem(1, "Normalize " + count + " to " + targetText + juce::String::fromUTF8("\xe2\x80\xa6"),
                 !occupied.empty());
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetScreenArea({ screenPosition.x, screenPosition.y, 1, 1 }),
        [this, occupied, target, targetText](int choice) {
            if (choice == 2) {
                startLoudnessCheck(occupied);
                return;
            }
            if (choice == 3) {
                stopLoudnessCheck();
                return;
            }
            if (choice != 1 || occupied.empty())
                return;
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle("Normalize " + juce::String(occupied.size()) + " slots to "
                               + targetText + "?")
                    .withMessage(juce::String::fromUTF8(
                        "Each loop gets its own constant gain to land at the target loudness; "
                        "a loop already there is left untouched. Every outcome is written to "
                        "operations.log.\n\nEach original WAV is kept in the history "
                        "\xe2\x80\x94 that is your undo."))
                    .withButton("Normalize")
                    .withButton("Cancel"),
                [this, occupied, target, targetText](int button) {
                    if (button == 1)
                        startNormalizeBatch(occupied, target, targetText);
                });
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

void MainComponent::clearSlot(int slot)
{
    const auto* row = slotRowFor(slot);
    clearSlotAction::request(slot, row != nullptr && row->info.hasAudio, recorder,
        [safe = juce::Component::SafePointer<MainComponent>(this), slot](PedalWorker::Job job) {
            if (safe == nullptr)
                return;
            safe->releasePlayerIfHolding(slot, slot); // the clear deletes its WAV (issue #26)
            safe->worker.enqueue(std::move(job));
        });
}

void MainComponent::openSettings()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(makeSettingsDialog().release());
    options.dialogTitle = "Settings";
    options.dialogBackgroundColour = kBackground;
    options.escapeKeyTriggersCloseButton = true;
    options.resizable = false;
    options.launchAsync();
}

std::unique_ptr<SettingsDialog> MainComponent::makeSettingsDialog()
{
    auto dialog = std::make_unique<SettingsDialog>(
        engine.deviceManager(), settings,
        SettingsDialog::Columns {
            settings.file() == nullptr || settings.file()->getBoolValue(kOneShotColumnKey, true),
            settings.file() != nullptr && settings.file()->getBoolValue(kCountInColumnKey, false),
            settings.file() != nullptr && settings.file()->getBoolValue(kLoudnessColumnKey, false) },
        [this](SettingsDialog::Columns columns) {
            if (auto* file = settings.file()) {
                file->setValue(kOneShotColumnKey, columns.oneShot);
                file->setValue(kCountInColumnKey, columns.countIn);
                file->setValue(kLoudnessColumnKey, columns.loudness);
                file->saveIfNeeded();
            }
            applyColumnPreferences();
        },
        SettingsDialog::ImportPrefs {
            settings.file() != nullptr && settings.file()->getBoolValue(kNormalizeOnUploadKey, false),
            settings.file() != nullptr
                ? settings.file()->getDoubleValue(kNormalizeTargetLufsKey, kDefaultTargetLufs)
                : kDefaultTargetLufs },
        [this](SettingsDialog::ImportPrefs prefs) {
            if (auto* file = settings.file()) {
                file->setValue(kNormalizeOnUploadKey, prefs.normalizeOnUpload);
                file->setValue(kNormalizeTargetLufsKey, prefs.targetLufs);
                file->saveIfNeeded();
            }
        });

    // The storage panel (issue #74) is fed by the worker, where the store
    // lives, and reads nothing itself. A SafePointer, because the dialog may
    // be closed while a read or a release is still on the worker's queue.
    juce::Component::SafePointer<HistoryStoragePanel> panel(&dialog->storage());
    dialog->storage().onLimitChanged = [this, panel](std::int64_t bytes) {
        if (auto* file = settings.file()) {
            file->setValue(kHistoryLimitKey, juce::var(static_cast<juce::int64>(bytes)));
            file->saveIfNeeded();
        }
        refreshHistoryStorage(panel); // the forecast and the offer follow the limit
    };
    dialog->storage().onRelease = [this, panel](std::vector<std::string> hashes) {
        releaseHistoryTakes(panel, std::move(hashes));
    };
    refreshHistoryStorage(panel);
    return dialog;
}

std::int64_t MainComponent::historyLimit()
{
    auto* file = settings.file();
    if (file == nullptr)
        return history::retention::kDefaultLimit;
    return file->getValue(kHistoryLimitKey,
                          juce::String(static_cast<juce::int64>(history::retention::kDefaultLimit)))
        .getLargeIntValue();
}

namespace
{
    // WORKER THREAD: the store's numbers, handed to the panel on the message
    // thread — if the dialog is still open by then.
    void deliverHistoryStorage(history::HistoryStore& store, std::int64_t limit,
                               juce::Component::SafePointer<HistoryStoragePanel> panel,
                               const std::shared_ptr<bool>& alive)
    {
        auto read = HistoryStoragePanel::Facts::read(
            store, limit, static_cast<std::int64_t>(juce::Time::currentTimeMillis()));
        juce::MessageManager::callAsync([panel, alive, facts = std::move(read)]() mutable {
            if (*alive && panel != nullptr)
                panel->show(std::move(facts));
        });
    }
} // namespace

// The numbers behind Settings -> History, read on the worker like everything
// that touches the store — and without the card: the store is on this
// computer. Quiet and in the background: the dialog is what shows the
// result, and a toast for a read nobody asked to be told about would only
// cover it.
void MainComponent::refreshHistoryStorage(juce::Component::SafePointer<HistoryStoragePanel> panel)
{
    worker.enqueue({ "Read the history storage",
                     0,
                     [rec = recorder, limit = historyLimit(), panel,
                      alive = uiAlive](const volume::fs::path&) {
                         deliverHistoryStorage(rec->store(), limit, panel, alive);
                     },
                     nullptr,
                     0,
                     true,     // background: nothing to lock for a read
                     true,     // quiet: the panel is the report
                     false }); // and it needs no card
}

// The press behind "Release N takes": the bytes go, the file hands its pages
// back, and the panel reads the store again. In the foreground and out loud:
// the player pressed it and waits, and what came back is worth a line. The
// panel is read again whatever the release did — a release the store refused
// (a take held since the panel last looked) must not leave it saying
// "Releasing", and the refusal's reason reaches the toast on its own.
void MainComponent::releaseHistoryTakes(juce::Component::SafePointer<HistoryStoragePanel> panel,
                                        std::vector<std::string> offered)
{
    auto note = std::make_shared<juce::String>();
    const juce::String takes =
        juce::String(offered.size()) + (offered.size() == 1 ? " take" : " takes");
    PedalWorker::Job job {
        "Release " + takes + " from the history",
        0,
        [rec = recorder, hashes = std::move(offered), note, takes,
         logDir = settings.dataDir()](const volume::fs::path&) {
            auto& store = rec->store();
            const std::int64_t freed = store.releaseBlobs(
                hashes, store.offeredTargets(), static_cast<std::int64_t>(juce::Time::currentTimeMillis()));
            while (store.vacuum(kVacuumSlicePages) > 0) {
            }
            *note = juce::String::fromUTF8(history::retention::bytesText(freed).c_str())
                  + " given back";
            oplog::append(logDir, "history: released " + takes + ", " + *note);
        },
        note,
        0,
        false,  // not background: the player asked for it and waits
        false,  // not quiet: the outcome is the whole point
        false   // and it needs no card
    };
    job.after = [rec = recorder, limit = historyLimit(), panel,
                 alive = uiAlive](const std::string&) {
        deliverHistoryStorage(rec->store(), limit, panel, alive);
    };
    worker.enqueue(std::move(job));
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    // The batch overlay swallows the keyboard the way it swallows clicks —
    // space starting a preview mid-batch is exactly the conflict it exists
    // to prevent (issue #61).
    if (batchOverlay.isVisible())
        return true;
    if (historyKeys(key))
        return true;
    // Standard list ergonomics: select every row — the way to point a check
    // (or a batch) at the whole setlist without a dedicated button.
    if (key == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0) && table.isVisible()) {
        table.selectAll();
        return true;
    }
    if (key == juce::KeyPress::escapeKey && checkId != 0) {
        stopLoudnessCheck();
        return true;
    }
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
    batchOverlay.setBounds(getLocalBounds());
    auto bottom = area.removeFromBottom(kBottomPaneHeight).reduced(12, 8);
    bottomTabs.setBounds(bottom.removeFromTop(26));
    player.setBounds(bottom);
    inspector.setBounds(bottom);
    rhythmPane.setBounds(bottom);
    history.setBounds(bottom);
    area.removeFromBottom(8);
    table.setBounds(area.reduced(12, 0));
    hint.setBounds(area);
}

// --- the History window and Undo / Redo (#73) ---

void MainComponent::openHistoryWindow()
{
    if (historyHost == nullptr)
        historyHost = std::make_unique<HistoryWindowHost>(
            historyView, [this](const juce::KeyPress& key) { return historyKeys(key); });
    historyHost->setVisible(true);
    historyHost->toFront(true);
    feedHistoryWindow();
}

// The whole card's timeline, read on the worker like the slot's tab — no
// card needed — and turned into the window's rows there. The owner keeps what
// the buttons need (the take to play, the slots a restore touches); the view
// keeps the sentences. Every call re-reads the store: a pin the window showed
// optimistically is confirmed or taken back here.
void MainComponent::feedHistoryWindow()
{
    juce::Component::SafePointer<MainComponent> safe(this);
    worker.enqueue({ "Read the card's history",
                     0,
                     [rec = recorder, safe, alive = uiAlive](const volume::fs::path&) {
                         const auto timeline = rec->store().cardTimeline();
                         const auto cardRows = history::rows::forCard(timeline);
                         std::vector<HistoryWindow::Row> rows;
                         std::vector<WindowEntry> entries;
                         const auto today = juce::Time::getCurrentTime();
                         for (std::size_t i = 0; i < cardRows.size() && i < timeline.size(); ++i) {
                             const auto& row = cardRows[i];
                             const juce::Time when(row.at);
                             const bool sameDay = when.getDayOfYear() == today.getDayOfYear()
                                 && when.getYear() == today.getYear();
                             juce::String audio;
                             if (row.takes.size() == 1) {
                                 audio = juce::String(row.takes.front().audio);
                             } else if (row.kind != "snapshot") {
                                 for (const auto& take : row.takes)
                                     if (!take.audio.empty())
                                         audio << (audio.isEmpty() ? "" : juce::String::fromUTF8(" \xc2\xb7 "))
                                               << "slot " << take.slot << ": " << juce::String(take.audio);
                             }
                             std::vector<int> slots = row.slots();
                             rows.push_back({ when.formatted(sameDay ? "%H:%M" : "%d %b %H:%M"),
                                              juce::String(row.action), juce::String(row.detail),
                                              juce::String(row.state), audio,
                                              slots, row.playable(), row.restorable(), row.pinned,
                                              row.op, row.kind == "snapshot", row.restorableSlots() });
                             WindowEntry entry;
                             entry.op = row.op;
                             entry.takeHash = row.takeHash();
                             for (const auto& take : row.takes)
                                 if (take.playable) {
                                     entry.slot = take.slot;
                                     break;
                                 }
                             for (const auto& touched : timeline[i].slots)
                                 if (touched.slot == entry.slot)
                                     entry.takeName = touched.facts.takeName;
                             entry.action = juce::String(row.action);
                             entry.slots = std::move(slots);
                             entry.isSnapshot = row.kind == "snapshot";
                             entry.snapshotSlots = row.restorableSlots();
                             entry.restorable = row.restorable();
                             entries.push_back(std::move(entry));
                         }
                         juce::MessageManager::callAsync(
                             [safe, alive, viewRows = std::move(rows), viewEntries = std::move(entries)]() mutable {
                                 if (!*alive || safe == nullptr)
                                     return;
                                 safe->windowEntries = std::move(viewEntries);
                                 safe->historyView.show(std::move(viewRows));
                                 ++safe->historyWindowFed;
                             });
                     },
                     nullptr,
                     0,
                     true,     // background: reading the history locks nothing
                     true,     // quiet: only a failure is worth saying
                     false }); // and it needs no card
}

const MainComponent::WindowEntry* MainComponent::windowEntry(std::int64_t op) const
{
    for (const auto& entry : windowEntries)
        if (entry.op == op)
            return &entry;
    return nullptr;
}

void MainComponent::playFromWindow(std::int64_t op)
{
    const WindowEntry* entry = windowEntry(op);
    if (entry == nullptr || entry->takeHash.empty())
        return;
    playArchivedTake(entry->slot, entry->takeHash,
                     juce::String(entry->slot) + juce::String::fromUTF8(" \xc2\xb7 ") + entry->action
                         + juce::String::fromUTF8(" \xc2\xb7 from the history"));
}

// "Export take…": the bytes the history keeps, written where the player
// chose. The chooser has asked about a file already there; the export writes
// beside it and renames, so a name that exists is a file that is complete.
void MainComponent::exportFromWindow(std::int64_t op)
{
    const WindowEntry* entry = windowEntry(op);
    if (entry == nullptr || entry->takeHash.empty())
        return;
    const juce::String name = entry->takeName.empty() ? juce::String("take.wav")
                                                       : juce::String(entry->takeName);
    fileChooser = std::make_unique<juce::FileChooser>(
        "Export take", juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile(name),
        "*.wav");
    juce::Component::SafePointer<MainComponent> safe(this);
    fileChooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe, hash = entry->takeHash](const juce::FileChooser& chooser) {
            const juce::File file = chooser.getResult();
            if (safe == nullptr || file == juce::File())
                return;
            safe->worker.enqueue({ "Export take to " + file.getFileName(),
                                   0,
                                   [rec = safe->recorder, hash, path = file.getFullPathName().toStdString()](
                                       const volume::fs::path&) {
                                       history::exportTake(rec->store(), hash, path);
                                   },
                                   nullptr, 0, false, false, false });
        });
}

// "Restore this state": every slot the row touched, back to what that
// operation left in it — one operation, recorded, undoable in turn.
// A first-sighting snapshot restores only the slot the player chooses.
void MainComponent::restoreFromWindow(std::int64_t op, std::optional<int> snapshotSlot)
{
    const WindowEntry* entry = windowEntry(op);
    if (entry == nullptr || !entry->restorable || entry->slots.empty())
        return;
    if (entry->isSnapshot) {
        if (!snapshotSlot) snapshotSlot = historyView.filter();
        if (!snapshotSlot) {
            juce::PopupMenu menu;
            for (const int slot : entry->snapshotSlots)
                menu.addItem(slot, "Restore slot " + juce::String(slot));
            juce::Component::SafePointer<MainComponent> safe(this);
            menu.showMenuAsync(juce::PopupMenu::Options(), [safe, op](int slot) {
                if (safe != nullptr && slot > 0) safe->restoreFromWindow(op, slot);
            });
            return;
        }
        if (std::find(entry->snapshotSlots.begin(), entry->snapshotSlots.end(), *snapshotSlot)
            == entry->snapshotSlots.end()) return;
    }
    const std::vector<int> slots = entry->isSnapshot ? std::vector<int> { *snapshotSlot } : entry->slots;
    for (const int slot : slots)
        releasePlayerIfHolding(slot, slot); // a restore rewrites the slot's audio (issue #26)
    juce::String where;
    if (slots.size() == 1)
        where = "slot " + juce::String(slots[0]);
    else if (slots.size() == 2)
        where = "slots " + juce::String(slots[0]) + " and " + juce::String(slots[1]);
    else
        where = juce::String(static_cast<int>(slots.size())) + " slots";
    const auto options = makeWriteOptions();
    worker.enqueue(recorded("restore", options,
                            { "Restore " + where + " to " + entry->action, slots.size() == 1 ? slots[0] : 0,
                              [rec = recorder, op, options, snapshotSlot](const volume::fs::path& volumePath) {
                                  history::restoreOperation(rec->store(), op, volumePath, options, snapshotSlot);
                              } }));
}

// A pin is the store's to keep: the window showed it at once, and the rows
// read after the write say whether it held — refused or not.
void MainComponent::pinFromWindow(std::int64_t op, bool pinned)
{
    juce::Component::SafePointer<MainComponent> safe(this);
    PedalWorker::Job job { pinned ? "Pin a history row" : "Unpin a history row", 0,
                           [rec = recorder, op, pinned](const volume::fs::path&) {
                               rec->store().pinOp(op, pinned);
                           },
                           nullptr, 0, true, false, false };
    job.after = [safe, alive = uiAlive](const std::string&) {
        juce::MessageManager::callAsync([safe, alive] {
            if (*alive && safe != nullptr)
                safe->feedHistoryWindow();
        });
    };
    worker.enqueue(std::move(job));
}

bool MainComponent::historyKeys(const juce::KeyPress& key)
{
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) {
        pressUndo(true);
        return true;
    }
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) {
        pressUndo(false);
        return true;
    }
    return false;
}

// Undo writes to the card like any operation: a card honestly connected, in
// a model this app writes to, and no job of the player's running.
bool MainComponent::cardTakesEdits() const
{
    return snapshot.state == lifecycle::State::connected && snapshot.error.empty()
        && CardPermissions::of(snapshot.family).anyWrite() && !pedalBusy;
}

void MainComponent::refreshUndoOffer()
{
    juce::Component::SafePointer<MainComponent> safe(this);
    worker.enqueue({ "Read what Undo would put back",
                     0,
                     [rec = recorder, safe, alive = uiAlive](const volume::fs::path&) {
                         const history::undo::Offer offer = history::undo::offer(rec->store());
                         juce::MessageManager::callAsync([safe, alive, offer] {
                             if (!*alive || safe == nullptr)
                                 return;
                             safe->undoOffer = offer;
                             ++safe->undoOfferReads;
                             if (safe->appMenu != nullptr)
                                 safe->appMenu->menuItemsChanged();
                         });
                     },
                     nullptr, 0, true, true, false });
}

juce::String MainComponent::undoMenuText(bool redo) const
{
    return juce::String(history::undo::menuText(redo, undoOffer));
}

bool MainComponent::undoEnabled(bool redo) const
{
    return (redo ? undoOffer.redo : undoOffer.undo).has_value() && cardTakesEdits()
        && !historyEditPending   // one press at a time
        && !clearingHistory;     // and never against history being forgotten
}

// The press: planned on the worker against the store as it is, handed back
// with its words, its refusal or its reasons to ask first.
void MainComponent::pressUndo(bool redo)
{
    if (!undoEnabled(redo))
        return;
    historyEditPending = true;
    const std::int64_t target = *(redo ? undoOffer.redo : undoOffer.undo);
    juce::Component::SafePointer<MainComponent> safe(this);
    worker.enqueue({ redo ? "Plan the redo" : "Plan the undo",
                     0,
                     [rec = recorder, redo, target, volume = snapshot.volume, safe,
                      alive = uiAlive](const volume::fs::path&) {
                         history::HistoryStore& store = rec->store();
                         const auto timeline = store.cardTimeline();
                         const history::undo::Offer offer = history::undo::offerFrom(store.offeredTargets(), timeline);
                         HistoryEdit edit;
                         edit.redo = redo;
                         edit.target = target;
                         edit.words = juce::String(history::undo::menuText(redo, offer));
                         const auto offered = redo ? offer.redo : offer.undo;
                         const history::undo::Plan plan = history::undo::plan(timeline, target);
                         if (!offered || *offered != target)
                             edit.refusal = "the history moved on since the press";
                         else if (!plan.possible())
                             edit.refusal = juce::String(plan.reason);
                         if (plan.swapBack) {
                             edit.slots.push_back(plan.swapBack->first);
                             edit.slots.push_back(plan.swapBack->second);
                         }
                         for (const auto& step : plan.steps)
                             edit.slots.push_back(step.slot);
                         // Across a connection means: from another session than the
                         // one this run records in — none yet counts as another.
                         const history::undo::Bump bump = history::undo::bumpFor(
                             plan, timeline, rec->sessionOn(volume::fs::path(volume)));
                         edit.crossings = bump.keys;
                         edit.reasons = bump.reasons;
                         juce::MessageManager::callAsync([safe, alive, edit] {
                             if (*alive && safe != nullptr)
                                 safe->proposeHistoryEdit(edit);
                         });
                     },
                     nullptr, 0, true, true, false,
                     nullptr,
                     [safe, alive = uiAlive](const std::string& error) {
                         // A plan that could not be read at all: the press is over.
                         if (!error.empty())
                             juce::MessageManager::callAsync([safe, alive, error] {
                                 if (*alive && safe != nullptr)
                                     safe->settleHistoryEdit(juce::String::fromUTF8(error.c_str()));
                             });
                     } });
}

// Alisa's rule for the speed bump: crossing a connection, a change made on the
// pedal, or a later change still in effect is allowed, and warned about the
// first time; the same crossing is not asked about again in this session.
void MainComponent::proposeHistoryEdit(HistoryEdit edit)
{
    const juce::String verb = edit.redo ? "Redo" : "Undo";
    if (edit.refusal.isNotEmpty()) {
        toast.show(verb + juce::String::fromUTF8(" \xe2\x80\x94 ") + edit.refusal);
        settleHistoryEdit(verb + ": " + edit.refusal);
        return;
    }
    const bool askedBefore = std::all_of(edit.crossings.begin(), edit.crossings.end(),
                                         [this](const std::string& key) {
                                             return acknowledgedCrossings.count(key) > 0;
                                         });
    if (askedBefore) {
        runHistoryEdit(std::move(edit));
        return;
    }
    juce::String message;
    for (const auto& reason : edit.reasons)
        message << juce::String(reason) << "\n";
    message << "\n" << verb
            << " is recorded like any other change and can be undone in turn. LooperCat asks about"
               " this once.";
    juce::Component::SafePointer<MainComponent> safe(this);
    askFirst(edit.words + "?", message, verb + " anyway", [safe, edit](bool yes) {
        if (safe == nullptr)
            return;
        if (!yes) {
            safe->settleHistoryEdit((edit.redo ? "Redo" : "Undo") + juce::String(": cancelled"));
            return;
        }
        for (const auto& key : edit.crossings)
            safe->acknowledgedCrossings.insert(key);
        safe->runHistoryEdit(edit);
    });
}

// One recorded operation: opened and checked in the job's `before` (a press
// the history has moved past begins nothing), carried out in `work` through
// the core's primitives, closed in `after` with the words of what it undid.
void MainComponent::runHistoryEdit(HistoryEdit edit)
{
    if (!cardTakesEdits()) { // the dialog stood open while the pedal went away or got busy
        settleHistoryEdit((edit.redo ? "Redo" : "Undo") + juce::String(": the pedal is not ready"));
        return;
    }
    for (const int slot : edit.slots)
        releasePlayerIfHolding(slot, slot);
    const auto options = makeWriteOptions();
    auto checked = std::make_shared<history::undo::Checked>();
    historyEditDescription = edit.words;
    PedalWorker::Job job { edit.words, edit.slots.size() == 1 ? edit.slots[0] : 0,
                           [rec = recorder, checked, options](const volume::fs::path& volumePath) {
                               history::undo::apply(rec->store(), checked->plan, volumePath, options);
                           } };
    job.before = [rec = recorder, id = options.opId, redo = edit.redo, target = edit.target,
                  checked](const volume::fs::path& volumePath) {
        *checked = history::undo::beginPress(*rec, id, redo, target, volumePath);
    };
    job.after = [rec = recorder, id = options.opId, checked](const std::string& error) {
        rec->finish(id, error, checked->note);
    };
    worker.enqueue(std::move(job));
}

void MainComponent::settleHistoryEdit(juce::String outcome)
{
    historyEditPending = false;
    historyEditDescription.clear();
    historyEditOutcome = std::move(outcome);
    ++historyEditsDone;
    if (appMenu != nullptr)
        appMenu->menuItemsChanged();
}

} // namespace loopercat
