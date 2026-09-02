// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "AudioEngine.h"

#include <loopercat/Normalize.hpp>

#include <felitronics/appkit/Brand.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

//==============================================================================
// loopercat::PlayerPane — the listening strip for the selected slot: transport
// row (play/stop, loop toggle, title, selection readout, time) over the
// waveform with a playhead and trim markers. Drag the in/out flags to choose
// a section — it previews as a gapless loop immediately; Trim rewrites the
// slot to exactly that range (the owner runs the command). The pane drives
// the AudioEngine and draws its state; it owns no audio objects.
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
    // slot's own on-pedal behavior; `frames` is the slot's WavLen (frame
    // trim math runs on it). A load failure shows in the pane.
    void setSlot(int slot, const juce::File& wav, const juce::String& title, bool oneShot,
                 long long frames);
    void clear();

    // Re-open the current file after it changed on disk (a trim landed):
    // fresh reader, fresh thumbnail, markers reset.
    void reload();

    // Programmatic markers (the --markers snapshot seam; clamped, in seconds).
    void setMarkers(double inSeconds, double outSeconds);

    const juce::String& currentPath() const { return currentPath_; }
    // True once the read pass has fed the whole file into the waveform.
    bool isThumbnailReady() const
    {
        return !readPass_.isThreadRunning() && thumbnail_.isFullyLoaded();
    }

    std::function<void(double)> onVolumeChanged;                     // preview volume moved (0..100)

    // Set the preview volume (0..100) — the fader follows and the gain applies.
    void setVolume(double percent);

    int currentSlot() const { return slot_; }

    // The one-line truth about tempo (issue #29): the pedal time-stretches
    // when a slot's Tempo differs from its RecTmp, the preview plays the
    // recording as-is. Empty hides the note.
    void setTempoNote(const juce::String& note);

    // Close every handle on the loaded WAV (reader, read-ahead, thumbnail
    // source) but keep the pane's state, so reload() can bring it back.
    void releaseFile();
    std::function<void(int, juce::int64, juce::int64)> onTrim;       // (slot, inFrame, outFrame)

    // Loudness of the loaded loop (issue #61). The pane reads every file it
    // loads in ONE background pass that feeds both the waveform and a
    // BS.1770 meter, and hands the reading up through onLoudnessRead the
    // moment the waveform is complete — no second read off the card, no
    // button. The owner turns it into words (target, damage) and feeds them
    // back through setLoudness; a read-only check running elsewhere lands
    // the same way. The readout and Normalize… share the row with Trim and
    // step aside while a trim selection is active: the selection owns the
    // row then, and Normalize is whole-loop work that must not read as "the
    // selection". All setters ignore a slot that is not the loaded one.
    std::function<void(int, const wav::LoudnessReading&)> onLoudnessRead;
    void setLoudness(int slot, const juce::String& text, bool attention, bool damaged,
                     const juce::String& tooltip);
    void setLoudnessPending(int slot);
    void clearLoudness(int slot = 0); // 0 = whatever is loaded
    std::function<void(int)> onNormalize; // Normalize… pressed for this slot

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;

private:
    enum class Drag { none, seek, inMarker, outMarker };

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }
    void applyFile(const juce::File& wav);
    void seekTo(juce::Point<float> position);
    void markersChanged();
    bool markersActive() const;
    juce::int64 frameAt(double seconds) const;
    float xOf(double seconds) const;
    double secondsAt(float x) const;
    juce::Rectangle<int> waveArea() const;
    void updateTransportRow();
    void updateLoudnessButtons();
    void layoutReadout();
    void passFinished(int slot, std::optional<wav::LoudnessReading> reading); // message thread

    // The one read pass over a loaded file: waveform blocks into the
    // thumbnail, samples into the meter, then the reading up to the pane. A
    // new load or a clear stops it between blocks — browsing fast through
    // slots must not queue a card's worth of reads.
    class ReadPass final : public juce::Thread
    {
    public:
        explicit ReadPass(PlayerPane& owner)
            : juce::Thread("LooperCat player read pass"), owner_(owner) {}
        void start(const juce::File& file, int slot);
        void stop() { stopThread(5000); }
        void run() override;

    private:
        PlayerPane& owner_;
        juce::File file_;
        int slot_ = 0;
    };

    // The readout after the title: a line of text, or a warning sign with a
    // word — the row is tight, so the tooltip carries what the row cannot.
    class LoudnessReadout final : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        void set(const juce::String& text, bool attention, bool warning, const juce::String& tip);
        void paint(juce::Graphics& g) override;

    private:
        juce::String text_;
        bool attention_ = false, warning_ = false;
    };

    AudioEngine& engine_;
    juce::AudioThumbnailCache thumbnailCache_ { 8 };
    juce::AudioThumbnail thumbnail_ { 512, engine_.formats(), thumbnailCache_ };

    juce::TextButton playButton_;
    juce::ToggleButton loopButton_ { "Loop" };
    juce::Slider volumeSlider_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Rectangle<int> volumeIconArea_; // the speaker glyph, drawn in paint()
    juce::TextButton trimButton_ { "Trim" };
    juce::TextButton resetButton_ { "Reset" };
    juce::TextButton normalizeButton_ { juce::String::fromUTF8("Normalize\xe2\x80\xa6") };
    juce::String title_, error_, currentPath_, tempoNote_;
    LoudnessReadout readout_;
    bool loudnessDamaged_ = false, loudnessPending_ = false;
    int slot_ = 0;
    long long slotFrames_ = 0;
    bool oneShot_ = false;
    double inSeconds_ = 0.0, outSeconds_ = 0.0;
    Drag drag_ = Drag::none;
    bool lastPaintedPlaying_ = false;
    ReadPass readPass_ { *this }; // last: it touches thumbnail_ and engine_, so it dies first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayerPane)
};

} // namespace loopercat
