// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SectionLoopSource.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>

//==============================================================================
// loopercat::AudioEngine — slot playback over the stock real-time-safe JUCE
// chain: AudioFormatReaderSource -> (buffered) AudioTransportSource ->
// AudioSourcePlayer. The audio thread only ever runs that chain — all our
// code (load/seek/loop) stays on the message thread, and bulk file reads
// happen on the owned read-ahead TimeSliceThread, never in the callback.
//
// The engine IS the device callback (a thin delegate to the player): the
// device manager registers it, and headless harnesses pump the exact same
// entry point with a fake device — no test-only seams.
//==============================================================================
namespace loopercat
{

class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    AudioEngine()
    {
        formats_.registerBasicFormats();
        readAhead_.startThread();
        player_.setSource(&transport_);
    }

    ~AudioEngine() override
    {
        deviceManager_.removeAudioCallback(this);
        player_.setSource(nullptr);
        transport_.setSource(nullptr);
        sectionSource_.reset();
        readerSource_.reset();
        readAhead_.stopThread(2000);
    }

    // Bring up the output device (headless tests never call this — everything
    // else works without a device). `savedStateXml` restores a persisted
    // device choice; empty picks defaults. Returns an error message, empty on
    // success — the UI shows it, playback is simply unavailable until fixed.
    juce::String initialiseDevice(const juce::String& savedStateXml)
    {
        const std::unique_ptr<juce::XmlElement> xml = juce::XmlDocument::parse(savedStateXml);
        const juce::String error = deviceManager_.initialise(0, 2, xml.get(), true);
        deviceManager_.addAudioCallback(this);
        return error;
    }

    juce::AudioDeviceManager& deviceManager() { return deviceManager_; }
    juce::AudioFormatManager& formats() { return formats_; }

    // Load a slot's wav, stopped at position 0; the previous source is
    // released first. Only the header is read here — bulk I/O belongs to the
    // read-ahead thread. MESSAGE THREAD (or a harness's main).
    juce::Result load(const juce::File& file)
    {
        unload();
        std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(file));
        if (reader == nullptr)
            return juce::Result::fail("cannot read " + file.getFullPathName());
        sampleRate_ = reader->sampleRate;
        fileFrames_ = reader->lengthInSamples;
        readerSource_ = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);
        readerSource_->setLooping(looping_);
        sectionSource_ = std::make_unique<SectionLoopSource>(*readerSource_);
        transport_.setSource(sectionSource_.get(), kReadAheadSamples, &readAhead_, sampleRate_);
        transport_.setPosition(0);
        return juce::Result::ok();
    }

    void unload()
    {
        transport_.stop();
        transport_.setSource(nullptr);
        sectionSource_.reset();
        readerSource_.reset();
    }

    bool hasSource() const { return readerSource_ != nullptr; }

    void play()
    {
        jassert(hasSource()); // UI must not offer play with nothing loaded
        if (!hasSource())
            return;
        if (lengthSeconds() - positionSeconds() < 0.01)
            transport_.setPosition(0); // play at the end means play again
        transport_.start();
    }

    void stop() { transport_.stop(); }
    void togglePlay() { isPlaying() ? stop() : play(); }

    bool isPlaying() const { return transport_.isPlaying(); }

    // With a section looping, the transport runs on the endless LINEAR
    // stream (see SectionLoopSource) — fold it back to the audible position.
    double positionSeconds() const
    {
        const double raw = transport_.getCurrentPosition();
        if (sectionSource_ == nullptr || !sectionSource_->hasSection() || sampleRate_ <= 0)
            return raw;
        const auto linear = static_cast<juce::int64>(std::llround(raw * sampleRate_));
        return static_cast<double>(SectionLoopSource::mapToSection(
                   linear, framesFor(sectionStartSeconds_), framesFor(sectionEndSeconds_)))
             / sampleRate_;
    }

    // The FILE length — the transport would report the endless linear stream
    // while a section is active.
    double lengthSeconds() const
    {
        return sampleRate_ > 0 ? static_cast<double>(fileFrames_) / sampleRate_ : 0.0;
    }

    void setPosition(double seconds)
    {
        // In section mode the target is an audible (file) position inside the
        // section, which IS a valid linear position — identity in-range.
        transport_.setPosition(juce::jlimit(0.0, lengthSeconds(), seconds));
    }

    // Seamless wrap at the end of the file (the slot IS a loop). One-shot
    // slots preview with this off, matching their on-pedal behavior.
    void setLooping(bool shouldLoop)
    {
        looping_ = shouldLoop;
        if (readerSource_ != nullptr)
            readerSource_->setLooping(shouldLoop);
    }

    bool isLooping() const { return looping_; }

    // Preview gain, 1.0 = unity. Applies to the monitor output only — the
    // pedal's own per-memory level is a database field, not this.
    void setGain(float gain) { transport_.setGain(gain); }

    // Marker preview: loop the [start, end) second range seamlessly (the
    // wrap happens under the buffering layer — see SectionLoopSource). If
    // playback is outside the section it is pulled to the section start.
    void setSection(double startSeconds, double endSeconds)
    {
        if (sectionSource_ == nullptr)
            return;
        const double audible = positionSeconds(); // BEFORE the bounds move
        sectionStartSeconds_ = startSeconds;
        sectionEndSeconds_ = endSeconds;
        sectionSource_->setSection(framesFor(startSeconds), framesFor(endSeconds));
        if (audible < startSeconds || audible >= endSeconds)
            transport_.setPosition(startSeconds);
        else
            transport_.setPosition(audible); // re-anchor the linear stream at the audible spot
    }

    void clearSection()
    {
        if (sectionSource_ == nullptr || !sectionSource_->hasSection())
            return;
        const double audible = positionSeconds();
        sectionSource_->clearSection();
        transport_.setPosition(audible); // back to plain file coordinates
    }

    bool hasSection() const { return sectionSource_ != nullptr && sectionSource_->hasSection(); }

    // --- AudioIODeviceCallback: a thin delegate to the stock player chain ---

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels,
                                          int numSamples, const juce::AudioIODeviceCallbackContext& context) override
    {
        player_.audioDeviceIOCallbackWithContext(inputChannelData, numInputChannels,
                                                 outputChannelData, numOutputChannels,
                                                 numSamples, context);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        player_.audioDeviceAboutToStart(device);
    }

    void audioDeviceStopped() override { player_.audioDeviceStopped(); }

private:
    // ~1.5 s of read-ahead at 44.1 kHz — generous headroom for USB flash.
    static constexpr int kReadAheadSamples = 1 << 16;

    juce::AudioDeviceManager deviceManager_;
    juce::AudioFormatManager formats_;
    juce::int64 framesFor(double seconds) const
    {
        return static_cast<juce::int64>(std::llround(seconds * sampleRate_));
    }

    juce::TimeSliceThread readAhead_ { "LooperCat audio read-ahead" };
    juce::AudioSourcePlayer player_;
    juce::AudioTransportSource transport_;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource_;
    std::unique_ptr<SectionLoopSource> sectionSource_;
    double sampleRate_ = 44100.0;
    juce::int64 fileFrames_ = 0;
    double sectionStartSeconds_ = 0.0, sectionEndSeconds_ = 0.0;
    bool looping_ = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace loopercat
