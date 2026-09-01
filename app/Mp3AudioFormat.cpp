// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Mp3AudioFormat.h"

// minimp3 lives entirely inside this translation unit: float output to match
// the pedal's own sample format, and the one-and-only implementation expansion.
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>

#include <vector>

namespace loopercat
{

namespace
{
    //==========================================================================
    // The whole file is buffered up front: backing tracks are a few MB, and
    // mp3dec_ex needs random access for its sample-exact seek index. `samples`
    // in minimp3_ex counts INTERLEAVED samples (frames × channels) — so does
    // its seek position — with the Xing/LAME delay and padding already carved
    // off when the tag is present.
    class Mp3Reader final : public juce::AudioFormatReader
    {
    public:
        explicit Mp3Reader(juce::InputStream* stream)
            : juce::AudioFormatReader(stream, "MP3")
        {
            input->readIntoMemoryBlock(data);
            if (mp3dec_ex_open_buf(&dec, static_cast<const juce::uint8*>(data.getData()),
                                   data.getSize(), MP3D_SEEK_TO_SAMPLE)
                != 0)
                return;
            opened = true;
            if (dec.info.channels <= 0 || dec.info.hz <= 0 || dec.samples == 0)
                return;
            sampleRate = dec.info.hz;
            numChannels = static_cast<unsigned int>(dec.info.channels);
            lengthInSamples =
                static_cast<juce::int64>(dec.samples / static_cast<juce::uint64>(dec.info.channels));
            bitsPerSample = 32;
            usesFloatingPointData = true;
        }

        ~Mp3Reader() override
        {
            if (opened)
                mp3dec_ex_close(&dec);
        }

        bool ok() const { return opened && lengthInSamples > 0; }

        bool readSamples(int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                         juce::int64 startSampleInFile, int numSamples) override
        {
            clearSamplesBeyondAvailableLength(destChannels, numDestChannels, startOffsetInDestBuffer,
                                              startSampleInFile, numSamples, lengthInSamples);
            if (numSamples <= 0)
                return true;

            const int channels = static_cast<int>(numChannels);
            const juce::int64 available = lengthInSamples - startSampleInFile;
            const int frames = static_cast<int>(
                juce::jlimit<juce::int64>(0, numSamples, juce::jmax<juce::int64>(0, available)));

            if (frames > 0)
            {
                if (nextFrame != startSampleInFile)
                {
                    if (mp3dec_ex_seek(&dec, static_cast<juce::uint64>(startSampleInFile)
                                                 * static_cast<juce::uint64>(channels))
                        != 0)
                        return false;
                    nextFrame = startSampleInFile;
                }

                interleaved.resize(static_cast<size_t>(frames) * static_cast<size_t>(channels));
                const size_t got = mp3dec_ex_read(&dec, interleaved.data(), interleaved.size());
                const int gotFrames = static_cast<int>(got / static_cast<size_t>(channels));
                nextFrame += gotFrames;

                for (int ch = 0; ch < numDestChannels; ++ch)
                {
                    auto* dest = reinterpret_cast<float*>(destChannels[ch]);
                    if (dest == nullptr)
                        continue;
                    dest += startOffsetInDestBuffer;
                    if (ch < channels)
                    {
                        const float* src = interleaved.data() + ch;
                        for (int i = 0; i < gotFrames; ++i)
                            dest[i] = src[static_cast<size_t>(i) * static_cast<size_t>(channels)];
                        // A short read inside the advertised length would leave
                        // stale garbage — silence is the honest filler.
                        for (int i = gotFrames; i < frames; ++i)
                            dest[i] = 0.0f;
                    }
                    else
                    {
                        for (int i = 0; i < frames; ++i)
                            dest[i] = 0.0f;
                    }
                }
            }
            return true;
        }

    private:
        juce::MemoryBlock data;
        mp3dec_ex_t dec {};
        bool opened = false;
        juce::int64 nextFrame = 0;      // the decoder's cursor, to skip redundant seeks
        std::vector<float> interleaved; // scratch for one readSamples call

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Mp3Reader)
    };
} // namespace

Mp3AudioFormat::Mp3AudioFormat() : juce::AudioFormat("MP3 file", ".mp3") {}

juce::Array<int> Mp3AudioFormat::getPossibleSampleRates()
{
    return { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
}

juce::Array<int> Mp3AudioFormat::getPossibleBitDepths()
{
    return { 32 }; // decoded straight to float
}

juce::AudioFormatReader* Mp3AudioFormat::createReaderFor(juce::InputStream* sourceStream,
                                                         bool deleteStreamIfOpeningFails)
{
    auto reader = std::make_unique<Mp3Reader>(sourceStream);
    if (reader->ok())
        return reader.release();
    if (! deleteStreamIfOpeningFails)
        reader->input = nullptr; // the caller keeps the stream — the standard JUCE reader dance
    return nullptr;
}

} // namespace loopercat
