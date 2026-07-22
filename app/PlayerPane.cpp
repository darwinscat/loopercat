// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PlayerPane.h"

namespace loopercat
{

namespace
{
    const juce::Colour kPaneBackground { 0xff0e0e13 };
    const juce::Colour kText { 0xffd8d8d8 };
    const juce::Colour kDim { 0xff63636d };
    constexpr int kTransportRowHeight = 30;

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

    gearButton_.onClick = [this] {
        if (onGear)
            onGear();
    };

    thumbnail_.addChangeListener(this); // repaint as the background build progresses

    addAndMakeVisible(playButton_);
    addAndMakeVisible(loopButton_);
    addAndMakeVisible(gearButton_);

    startTimerHz(30);
}

PlayerPane::~PlayerPane()
{
    thumbnail_.removeChangeListener(this);
}

void PlayerPane::setSlot(const juce::File& wav, const juce::String& title, bool oneShot)
{
    title_ = title;
    error_.clear();
    currentPath_.clear();
    thumbnail_.clear();

    const juce::Result loaded = engine_.load(wav);
    if (loaded.failed()) {
        error_ = loaded.getErrorMessage();
    } else {
        currentPath_ = wav.getFullPathName();
        engine_.setLooping(!oneShot); // preview with the slot's own behavior
        thumbnail_.setSource(new juce::FileInputSource(wav));
    }
    updateTransportRow();
    repaint();
}

void PlayerPane::clear()
{
    engine_.unload();
    thumbnail_.clear();
    title_.clear();
    error_.clear();
    currentPath_.clear();
    updateTransportRow();
    repaint();
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

juce::Rectangle<int> PlayerPane::waveArea() const
{
    return getLocalBounds().withTrimmedTop(kTransportRowHeight);
}

void PlayerPane::paint(juce::Graphics& g)
{
    const auto wave = waveArea().toFloat();
    g.setColour(kPaneBackground);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);

    // --- transport row text: title left (after the buttons), time right ---
    const auto row = getLocalBounds().removeFromTop(kTransportRowHeight);
    g.setFont(juce::FontOptions(13.0f));
    if (title_.isNotEmpty()) {
        g.setColour(kText);
        g.drawText(title_, row.withTrimmedLeft(150).withTrimmedRight(160),
                   juce::Justification::centredLeft, true);
    }
    if (engine_.hasSource()) {
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

    if (engine_.lengthSeconds() > 0) {
        const auto fraction = engine_.positionSeconds() / engine_.lengthSeconds();
        const float x = wave.getX() + static_cast<float>(fraction) * wave.getWidth();
        g.setColour(felitronics::appkit::brand::orange);
        g.fillRect(x - 0.75f, wave.getY(), 1.5f, wave.getHeight());
    }
}

void PlayerPane::resized()
{
    auto row = getLocalBounds().removeFromTop(kTransportRowHeight).reduced(6, 4);
    playButton_.setBounds(row.removeFromLeft(46));
    row.removeFromLeft(8);
    loopButton_.setBounds(row.removeFromLeft(64));
    gearButton_.setBounds(row.removeFromRight(24).reduced(0, 1));
}

void PlayerPane::seekTo(juce::Point<float> position)
{
    const auto wave = waveArea().toFloat();
    if (!engine_.hasSource() || wave.getWidth() <= 0.0f)
        return;
    const auto fraction = juce::jlimit(0.0f, 1.0f, (position.x - wave.getX()) / wave.getWidth());
    engine_.setPosition(fraction * engine_.lengthSeconds());
    repaint();
}

void PlayerPane::mouseDown(const juce::MouseEvent& e)
{
    if (waveArea().contains(e.getPosition()))
        seekTo(e.position);
}

void PlayerPane::mouseDrag(const juce::MouseEvent& e)
{
    if (waveArea().contains(e.getMouseDownPosition()))
        seekTo(e.position);
}

} // namespace loopercat
