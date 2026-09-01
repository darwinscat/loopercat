// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <memory>

//==============================================================================
// loopercat::BatchOverlay — the batch takeover (issue #61). While a batch of
// slot jobs runs, this component covers the whole window and swallows every
// click: a half-attended batch and a live UI is how conflicts happen
// (switching the preview onto a slot mid-rewrite is the Windows file-lock
// trap of issue #26). Semi-transparent ON PURPOSE — the table stays visible
// underneath, busy pulse hopping slot to slot, so the lock reads as "working",
// never as "frozen".
//
// Two bars, both honest: the batch (slot k of n, fed by the owner as results
// arrive) and the current file (fed by the core's progress callback through
// an atomic the worker thread writes and our timer reads — real frames, not
// an animation). Cancel finishes the slot in flight — a per-slot write is
// atomic and stays that way — and drops the rest; the owner reports what was
// done and what was dropped.
//
// The overlay swallows input by existing (it intercepts clicks over the full
// window); the owner is responsible for gating keyboard shortcuts while it
// is visible.
//==============================================================================
namespace loopercat
{

class BatchOverlay final : public juce::Component, private juce::Timer
{
public:
    BatchOverlay()
    {
        setInterceptsMouseClicks(true, true);

        title_.setFont(juce::FontOptions(15.0f));
        title_.setColour(juce::Label::textColourId, juce::Colour(0xffd8d8d8));
        title_.setJustificationType(juce::Justification::centred);

        batchCaption_.setFont(juce::FontOptions(11.0f));
        batchCaption_.setColour(juce::Label::textColourId, juce::Colour(0xff8a8a92));
        fileCaption_.setFont(juce::FontOptions(11.0f));
        fileCaption_.setColour(juce::Label::textColourId, juce::Colour(0xff8a8a92));
        fileCaption_.setText("CURRENT FILE", juce::dontSendNotification);

        cancel_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a2a34));
        cancel_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffd8d8d8));
        cancel_.onClick = [this] {
            if (onCancel)
                onCancel();
        };

        for (auto* child : std::initializer_list<juce::Component*> {
                 &title_, &batchCaption_, &batchBar_, &fileCaption_, &fileBar_, &cancel_ })
            addAndMakeVisible(child);
    }

    std::function<void()> onCancel;

    // Arm and show. `filePermille` is written by the worker thread (0..1000,
    // reset by each job as it starts); our timer turns it into the file bar.
    void begin(const juce::String& title, int total,
               std::shared_ptr<const std::atomic<int>> filePermille)
    {
        title_.setText(title, juce::dontSendNotification);
        total_ = total;
        filePermille_ = std::move(filePermille);
        batchProgress_ = 0.0;
        fileProgress_ = 0.0;
        cancel_.setButtonText("Cancel");
        cancel_.setEnabled(true);
        setDone(0);
        setVisible(true);
        toFront(false);
        startTimerHz(30);
    }

    // Results accounted for so far (done + failed); the batch bar and its
    // "k / n" caption both come from here.
    void setDone(int done)
    {
        batchProgress_ = total_ > 0 ? static_cast<double>(done) / total_ : 0.0;
        batchCaption_.setText("BATCH " + juce::String(done) + " / " + juce::String(total_),
                              juce::dontSendNotification);
    }

    // Cancel was pressed: the queued tail is gone, one slot may still be in
    // flight — say so, and make the button honest about being spent.
    void setCancelling()
    {
        cancel_.setButtonText(juce::String::fromUTF8("finishing this slot\xe2\x80\xa6"));
        cancel_.setEnabled(false);
    }

    void end()
    {
        stopTimer();
        filePermille_.reset();
        setVisible(false);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black.withAlpha(0.7f)); // dim but not opaque: the table stays visible below
        g.setColour(juce::Colour(0xf01a1a22));
        g.fillRoundedRectangle(card().toFloat(), 10.0f);
        g.setColour(juce::Colour(0xff2a2a34));
        g.drawRoundedRectangle(card().toFloat().reduced(0.5f), 10.0f, 1.0f);
    }

    void resized() override
    {
        // The current file leads: it is the live, fast-moving bar, and the
        // eye lands on the top row first (field feedback, 2026-09-02).
        auto area = card().reduced(20, 14);
        title_.setBounds(area.removeFromTop(24));
        area.removeFromTop(10);
        fileCaption_.setBounds(area.removeFromTop(16));
        fileBar_.setBounds(area.removeFromTop(18));
        area.removeFromTop(10);
        batchCaption_.setBounds(area.removeFromTop(16));
        batchBar_.setBounds(area.removeFromTop(18));
        area.removeFromTop(14);
        cancel_.setBounds(area.removeFromTop(26).withSizeKeepingCentre(160, 26));
    }

private:
    juce::Rectangle<int> card() const
    {
        return getLocalBounds().withSizeKeepingCentre(juce::jmin(420, getWidth() - 40), 196);
    }

    void timerCallback() override
    {
        if (filePermille_ != nullptr)
            fileProgress_ = filePermille_->load() / 1000.0;
    }

    int total_ = 0;
    double batchProgress_ = 0.0, fileProgress_ = 0.0;
    std::shared_ptr<const std::atomic<int>> filePermille_;

    juce::Label title_, batchCaption_, fileCaption_;
    juce::ProgressBar batchBar_ { batchProgress_ };
    juce::ProgressBar fileBar_ { fileProgress_ };
    juce::TextButton cancel_ { "Cancel" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BatchOverlay)
};

} // namespace loopercat
