// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "support.hpp"

#include "../app/ClearSlotAction.h"
#include "../app/AudioEngine.h"
#include "../app/HistoryPane.h"
#include "../app/history/CardRestore.h"
#include "../app/history/HistoryRecorder.h"
#include "../app/history/TakeAudition.h"

#include <bit>
#include <chrono>
#include <map>

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

constexpr int kBlock = 512;
constexpr int kFrames = 88200; // one bar at 120 BPM, accepted by the pedal

float rampSample(int frame)
{
    return static_cast<float>(frame - kFrames / 2) / 131072.0f;
}

// The real playback callback runs against a device description, without hardware.
struct FakeDevice final : juce::AudioIODevice {
    FakeDevice() : juce::AudioIODevice("fake", "fake-type") {}
    juce::StringArray getOutputChannelNames() override { return { "L", "R" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start(juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return false; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return 44100.0; }
    int getCurrentBitDepth() override { return 16; }
    juce::BigInteger getActiveOutputChannels() const override
    {
        juce::BigInteger b;
        b.setRange(0, 2, true);
        return b;
    }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
};

struct Fixture {
    fs::path root = fs::temp_directory_path()
        / ("loopercat-clear-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path card = root / "card";
    std::shared_ptr<history::HistoryRecorder> rec;

    Fixture()
    {
        fs::create_directories(volume::dataDir(card));
        fs::create_directories(card / "ROLAND" / "WAVE");
        for (const int bank : { 1, 2 })
            commands::writeFileBytes(volume::memoryPath(card, bank),
                rc0::setTailMarker(testkit::syntheticMemoryText(), bank));
        rec = std::make_shared<history::HistoryRecorder>(root / "history", "RC-5",
            [] { return juce::Time::currentTimeMillis(); });
        auto wav = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = kFrames });
        // The pedal accepts float32; each sample encodes its frame, with mirrored stereo.
        for (int frame = 0; frame < kFrames; ++frame)
            for (int channel = 0; channel < 2; ++channel) {
                const float sample = rampSample(frame) * (channel == 0 ? 1.0f : -1.0f);
                const auto bits = std::bit_cast<std::uint32_t>(sample);
                const auto offset = static_cast<std::size_t>(44 + frame * 8 + channel * 4);
                for (unsigned byte = 0; byte < 4; ++byte)
                    wav[offset + byte] = static_cast<unsigned char>((bits >> (byte * 8)) & 0xff);
            }
        const auto source = root / "take.wav";
        commands::writeFileBytes(source,
            std::string_view(reinterpret_cast<const char*>(wav.data()), wav.size()));
        run("push", [&](const auto& write) {
            commands::PushOptions options {};
            options.write = write;
            commands::push(card, source, 4, options);
        });
    }

    ~Fixture() { rec.reset(); fs::remove_all(root); }

    template <typename Work>
    void run(const std::string& kind, Work work)
    {
        commands::WriteOptions options {};
        options.opId = kind;
        const auto write = history::withHistory(rec, std::move(options));
        rec->begin(kind, kind, card);
        work(write);
        rec->finish(kind, {});
    }

    std::map<std::string, std::string> cardBytes() const
    {
        std::map<std::string, std::string> bytes;
        for (const auto& file : fs::recursive_directory_iterator(card))
            if (file.is_regular_file())
                bytes[fs::relative(file.path(), card).string()] = commands::readFileBytes(file.path());
        return bytes;
    }
};

void settle()
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
}

juce::Button* buttonNamed(juce::Component& parent, const juce::String& name)
{
    for (auto* child : parent.getChildren())
        if (auto* button = dynamic_cast<juce::Button*>(child))
            if (button->getButtonText() == name)
                return button;
    return nullptr;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI runtime;

    // Inspect the same item the slot context menu adds, including its enabled state.
    for (const bool hasTake : { false, true }) {
        juce::PopupMenu menu;
        clearSlotAction::addToMenu(menu, hasTake);
        juce::PopupMenu::MenuItemIterator items(menu);
        CHECK(items.next());
        CHECK_EQ(items.getItem().itemID, clearSlotAction::menuItemId);
        CHECK_EQ(items.getItem().text, juce::String::fromUTF8("Clear slot\xe2\x80\xa6"));
        CHECK_EQ(items.getItem().isEnabled, hasTake);
        CHECK(!items.next());
    }

    // Even a direct request for an empty slot must not ask or enqueue a job.
    {
        Fixture fixture;
        const auto before = fixture.cardBytes();
        CHECK(volume::listSlotWavs(fixture.card, 5).empty());
        int jobs = 0;
        int asks = 0;
        clearSlotAction::request(5, false, fixture.rec, [&](PedalWorker::Job) { ++jobs; },
            [&](int, std::function<void(int)>) { ++asks; });
        settle();
        CHECK_EQ(asks, 0);
        CHECK_EQ(jobs, 0);
        CHECK(fixture.cardBytes() == before);
        CHECK_EQ(fixture.rec->store().cardTimeline().size(), 1u);
        CHECK(juce::Component::getCurrentlyModalComponent() == nullptr);
    }

    // Supply the modal result without creating an AlertWindow: its constructor
    // creates a native peer, which requires an X display on Linux CI.
    // Cancel, Clear, dismissal and unexpected results use the app's actual gate.
    for (const int response : { 0, 1, -1, 2 }) {
        Fixture fixture;
        const auto before = fixture.cardBytes();
        const auto body = rc0::slotBody(commands::readMemory(fixture.card), 4);
        const auto name = volume::listSlotWavs(fixture.card, 4).front();
        const auto take = commands::readFileBytes(volume::wavDir(fixture.card, 4) / name);
        int jobs = 0;
        int asks = 0;
        std::function<void(int)> decide;
        clearSlotAction::request(4, true, fixture.rec, [&](PedalWorker::Job job) {
            ++jobs;
            CHECK_EQ(job.slot, 4);
            CHECK_EQ(job.description, juce::String("Clear slot 4"));
            CHECK(job.before != nullptr);
            CHECK(job.after != nullptr);
            // Execute the app's job using the worker's lifecycle order.
            job.before(fixture.card);
            job.work(fixture.card);
            job.after({});
        }, [&](int slot, std::function<void(int)> callback) {
            ++asks;
            CHECK_EQ(slot, 4);
            decide = std::move(callback);
        });
        CHECK_EQ(asks, 1);
        CHECK_EQ(jobs, 0);
        CHECK(fixture.cardBytes() == before);
        CHECK_EQ(fixture.rec->store().cardTimeline().size(), 1u);
        CHECK(juce::Component::getCurrentlyModalComponent() == nullptr);
        CHECK(decide != nullptr);
        if (decide == nullptr)
            continue;
        settle();
        CHECK_EQ(jobs, 0);
        CHECK(fixture.cardBytes() == before);
        CHECK_EQ(fixture.rec->store().cardTimeline().size(), 1u);
        decide(response);
        settle();
        CHECK(juce::Component::getCurrentlyModalComponent() == nullptr);
        if (response != 1) {
            CHECK_EQ(jobs, 0);
            CHECK(fixture.cardBytes() == before); // includes both banks, WAVs and every card file
            CHECK_EQ(fixture.rec->store().cardTimeline().size(), 1u);
            CHECK(!fixture.rec->store().takeBytes(history::HistoryStore::contentHash(take)));
            continue;
        }
        CHECK_EQ(jobs, 1);
        CHECK(volume::listSlotWavs(fixture.card, 4).empty());
        CHECK(rc0::slotBody(commands::readMemory(fixture.card), 4) == rc0::factorySlotBody(4));
        const auto rows = history::rows::forSlot(fixture.rec->store().slotTimeline(4));
        CHECK_EQ(rows.size(), 2u);
        CHECK_EQ(rows.back().line.action, std::string("Cleared"));
        CHECK(rows.back().playable);
        CHECK(rows.front().restorable);

        HistoryPane pane;
        std::vector<HistoryPane::Row> shown;
        for (const auto& row : rows)
            shown.push_back({ {}, juce::String(row.line.action), juce::String(row.line.detail),
                juce::String(row.line.audio), row.playable, row.restorable, row.op });
        pane.setRows(std::move(shown), 4);
        history::TakeAudition audition(fixture.root / "audition");
        AudioEngine engine;
        FakeDevice device;
        engine.audioDeviceAboutToStart(&device);
        int plays = 0;
        pane.onPlay = [&](std::int64_t op) {
            ++plays;
            CHECK_EQ(op, rows.back().op);
            const auto file = audition.materialize(fixture.rec->store(), rows.back().takeHash);
            CHECK(file.has_value());
            if (!file)
                return;
            CHECK(commands::readFileBytes(*file) == take);
            CHECK(engine.load(juce::File(juce::String(file->string()))).wasOk());
            engine.play();
        };
        auto* play = buttonNamed(pane, "Play");
        CHECK(play != nullptr && play->isEnabled());
        if (play != nullptr)
            play->triggerClick();
        settle();
        CHECK_EQ(plays, 1);
        CHECK(engine.isPlaying());
        bool heard = false;
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000;
        while (!heard && juce::Time::getMillisecondCounterHiRes() < deadline) {
            float left[kBlock] {}, right[kBlock] {};
            float* outputs[] { left, right };
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, kBlock, {});
            for (int i = 0; i < kBlock; ++i) {
                if (std::abs(left[i]) < 1.0e-6f)
                    continue;
                const auto frame = static_cast<int>(std::lround(left[i] * 131072.0f)) + kFrames / 2;
                CHECK(frame >= 0 && frame < kFrames / 4);
                bool matches = true;
                for (int sample = i; sample < kBlock; ++sample)
                    if (std::abs(left[sample] - rampSample(frame + sample - i)) >= 1.0e-6f
                        || std::abs(left[sample] + right[sample]) >= 1.0e-6f)
                        matches = false;
                CHECK(matches);
                heard = true;
                break;
            }
            if (!heard)
                juce::Thread::sleep(10);
        }
        CHECK(heard);
        engine.unload();
        engine.audioDeviceStopped();

        int restores = 0;
        pane.onRestore = [&](std::int64_t op) {
            ++restores;
            CHECK_EQ(op, rows.front().op);
            fixture.run("restore", [&](const auto& write) {
                history::restoreOperation(fixture.rec->store(), op, fixture.card, write);
            });
        };
        for (auto* child : pane.getChildren())
            if (auto* list = dynamic_cast<juce::ListBox*>(child))
                list->selectRow(0);
        auto* restore = buttonNamed(pane, "Restore this state");
        CHECK(restore != nullptr && restore->isEnabled());
        if (restore != nullptr)
            restore->triggerClick();
        settle();
        CHECK_EQ(restores, 1);
        CHECK(rc0::slotBody(commands::readMemory(fixture.card), 4) == body);
        CHECK(volume::listSlotWavs(fixture.card, 4) == std::vector<std::string> { name });
        CHECK(commands::readFileBytes(volume::wavDir(fixture.card, 4) / name) == take);
    }
    return testkit::summary("clear_confirmation_tests");
}
