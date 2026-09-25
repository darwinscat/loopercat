// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"
#include "history/LegacyImport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>

namespace loopercat
{

class LooperCatApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "LooperCat"; }
    const juce::String getApplicationVersion() override { return LOOPERCAT_VERSION; }
    // One window per user — except the headless seams: a CI or verification
    // run must do its job and exit even while a windowed instance is open,
    // not silently forward its arguments to that window. Every headless flag
    // has to be listed here, and the failure mode when one is forgotten is
    // the quietest kind: the process exits 0 having printed nothing and done
    // nothing, which reads exactly like a feature that does not work.
    bool moreThanOneInstanceAllowed() override
    {
        const auto args = getCommandLineParameters();
        return args.contains("--snapshot") || args.contains("--midi-probe")
            || args.contains("--cycle") || args.contains("--import-legacy")
            || args.contains("--history-storage");
    }

    void initialise(const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();

        // --volume <path>: pin the pedal volume instead of autodetecting —
        // rc5cat parity, and the hook that lets a synthetic pedal directory
        // drive the app in tests.
        const int volumeFlag = args.indexOf("--volume");
        const juce::String explicitVolume = volumeFlag >= 0 ? args[volumeFlag + 1] : juce::String();

        // --data <dir>: the app's whole data home (settings, history, trash,
        // backups) in another directory — the partner of --volume, so a run
        // against a synthetic pedal writes nothing into the player's own data.
        const int dataFlag = args.indexOf("--data");
        const juce::File dataOverride = dataFlag >= 0
            ? juce::File::getCurrentWorkingDirectory().getChildFile(args[dataFlag + 1])
            : juce::File();

        // --snapshot <file.png>: render the main content offscreen and exit.
        // The headless proof that the window actually draws — no display
        // permissions involved; used by the DoD check and CI screenshots.
        // --select <slot> additionally selects that slot (1..99) and waits
        // for its waveform before rendering; --properties and --history
        // switch the bottom pane to those tabs, so those faces render
        // headless too.
        // --midi-probe: list the MIDI outputs the app can see, send the
        // storage-mode frame through the very same path Connect uses, and
        // report what happened. A diagnostic seam in the spirit of
        // --snapshot: on Linux the Connect button reported a clean send
        // while the pedal never moved, and telling "found nothing" apart
        // from "sent into the void" needs the send without the window.
        if (args.contains("--midi-probe")) {
            for (const auto& device : juce::MidiOutput::getAvailableDevices())
                std::cout << "midi out: [" << device.name << "] id=[" << device.identifier
                          << "]\n";
            const auto found = pedallink::findPedal();
            std::cout << "found: " << (found ? found->name : juce::String("NOTHING")) << "\n";
            const int probeFlag = args.indexOf("--midi-probe");
            // "--midi-probe read <identifier>" asks that endpoint for its
            // storage register and sends nothing else (issue #85): the seam
            // behind "ask the pedal why" — the pedal's own word, or the
            // honest silence, without the window.
            if (probeFlag >= 0 && args[probeFlag + 1] == "read") {
                const juce::String wanted = args[probeFlag + 2];
                std::optional<juce::MidiDeviceInfo> pedal;
                for (const auto& candidate : pedallink::findPedals())
                    if (candidate.identifier == wanted)
                        pedal = candidate;
                if (!pedal) {
                    std::cout << "register: no RC-5 output with identifier [" << wanted << "]" << std::endl;
                    setApplicationReturnValue(1);
                } else {
                    try {
                        const auto state = pedallink::readStorageState(*pedal);
                        std::cout << "register: " << pedallink::describe(state) << std::endl;
                        setApplicationReturnValue(0);
                    } catch (const loopercat::Error& e) {
                        std::cout << "register: error: " << e.what() << std::endl;
                        setApplicationReturnValue(1);
                    }
                }
                quit();
                return;
            }
            // "--midi-probe exit" sends the leaving frame instead, so the
            // Disconnect half of the same path can be measured too.
            const bool enter = !(probeFlag >= 0 && args[probeFlag + 1] == "exit");
            const juce::String error = pedallink::requestStorageMode(enter);
            std::cout << "send: " << (error.isEmpty() ? juce::String("reported ok") : error)
                      << std::endl;
            setApplicationReturnValue(error.isEmpty() ? 0 : 1);
            quit();
            return;
        }

        // --cycle: press Connect, wait for the volume, press Disconnect, and
        // narrate everything the window would have shown. Built because the
        // interesting failures on Linux are TRANSIENT — banners that flash for
        // a few hundred milliseconds during a disconnect are unreadable on
        // screen and impossible to screenshot reliably, but they are the only
        // record of what went wrong.
        // --import-legacy: the Maintenance item without the window. Records
        // the backups/ and trash/ folders under the data home into the
        // history and prints the run's sentence, every skipped folder with
        // its reason before it. For a verification run against a copied
        // home (--data), and for the DoD check of #72's last stage.
        if (args.contains("--import-legacy")) {
            setApplicationReturnValue(runLegacyImport(dataOverride));
            quit();
            return;
        }

        if (args.contains("--cycle")) {
            setApplicationReturnValue(runCycle(explicitVolume, dataOverride));
            quit();
            return;
        }

        // --history-storage [release | limit <gb>]: Settings -> History
        // without the window. Builds the very dialog the gear opens, waits
        // for the store's numbers to reach its panel through the worker, and
        // prints what the panel says. With `release` it then lowers "keep at
        // most" to nothing, presses the button, waits for the panel to be
        // read again and prints it again — the end-to-end proof that the
        // button frees space and the panel tells the truth afterwards. With
        // `limit <gb>` it types that limit into the field and waits for the
        // owner to come back with it; a plain run afterwards shows whether
        // the limit survived the process. With --snapshot <file.png> the
        // dialog is rendered too. For a verification run against a copied
        // home (--data).
        if (args.contains("--history-storage")) {
            setApplicationReturnValue(runHistoryStorage(args, explicitVolume, dataOverride));
            quit();
            return;
        }

        const int snapshotFlag = args.indexOf("--snapshot");
        if (snapshotFlag >= 0) {
            const int selectFlag = args.indexOf("--select");
            const int slot = selectFlag >= 0 ? args[selectFlag + 1].getIntValue() : 0;
            setApplicationReturnValue(
                writeSnapshot(args[snapshotFlag + 1], explicitVolume.toStdString(), slot,
                              dataOverride));
            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow>(getApplicationName(),
                                                  explicitVolume.toStdString(), dataOverride);
    }

    void shutdown() override { mainWindow = nullptr; }

    // Quit is Disconnect (issue #1): while the app holds the pedal's volume,
    // release it and walk the pedal out of STORAGE first. MainComponent
    // bounds the wait, so a busy or wedged volume can never hold the exit
    // hostage. The close button, Cmd-Q and system-initiated quit (logout,
    // shutdown) all funnel through here.
    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr && mainWindow->beginQuitDisconnect([] {
                juce::JUCEApplication::getInstance()->quit();
            }))
            return; // the release (or its time bound) resumes the quit
        quit();
    }

private:
    // Pump the message loop for `ms`, printing any banner line that appeared
    // since the last look. Returns the current lifecycle state name.
    static std::string pumpAndReport(MainComponent& content, int ms,
                                     std::vector<std::string>& seen, std::string& lastState)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
        for (const std::string& line : content.bannerLines()) {
            if (std::find(seen.begin(), seen.end(), line) == seen.end()) {
                seen.push_back(line);
                std::cout << "  banner: " << line << std::endl;
            }
        }
        const std::string state = content.lifecycleStateName();
        if (state != lastState) {
            std::cout << "  state: " << lastState << " -> " << state << std::endl;
            lastState = state;
        }
        return state;
    }

    static int runLegacyImport(const juce::File& dataOverride)
    {
        try {
            AppSettings settings(dataOverride);
            const juce::File home = settings.dataDir();
            history::HistoryStore store(std::filesystem::path(
                home.getChildFile("history").getFullPathName().toStdString()));
            const auto report = history::legacy::importFolders(
                store, std::filesystem::path(home.getFullPathName().toStdString()),
                static_cast<std::int64_t>(juce::Time::currentTimeMillis()));
            for (const auto& skipped : report.skipped)
                std::cout << "skipped " << skipped.path << ": " << skipped.reason << "\n";
            std::cout << history::legacy::describe(report) << std::endl;
            return 0;
        } catch (const std::exception& e) {
            std::cout << "import failed: " << e.what() << std::endl;
            return 1;
        }
    }

    static int runCycle(const juce::String& explicitVolume, const juce::File& dataOverride)
    {
        MainComponent content(explicitVolume.toStdString(), dataOverride);
        content.refreshNow();

        std::vector<std::string> seen;
        std::string lastState = content.lifecycleStateName();
        std::cout << "start: state=" << lastState << " volume=[" << content.volumePath() << "]"
                  << std::endl;

        std::cout << "--- Connect ---" << std::endl;
        content.beginConnect();
        for (int i = 0; i < 120 && content.volumePath().empty(); ++i)
            pumpAndReport(content, 250, seen, lastState);

        const bool mounted = !content.volumePath().empty();
        std::cout << "after Connect: mounted=" << (mounted ? "yes" : "NO") << " volume=["
                  << content.volumePath() << "] state=" << content.lifecycleStateName()
                  << std::endl;
        if (!mounted)
            return 1;

        // Optionally play a slot first. This is not decoration: the reported
        // transient banners appeared after LISTENING, and playback is what
        // leaves the read-ahead thread holding a file on the card when the
        // unmount asks for it.
        const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
        const int playFlag = args.indexOf("--play");
        if (playFlag >= 0) {
            const int slot = args[playFlag + 1].getIntValue();
            std::cout << "--- playing slot " << slot << " ---" << std::endl;
            content.playSlot(slot);
            for (int i = 0; i < 24; ++i)
                pumpAndReport(content, 250, seen, lastState);
        } else {
            for (int i = 0; i < 8; ++i)
                pumpAndReport(content, 250, seen, lastState);
        }

        std::cout << "--- Disconnect ---" << std::endl;
        content.beginDisconnect();
        for (int i = 0; i < 120; ++i) {
            pumpAndReport(content, 250, seen, lastState);
            if (content.volumePath().empty() && lastState != "connected")
                break;
        }
        std::cout << "after Disconnect: volume=[" << content.volumePath()
                  << "] state=" << content.lifecycleStateName() << std::endl;

        // A few more turns: the banners worth catching are the ones that
        // appear AFTER the volume is gone.
        for (int i = 0; i < 12; ++i)
            pumpAndReport(content, 250, seen, lastState);

        std::cout << "banners seen in total: " << seen.size() << std::endl;
        return content.volumePath().empty() ? 0 : 2;
    }

    static int runHistoryStorage(const juce::StringArray& args, const juce::String& explicitVolume,
                                 const juce::File& dataOverride)
    {
        MainComponent content(explicitVolume.toStdString(), dataOverride);
        content.refreshNow();
        const auto dialog = content.makeSettingsDialog();
        dialog->showHistoryStorage();
        HistoryStoragePanel& panel = dialog->storage();
        std::cout << "panel visible: " << (panel.isVisible() ? "yes" : "no") << std::endl;

        const auto say = [&panel](const char* when) {
            std::cout << when << ":\n"
                      << "  cost: " << panel.costLine() << "\n"
                      << "  disk: " << panel.diskLine() << "\n"
                      << "  forecast: " << panel.forecastLine() << "\n"
                      << "  limit: " << panel.limitText() << " GB\n"
                      << "  offer: " << panel.offerLine() << "\n"
                      << "  button: " << panel.releaseButtonText()
                      << (panel.releaseEnabled() ? "" : " (disabled)") << "\n"
                      << "  rows: " << panel.offeredRows() << std::endl;
        };
        const auto waitUntil = [](auto done, int ms) {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + ms;
            while (!done() && juce::Time::getMillisecondCounterHiRes() < deadline)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
            return done();
        };

        if (!waitUntil([&panel] { return !panel.costLine().contains("not been read"); }, 15000)) {
            std::cerr << "the panel never received the store's numbers\n";
            return 2;
        }
        say("read");

        const int flag = args.indexOf("--history-storage");
        if (flag >= 0 && args[flag + 1] == "release") {
            panel.setKeepTarget(0);
            std::cout << "pressing: " << panel.releaseButtonText() << " (" << panel.offeredRows()
                      << " rows)" << std::endl;
            panel.confirmRelease();
            // The panel says "Releasing" until the worker has read the store
            // again — a release of gigabytes vacuums for a while.
            if (!waitUntil([&panel] { return !panel.releaseButtonText().contains("Releasing"); },
                           120000)) {
                std::cerr << "the panel was not read again after the release\n";
                return 2;
            }
            say("after release");
        }
        if (flag >= 0 && args[flag + 1] == "limit") {
            const int shown = panel.factsShown();
            panel.commitLimitText(args[flag + 2]);
            std::cout << "typed limit: " << args[flag + 2] << " -> field " << panel.limitText()
                      << " GB" << std::endl;
            if (!waitUntil([&panel, shown] { return panel.factsShown() > shown; }, 15000)) {
                std::cerr << "the panel was not read again after the limit changed\n";
                return 2;
            }
            say("after limit");
        }

        const int snapshotFlag = args.indexOf("--snapshot");
        return snapshotFlag >= 0 ? writePng(*dialog, args[snapshotFlag + 1]) : 0;
    }

    // Renders a face offscreen into a PNG — the headless proof that it
    // actually draws. 0 when written, 2 when the file cannot be.
    static int writePng(juce::Component& face, const juce::String& path)
    {
        if (path.isEmpty()) {
            std::cerr << "--snapshot requires a target file path\n";
            return 2;
        }
        const juce::Image image = face.createComponentSnapshot(face.getLocalBounds(), false, 1.0f);
        const juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile(path);
        file.deleteFile();
        juce::FileOutputStream out(file);
        if (out.failedToOpen() || !juce::PNGImageFormat().writeImageToStream(image, out)) {
            std::cerr << "cannot write snapshot to " << file.getFullPathName() << "\n";
            return 2;
        }
        std::cout << "snapshot: " << file.getFullPathName() << "\n";
        return 0;
    }

    static int writeSnapshot(const juce::String& path, const std::string& explicitVolume,
                             const int selectSlot, const juce::File& dataOverride)
    {
        MainComponent content(explicitVolume, dataOverride);
        content.refreshNow();
        if (juce::JUCEApplicationBase::getCommandLineParameterArray().contains("--properties"))
            content.showProperties();
        const bool wantsHistory =
            juce::JUCEApplicationBase::getCommandLineParameterArray().contains("--history");
        if (wantsHistory)
            content.showHistory();
        if (juce::JUCEApplicationBase::getCommandLineParameterArray().contains("--about")) {
            // The About popover parents into the top-level component — here that
            // is `content` itself, so the callout lands inside the snapshot. No
            // event-loop pump after this: headless the process is not foreground,
            // and the box's own modal machinery would dismiss it on first tick.
            content.showAbout();
        }
        if (selectSlot > 0) {
            content.selectSlot(selectSlot);
            const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
            // "--push <file>": push into the selected slot through the real
            // worker/rescan chain, then fall into the playerReady wait below —
            // which IS the check that the player picked the new file up
            // (the missed-restore bug this seam exists to falsify).
            const int pushFlag = args.indexOf("--push");
            if (pushFlag >= 0) {
                content.pushWav(selectSlot, args[pushFlag + 1], false);
                // The verdict this seam exists for: the worker converts and
                // pushes, the rescan lands, and the player must pick the new
                // file up ON ITS OWN — no click-away-and-back.
                const auto pushDeadline = juce::Time::getMillisecondCounterHiRes() + 30000;
                while (!content.listeningTo(selectSlot)
                       && juce::Time::getMillisecondCounterHiRes() < pushDeadline)
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
                if (!content.listeningTo(selectSlot)) {
                    std::cerr << "player did not pick up the pushed file\n";
                    return 2;
                }
            }
            const int markersFlag = args.indexOf("--markers");
            if (markersFlag >= 0) { // "--markers <in>:<out>" in seconds
                const juce::String spec = args[markersFlag + 1];
                content.setMarkers(spec.upToFirstOccurrenceOf(":", false, false).getDoubleValue(),
                                   spec.fromFirstOccurrenceOf(":", false, false).getDoubleValue());
            }
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
            while (!content.playerReady() && juce::Time::getMillisecondCounterHiRes() < deadline)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
            if (!content.playerReady()) {
                std::cerr << "waveform did not finish loading in time\n";
                return 2;
            }
        }
        if (wantsHistory) {
            // The tab reads its rows on the worker, so the render has to wait
            // for them the way it waits for a waveform — an empty slot has no
            // waveform to wait behind, and the shot would catch the tab blank.
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
            while (!content.historyReady() && juce::Time::getMillisecondCounterHiRes() < deadline)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        }
        return writePng(content, path);
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name, std::string explicitVolume, juce::File dataOverride)
            : DocumentWindow(name, juce::Colour(0xff121218), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            auto* main = new MainComponent(std::move(explicitVolume), std::move(dataOverride));
            content = main;
            setContentOwned(main, true);
            setResizable(true, true);
            setResizeLimits(760, 480, 4096, 4096);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        bool beginQuitDisconnect(std::function<void()> done)
        {
            return content->beginQuitDisconnect(std::move(done));
        }

    private:
        MainComponent* content = nullptr; // owned via setContentOwned

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace loopercat

START_JUCE_APPLICATION(loopercat::LooperCatApplication)
