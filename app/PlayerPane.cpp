// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PlayerPane.h"

#include <loopercat/Params.hpp>
#include <loopercat/Wav.hpp>

#include <cmath>

namespace loopercat
{

namespace
{
    const juce::Colour kPaneBackground { 0xff0e0e13 };
    const juce::Colour kText { 0xffd8d8d8 };
    const juce::Colour kDim { 0xff63636d };
    constexpr int kTransportRowHeight = 30;
    constexpr float kMarkerGrabZone = 7.0f;
    constexpr double kMinSectionSeconds = 0.1;

    juce::String formatSeconds(double seconds)
    {
        const auto whole = static_cast<long long>(seconds);
        return juce::String(whole / 60) + ":" + juce::String(whole % 60).paddedLeft('0', 2);
    }
} // namespace

PlayerPane::PlayerPane(AudioEngine& engine) : engine_(engine)
{
    playButton_.setButtonText(juce::String::fromUTF8("\xe2\x96\xb6")); // play triangle
    playButton_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1e1e26));
    playButton_.setEnabled(false);
    playButton_.onClick = [this] {
        engine_.togglePlay();
        updateTransportRow();
    };

    loopButton_.setColour(juce::ToggleButton::textColourId, kDim);
    loopButton_.setColour(juce::ToggleButton::tickColourId, felitronics::appkit::brand::violet);
    loopButton_.onClick = [this] { engine_.setLooping(loopButton_.getToggleState()); };

    volumeSlider_.setRange(0.0, 100.0, 1.0);
    volumeSlider_.setValue(100.0, juce::dontSendNotification);
    volumeSlider_.setColour(juce::Slider::trackColourId, felitronics::appkit::brand::violet);
    volumeSlider_.setColour(juce::Slider::thumbColourId, felitronics::appkit::brand::lilac);
    volumeSlider_.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff1e1e26));
    volumeSlider_.onValueChange = [this] {
        engine_.setGain(static_cast<float>(volumeSlider_.getValue() / 100.0));
        if (onVolumeChanged)
            onVolumeChanged(volumeSlider_.getValue());
    };

    trimButton_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a2440));
    trimButton_.setColour(juce::TextButton::textColourOffId, felitronics::appkit::brand::lilac);
    trimButton_.onClick = [this] {
        if (onTrim && markersActive())
            onTrim(slot_, frameAt(inSeconds_), frameAt(outSeconds_));
    };

    resetButton_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1e1e26));
    resetButton_.onClick = [this] {
        inSeconds_ = 0.0;
        outSeconds_ = engine_.lengthSeconds();
        markersChanged();
    };

    gearButton_.onClick = [this] {
        if (onGear)
            onGear();
    };

    thumbnail_.addChangeListener(this); // repaint as the background build progresses

    addAndMakeVisible(playButton_);
    addAndMakeVisible(loopButton_);
    addAndMakeVisible(volumeSlider_);
    addChildComponent(trimButton_);  // appear with active markers
    addChildComponent(resetButton_);
    addAndMakeVisible(gearButton_);

    startTimerHz(30);
}

PlayerPane::~PlayerPane()
{
    thumbnail_.removeChangeListener(this);
}

void PlayerPane::setVolume(double percent)
{
    // Through the slider so the fader, the engine gain and the persistence
    // callback stay one path.
    volumeSlider_.setValue(juce::jlimit(0.0, 100.0, percent), juce::sendNotificationSync);
}

void PlayerPane::setSlot(int slot, const juce::File& wav, const juce::String& title, bool oneShot,
                         long long frames)
{
    slot_ = slot;
    title_ = title;
    oneShot_ = oneShot;
    slotFrames_ = frames;
    applyFile(wav);
}

void PlayerPane::applyFile(const juce::File& wav)
{
    error_.clear();
    currentPath_.clear();
    thumbnail_.clear();
    inSeconds_ = outSeconds_ = 0.0;

    const juce::Result loaded = engine_.load(wav);
    if (loaded.failed()) {
        error_ = loaded.getErrorMessage();
    } else {
        currentPath_ = wav.getFullPathName();
        engine_.setLooping(!oneShot_); // preview with the slot's own behavior
        outSeconds_ = engine_.lengthSeconds();
        thumbnail_.setSource(new juce::FileInputSource(wav));
    }
    markersChanged();
    updateTransportRow();
    repaint();
}

void PlayerPane::reload()
{
    if (currentPath_.isEmpty())
        return;
    thumbnailCache_.clear(); // same path, new bytes — the cached thumbnail lies
    const juce::File file(currentPath_);
    slotFrames_ = 0; // unknown until the next snapshot; frame math re-derives from the reader
    applyFile(file);
}

void PlayerPane::releaseFile()
{
    engine_.unload();
    thumbnail_.setSource(nullptr);
    updateTransportRow();
}

void PlayerPane::clear()
{
    engine_.unload();
    thumbnail_.clear();
    title_.clear();
    error_.clear();
    currentPath_.clear();
    slot_ = 0;
    slotFrames_ = 0;
    inSeconds_ = outSeconds_ = 0.0;
    updateTransportRow();
    repaint();
}

// --- markers ---

void PlayerPane::setMarkers(double inSeconds, double outSeconds)
{
    const double length = engine_.lengthSeconds();
    inSeconds_ = juce::jlimit(0.0, juce::jmax(0.0, length - kMinSectionSeconds), inSeconds);
    outSeconds_ = juce::jlimit(inSeconds_ + kMinSectionSeconds, length, outSeconds);
    markersChanged();
}

bool PlayerPane::markersActive() const
{
    const double length = engine_.lengthSeconds();
    return engine_.hasSource() && length > 0
        && (inSeconds_ > 0.001 || outSeconds_ < length - 0.001);
}

juce::int64 PlayerPane::frameAt(double seconds) const
{
    const double length = engine_.lengthSeconds();
    const long long frames = slotFrames_ > 0
                               ? slotFrames_
                               : static_cast<long long>(std::llround(length * wav::kSampleRate));
    if (length <= 0)
        return 0;
    return juce::jlimit<juce::int64>(0, frames,
                                     static_cast<juce::int64>(std::llround(seconds / length
                                                                           * static_cast<double>(frames))));
}

void PlayerPane::markersChanged()
{
    const bool active = markersActive();
    if (active)
        engine_.setSection(inSeconds_, outSeconds_);
    else
        engine_.clearSection();
    trimButton_.setVisible(active);
    resetButton_.setVisible(active);
    repaint();
}

// --- painting ---

juce::Rectangle<int> PlayerPane::waveArea() const
{
    return getLocalBounds().withTrimmedTop(kTransportRowHeight);
}

float PlayerPane::xOf(double seconds) const
{
    const auto wave = waveArea().toFloat();
    const double length = engine_.lengthSeconds();
    return length > 0 ? wave.getX() + static_cast<float>(seconds / length) * wave.getWidth()
                      : wave.getX();
}

double PlayerPane::secondsAt(float x) const
{
    const auto wave = waveArea().toFloat();
    if (wave.getWidth() <= 0)
        return 0.0;
    const auto fraction = juce::jlimit(0.0f, 1.0f, (x - wave.getX()) / wave.getWidth());
    return fraction * engine_.lengthSeconds();
}

void PlayerPane::updateTransportRow()
{
    playButton_.setEnabled(engine_.hasSource());
    playButton_.setButtonText(juce::String::fromUTF8(engine_.isPlaying() ? "\xe2\x96\xa0"    // stop square
                                                                         : "\xe2\x96\xb6")); // play triangle
    loopButton_.setToggleState(engine_.isLooping(), juce::dontSendNotification);
    lastPaintedPlaying_ = engine_.isPlaying();
}

void PlayerPane::timerCallback()
{
    if (engine_.isPlaying() != lastPaintedPlaying_)
        updateTransportRow(); // e.g. a non-looping slot ran dry on its own
    if (engine_.isPlaying())
        repaint();
}

void PlayerPane::paint(juce::Graphics& g)
{
    const auto wave = waveArea().toFloat();
    g.setColour(kPaneBackground);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);

    // --- transport row text: title left (after the buttons), readout right ---
    const auto row = getLocalBounds().removeFromTop(kTransportRowHeight);
    g.setFont(juce::FontOptions(13.0f));
    if (title_.isNotEmpty()) {
        g.setColour(kText);
        g.drawText(title_, row.withTrimmedLeft(244).withTrimmedRight(420),
                   juce::Justification::centredLeft, true);
    }

    if (!volumeIconArea_.isEmpty()) {
        // The speaker glyph: says "volume" where a bare slider says nothing.
        const auto icon = volumeIconArea_.toFloat();
        const float cx = icon.getX() + 3.0f, cy = icon.getCentreY();
        juce::Path speaker;
        speaker.startNewSubPath(cx, cy - 2.5f);
        speaker.lineTo(cx + 3.0f, cy - 2.5f);
        speaker.lineTo(cx + 7.0f, cy - 6.0f);
        speaker.lineTo(cx + 7.0f, cy + 6.0f);
        speaker.lineTo(cx + 3.0f, cy + 2.5f);
        speaker.lineTo(cx, cy + 2.5f);
        speaker.closeSubPath();
        g.setColour(kDim);
        g.fillPath(speaker);
        juce::Path arc;
        arc.addCentredArc(cx + 8.0f, cy, 3.5f, 3.5f, 0.0f,
                          juce::MathConstants<float>::pi * 0.25f,
                          juce::MathConstants<float>::pi * 0.75f, true);
        g.strokePath(arc, juce::PathStrokeType(1.2f));
    }
    if (engine_.hasSource()) {
        // Selection readout: what the pedal would derive for the trimmed
        // loop — or why it would refuse it.
        if (markersActive()) {
            const juce::int64 selection = frameAt(outSeconds_) - frameAt(inSeconds_);
            juce::String readout;
            juce::Colour colour = kDim;
            try {
                const auto p = params::computeSlotParams(selection);
                readout = "sel " + formatSeconds(outSeconds_ - inSeconds_) + juce::String::fromUTF8(" \xc2\xb7 ")
                        + juce::String(p.measures) + juce::String::fromUTF8(" bars \xc2\xb7 ")
                        + juce::String(p.tempoTenths / 10) + "." + juce::String(p.tempoTenths % 10)
                        + " bpm";
                colour = felitronics::appkit::brand::lilac;
            } catch (const Error&) {
                readout = "selection out of the pedal's tempo range";
                colour = felitronics::appkit::brand::orange;
                trimButton_.setEnabled(false);
            }
            if (colour != felitronics::appkit::brand::orange)
                trimButton_.setEnabled(true);
            g.setColour(colour);
            g.drawText(readout, row.withTrimmedRight(240), juce::Justification::centredRight, false);
        }
        g.setColour(kDim);
        g.drawText(formatSeconds(engine_.positionSeconds()) + " / "
                       + formatSeconds(engine_.lengthSeconds()),
                   row.withTrimmedRight(44), juce::Justification::centredRight, false);
    }

    // --- the waveform ---
    if (error_.isNotEmpty()) {
        g.setColour(felitronics::appkit::brand::orange);
        g.drawText(error_, wave, juce::Justification::centred, true);
        return;
    }
    if (!engine_.hasSource()) {
        g.setColour(kDim);
        g.drawText("Select a slot to listen", wave, juce::Justification::centred, false);
        return;
    }

    g.setColour(felitronics::appkit::brand::violet.withAlpha(0.85f));
    thumbnail_.drawChannels(g, waveArea().reduced(2), 0.0, thumbnail_.getTotalLength(), 0.95f);

    // Outside the markers: dimmed; the section reads as the surviving take.
    if (markersActive()) {
        const float inX = xOf(inSeconds_), outX = xOf(outSeconds_);
        g.setColour(kPaneBackground.withAlpha(0.72f));
        g.fillRect(wave.getX(), wave.getY(), inX - wave.getX(), wave.getHeight());
        g.fillRect(outX, wave.getY(), wave.getRight() - outX, wave.getHeight());

        const auto flag = [&g, &wave](float x, juce::Colour colour, bool pointsRight) {
            g.setColour(colour);
            g.fillRect(x - 0.75f, wave.getY(), 1.5f, wave.getHeight());
            juce::Path p;
            const float d = pointsRight ? 7.0f : -7.0f;
            p.addTriangle(x, wave.getY(), x + d, wave.getY(), x, wave.getY() + 8.0f);
            g.fillPath(p);
        };
        flag(inX, felitronics::appkit::brand::orange, true);
        flag(outX, felitronics::appkit::brand::lilac, false);
    }

    if (engine_.lengthSeconds() > 0) {
        g.setColour(felitronics::appkit::brand::orange);
        g.fillRect(xOf(engine_.positionSeconds()) - 0.75f, wave.getY(), 1.5f, wave.getHeight());
    }
}

void PlayerPane::resized()
{
    auto row = getLocalBounds().removeFromTop(kTransportRowHeight).reduced(6, 4);
    playButton_.setBounds(row.removeFromLeft(46));
    row.removeFromLeft(8);
    loopButton_.setBounds(row.removeFromLeft(64));
    volumeIconArea_ = row.removeFromLeft(16);
    volumeSlider_.setBounds(row.removeFromLeft(84).reduced(0, 3));
    gearButton_.setBounds(row.removeFromRight(24).reduced(0, 1));
    row.removeFromRight(96); // the time readout, drawn in paint()
    trimButton_.setBounds(row.removeFromRight(58).reduced(2, 0));
    resetButton_.setBounds(row.removeFromRight(58).reduced(2, 0));
}

// --- mouse: markers grab first, everything else seeks ---

void PlayerPane::mouseDown(const juce::MouseEvent& e)
{
    drag_ = Drag::none;
    if (!waveArea().contains(e.getPosition()) || !engine_.hasSource())
        return;
    const float x = e.position.x;
    if (markersActive() || true) { // markers are grabbable even at full range (to start a selection, drag the edges)
        if (std::abs(x - xOf(inSeconds_)) <= kMarkerGrabZone)
            drag_ = Drag::inMarker;
        else if (std::abs(x - xOf(outSeconds_)) <= kMarkerGrabZone)
            drag_ = Drag::outMarker;
    }
    if (drag_ == Drag::none) {
        if (e.mods.isAltDown() || e.mods.isRightButtonDown()) {
            // Alt-drag (or right-drag): start a fresh selection from here.
            inSeconds_ = secondsAt(x);
            outSeconds_ = inSeconds_ + kMinSectionSeconds;
            drag_ = Drag::outMarker;
            markersChanged();
        } else {
            drag_ = Drag::seek;
            seekTo(e.position);
        }
    }
}

void PlayerPane::mouseDrag(const juce::MouseEvent& e)
{
    switch (drag_) {
    case Drag::seek:
        seekTo(e.position);
        break;
    case Drag::inMarker:
        inSeconds_ = juce::jlimit(0.0, outSeconds_ - kMinSectionSeconds, secondsAt(e.position.x));
        markersChanged();
        break;
    case Drag::outMarker:
        outSeconds_ = juce::jlimit(inSeconds_ + kMinSectionSeconds, engine_.lengthSeconds(),
                                   secondsAt(e.position.x));
        markersChanged();
        break;
    case Drag::none:
        break;
    }
}

void PlayerPane::mouseUp(const juce::MouseEvent&)
{
    drag_ = Drag::none;
}

void PlayerPane::mouseMove(const juce::MouseEvent& e)
{
    const bool nearMarker = engine_.hasSource() && waveArea().contains(e.getPosition())
                         && (std::abs(e.position.x - xOf(inSeconds_)) <= kMarkerGrabZone
                             || std::abs(e.position.x - xOf(outSeconds_)) <= kMarkerGrabZone);
    setMouseCursor(nearMarker ? juce::MouseCursor::LeftRightResizeCursor
                              : juce::MouseCursor::NormalCursor);
}

void PlayerPane::seekTo(juce::Point<float> position)
{
    if (!engine_.hasSource())
        return;
    engine_.setPosition(secondsAt(position.x));
    repaint();
}

} // namespace loopercat
