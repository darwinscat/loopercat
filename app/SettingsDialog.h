// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "AppSettings.h"
#include "TabStrip.h"

#include <felitronics/appkit/AudioSettingsPanel.h>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::SettingsDialog — the app's own settings, behind the gear in the
// header: the audio output on one tab, what the slot table shows on another,
// what happens to uploads on a third.
//
// Table columns are a preference, not a mode: the pedal's own facts (name,
// duration, bars, tempo, file) always show, and the per-slot behaviour flags
// are opt-in — One Shot on by default because most players use it, Play
// Count-In off until someone asks for it.
//
// Normalize-on-upload (issue #53) is opt-in and OFF by default: off keeps
// the byte-exact promise — a file the pedal accepts passes through untouched
// — so existing workflows see exactly the behaviour of every version before
// this one. The target is stored in LUFS; the label translates it into
// ReplayGain vocabulary (reference 89 dB = -18 LUFS, the linear offset is
// +107) because "normalize to 89 dB" is how the request arrived.
//==============================================================================
namespace loopercat
{

class SettingsDialog final : public juce::Component
{
public:
    struct Columns {
        bool oneShot;
        bool countIn;
    };

    struct ImportPrefs {
        bool normalizeOnUpload;
        double targetLufs;
    };

    // The target field refuses values outside this window: hotter than -8
    // leaves no headroom against a live band's transients, quieter than -30
    // buries the loop under any stage noise — both are typos, not choices.
    static constexpr double kMinTargetLufs = -30.0, kMaxTargetLufs = -8.0;

    SettingsDialog(juce::AudioDeviceManager& devices, AppSettings& settings,
                   Columns columns, std::function<void(Columns)> onColumnsChanged,
                   ImportPrefs importPrefs, std::function<void(ImportPrefs)> onImportChanged)
        : audio_(devices, { .minInputs = 0, .maxInputs = 0, .minOutputs = 2, .maxOutputs = 2 }),
          settings_(settings),
          onColumnsChanged_(std::move(onColumnsChanged)),
          importPrefs_(importPrefs),
          onImportChanged_(std::move(onImportChanged))
    {
        tabs_.onTabChanged = [this](int index) { showTab(index); };

        columnsTitle_.setText("Show these columns in the slot table",
                              juce::dontSendNotification);
        columnsTitle_.setFont(juce::FontOptions(12.0f));
        columnsTitle_.setColour(juce::Label::textColourId, juce::Colour(0xff8a8a92));

        oneShot_.setToggleState(columns.oneShot, juce::dontSendNotification);
        countIn_.setToggleState(columns.countIn, juce::dontSendNotification);
        for (auto* toggle : { &oneShot_, &countIn_ }) {
            toggle->setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd8d8d8));
            toggle->setColour(juce::ToggleButton::tickColourId,
                              felitronics::appkit::brand::violet);
            toggle->onClick = [this] { commitColumns(); };
            addChildComponent(toggle);
        }
        addChildComponent(columnsTitle_);

        normalize_.setToggleState(importPrefs_.normalizeOnUpload, juce::dontSendNotification);
        normalize_.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd8d8d8));
        normalize_.setColour(juce::ToggleButton::tickColourId,
                             felitronics::appkit::brand::violet);
        normalize_.onClick = [this] {
            importPrefs_.normalizeOnUpload = normalize_.getToggleState();
            target_.setEnabled(importPrefs_.normalizeOnUpload);
            commitImport();
        };
        addChildComponent(normalize_);

        targetCaption_.setText("Target loudness, LUFS", juce::dontSendNotification);
        targetCaption_.setFont(juce::FontOptions(12.0f));
        targetCaption_.setColour(juce::Label::textColourId, juce::Colour(0xff8a8a92));
        addChildComponent(targetCaption_);

        target_.setInputRestrictions(6, "-0123456789.");
        target_.setJustification(juce::Justification::centredRight);
        target_.setText(formatLufs(importPrefs_.targetLufs), juce::dontSendNotification);
        target_.setEnabled(importPrefs_.normalizeOnUpload);
        target_.onReturnKey = [this] { parseTarget(); };
        target_.onFocusLost = [this] { parseTarget(); };
        addChildComponent(target_);

        targetEquiv_.setFont(juce::FontOptions(12.0f));
        targetEquiv_.setColour(juce::Label::textColourId, juce::Colour(0xff8a8a92));
        refreshEquivalence();
        addChildComponent(targetEquiv_);

        importHint_.setText("Every upload lands at the same perceived loudness "
                            "(ITU-R BS.1770). Off: files the pedal accepts pass "
                            "through byte-exact, as before.",
                            juce::dontSendNotification);
        importHint_.setFont(juce::FontOptions(11.0f));
        importHint_.setColour(juce::Label::textColourId, juce::Colour(0xff6f6f78));
        addChildComponent(importHint_);

        addAndMakeVisible(tabs_);
        addAndMakeVisible(audio_);
        showTab(0);
        setSize(520, 300);
    }

    ~SettingsDialog() override
    {
        if (auto* file = settings_.file()) {
            file->setValue(kDeviceStateKey, audio_.saveState());
            file->saveIfNeeded();
        }
    }

    static constexpr const char* kDeviceStateKey = "audioDeviceState";

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff121218)); }

    void resized() override
    {
        auto area = getLocalBounds();
        tabs_.setBounds(area.removeFromTop(30));
        area = area.reduced(12, 10);
        audio_.setBounds(area);

        auto columnsArea = area;
        columnsTitle_.setBounds(columnsArea.removeFromTop(20));
        columnsArea.removeFromTop(8);
        oneShot_.setBounds(columnsArea.removeFromTop(26));
        countIn_.setBounds(columnsArea.removeFromTop(26));

        auto importArea = area;
        normalize_.setBounds(importArea.removeFromTop(26));
        importArea.removeFromTop(10);
        auto targetRow = importArea.removeFromTop(24);
        targetCaption_.setBounds(targetRow.removeFromLeft(150));
        target_.setBounds(targetRow.removeFromLeft(64));
        targetRow.removeFromLeft(8);
        targetEquiv_.setBounds(targetRow);
        importArea.removeFromTop(12);
        importHint_.setBounds(importArea.removeFromTop(40));
    }

private:
    void showTab(int index)
    {
        audio_.setVisible(index == 0);
        columnsTitle_.setVisible(index == 1);
        oneShot_.setVisible(index == 1);
        countIn_.setVisible(index == 1);
        for (auto* c : std::initializer_list<juce::Component*> {
                 &normalize_, &targetCaption_, &target_, &targetEquiv_, &importHint_ })
            c->setVisible(index == 2);
    }

    void commitColumns()
    {
        if (onColumnsChanged_)
            onColumnsChanged_({ oneShot_.getToggleState(), countIn_.getToggleState() });
    }

    void commitImport()
    {
        if (onImportChanged_)
            onImportChanged_(importPrefs_);
    }

    // "-18" for whole targets, "-17.5" otherwise — the field shows a number a
    // player typed, not a printf artefact.
    static juce::String formatLufs(double lufs)
    {
        juce::String s(lufs, 1);
        return s.endsWith(".0") ? s.dropLastCharacters(2) : s;
    }

    void refreshEquivalence()
    {
        // ReplayGain 2.0 fixes -18 LUFS = the RG 1.0 "89 dB" reference; the
        // scale is linear, so any target translates by the same +107 offset.
        targetEquiv_.setText("= ReplayGain " + formatLufs(importPrefs_.targetLufs + 107.0)
                                 + " dB",
                             juce::dontSendNotification);
    }

    void parseTarget()
    {
        // Unparsable text reads as 0.0, and 0 sits outside the window like
        // every other non-target — one range check rejects both.
        const double value = target_.getText().trim().getDoubleValue();
        if (value < kMinTargetLufs || value > kMaxTargetLufs) {
            // Not a target — snap back to the stored one, visibly.
            target_.setText(formatLufs(importPrefs_.targetLufs), juce::dontSendNotification);
            return;
        }
        importPrefs_.targetLufs = value;
        target_.setText(formatLufs(value), juce::dontSendNotification);
        refreshEquivalence();
        commitImport();
    }

    TabStrip tabs_ { { "Audio", "Columns", "Import" } };
    felitronics::appkit::AudioSettingsPanel audio_;
    AppSettings& settings_;
    std::function<void(Columns)> onColumnsChanged_;
    juce::Label columnsTitle_;
    juce::ToggleButton oneShot_ { "One Shot" };
    juce::ToggleButton countIn_ { "Play Count-In" };

    ImportPrefs importPrefs_;
    std::function<void(ImportPrefs)> onImportChanged_;
    juce::ToggleButton normalize_ { "Normalize uploads to a loudness target" };
    juce::Label targetCaption_;
    juce::TextEditor target_;
    juce::Label targetEquiv_;
    juce::Label importHint_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsDialog)
};

} // namespace loopercat
