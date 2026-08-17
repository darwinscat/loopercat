// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <fstream>
#include <iostream>

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
            || args.contains("--cycle");
    }

    void initialise(const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();

        // --volume <path>: pin the pedal volume instead of autodetecting —
        // rc5cat parity, and the hook that lets a synthetic pedal directory
        // drive the app in tests.
        const int volumeFlag = args.indexOf("--volume");
        const juce::String explicitVolume = volumeFlag >= 0 ? args[volumeFlag + 1] : juce::String();

        // --snapshot <file.png>: render the main content offscreen and exit.
        // The headless proof that the window actually draws — no display
        // permissions involved; used by the DoD check and CI screenshots.
        // --select <slot> additionally selects that slot (1..99) and waits
        // for its waveform before rendering; --properties switches the bottom
        // pane to its Properties tab, so that side renders headless too.
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
            // "--midi-probe exit" sends the leaving frame instead, so the
            // Disconnect half of the same path can be measured too.
            const int probeFlag = args.indexOf("--midi-probe");
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
        if (args.contains("--cycle")) {
            setApplicationReturnValue(runCycle(explicitVolume));
            quit();
            return;
        }

        const int snapshotFlag = args.indexOf("--snapshot");
        if (snapshotFlag >= 0) {
            const int selectFlag = args.indexOf("--select");
            const int slot = selectFlag >= 0 ? args[selectFlag + 1].getIntValue() : 0;
            setApplicationReturnValue(
                writeSnapshot(args[snapshotFlag + 1], explicitVolume.toStdString(), slot));
            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow>(getApplicationName(),
                                                  explicitVolume.toStdString());
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

    static int runCycle(const juce::String& explicitVolume)
    {
        MainComponent content(explicitVolume.toStdString());
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

    static int writeSnapshot(const juce::String& path, const std::string& explicitVolume,
                             const int selectSlot)
    {
        if (path.isEmpty()) {
            std::cerr << "--snapshot requires a target file path\n";
            return 2;
        }
        MainComponent content(explicitVolume);
        content.refreshNow();
        if (juce::JUCEApplicationBase::getCommandLineParameterArray().contains("--properties"))
            content.showProperties();
        if (selectSlot > 0) {
            content.selectSlot(selectSlot);
            const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
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
        const juce::Image image =
            content.createComponentSnapshot(content.getLocalBounds(), false, 1.0f);
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

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name, std::string explicitVolume)
            : DocumentWindow(name, juce::Colour(0xff121218), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            auto* main = new MainComponent(std::move(explicitVolume));
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
