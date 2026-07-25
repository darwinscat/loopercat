// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"
#include "MemorySettingsPanel.h"

#include <loopercat/Commands.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

namespace loopercat
{

class LooperCatApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "LooperCat"; }
    const juce::String getApplicationVersion() override { return LOOPERCAT_VERSION; }
    bool moreThanOneInstanceAllowed() override { return false; }

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
        // for its waveform before rendering.
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
    void systemRequestedQuit() override { quit(); }

private:
    static int writePng(const juce::Image& image, const juce::String& path)
    {
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
                             const int selectSlot)
    {
        if (path.isEmpty()) {
            std::cerr << "--snapshot requires a target file path\n";
            return 2;
        }

        // --settings <slot>: render the Memory Settings panel for that slot
        // instead of the main window — the editor's own headless proof.
        {
            const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
            const int settingsFlag = args.indexOf("--settings");
            if (settingsFlag >= 0) {
                if (explicitVolume.empty()) {
                    std::cerr << "--settings requires --volume\n";
                    return 2;
                }
                try {
                    const std::string text = commands::readMemory(explicitVolume);
                    const int slot = args[settingsFlag + 1].getIntValue();
                    MemorySettingsPanel panel(slot,
                                              memsettings::read(rc0::slotBody(text, slot)));
                    return writePng(
                        panel.createComponentSnapshot(panel.getLocalBounds(), false, 1.0f), path);
                } catch (const std::exception& e) {
                    std::cerr << e.what() << "\n";
                    return 2;
                }
            }
        }

        MainComponent content(explicitVolume);
        content.refreshNow();
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
        return writePng(content.createComponentSnapshot(content.getLocalBounds(), false, 1.0f),
                        path);
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name, std::string explicitVolume)
            : DocumentWindow(name, juce::Colour(0xff121218), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(std::move(explicitVolume)), true);
            setResizable(true, true);
            setResizeLimits(760, 480, 4096, 4096);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace loopercat

START_JUCE_APPLICATION(loopercat::LooperCatApplication)
