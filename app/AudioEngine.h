// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

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
        const double sampleRate = reader->sampleRate;
        readerSource_ = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);
        readerSource_->setLooping(looping_);
        transport_.setSource(readerSource_.get(), kReadAheadSamples, &readAhead_, sampleRate);
        transport_.setPosition(0);
        return juce::Result::ok();
    }

    void unload()
    {
        transport_.stop();
        transport_.setSource(nullptr);
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
    double positionSeconds() const { return transport_.getCurrentPosition(); }
    double lengthSeconds() const { return transport_.getLengthInSeconds(); }

    void setPosition(double seconds)
    {
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
    juce::TimeSliceThread readAhead_ { "LooperCat audio read-ahead" };
    juce::AudioSourcePlayer player_;
    juce::AudioTransportSource transport_;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource_;
    bool looping_ = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace loopercat
