// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "AudioEngine.h"

#include <felitronics/appkit/Brand.h>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::PlayerPane — the listening strip for the selected slot: transport
// row (play/stop, loop toggle, title, time readout, audio-settings gear) over
// the waveform with a playhead; click/drag the waveform to seek. The pane
// drives the AudioEngine and draws its state; it owns no audio objects.
//==============================================================================
namespace loopercat
{

class PlayerPane final : public juce::Component,
                         private juce::Timer,
                         private juce::ChangeListener
{
public:
    explicit PlayerPane(AudioEngine& engine);
    ~PlayerPane() override;

    // Load a slot for listening. `oneShot` presets the loop toggle to the
    // slot's own on-pedal behavior. A load failure shows in the pane.
    void setSlot(const juce::File& wav, const juce::String& title, bool oneShot);
    void clear();

    const juce::String& currentPath() const { return currentPath_; }
    bool isThumbnailReady() const { return thumbnail_.isFullyLoaded(); }

    std::function<void()> onGear; // the owner opens the audio-settings dialog

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }
    void seekTo(juce::Point<float> position);
    juce::Rectangle<int> waveArea() const;
    void updateTransportRow();

    AudioEngine& engine_;
    juce::AudioThumbnailCache thumbnailCache_ { 8 };
    juce::AudioThumbnail thumbnail_ { 512, engine_.formats(), thumbnailCache_ };

    juce::TextButton playButton_;
    juce::ToggleButton loopButton_ { "Loop" };
    felitronics::appkit::brand::GearButton gearButton_;
    juce::String title_, error_, currentPath_;
    bool lastPaintedPlaying_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayerPane)
};

} // namespace loopercat
