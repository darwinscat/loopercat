// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless harness for the player's buttons against what the card permits
// (CardPermissions.h): the REAL PlayerPane over the real AudioEngine with a
// real WAV loaded, its buttons found the way a user finds them — by their
// text — and pressed the way JUCE presses them, no test-only seam.
//
//   - on a card this app only reads (the two-track model's) a loaded loop
//     shows no Normalize…, and a selection shows Reset but no Trim — while
//     the selection itself is heard (the engine has the section)
//   - a press that reaches the hidden button anyway (JUCE delivers a
//     programmatic click to a hidden button) calls nobody
//   - on an RC-5 the same load shows Normalize…, the same selection shows
//     Trim, and the press reaches the owner with the slot and the frames
//   - the pane asks the card every time: a card that changes under a loaded
//     loop changes the buttons on the next layout, no reload needed
//   - a pane never told what it may do offers nothing

#include "support.hpp"

#include "../app/CardPermissions.h"
#include "../app/PlayerPane.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <fstream>
#include <optional>

using namespace loopercat;

namespace {

constexpr int kFrames = 44100; // 1.0 s at the pedal's rate

juce::TextButton* buttonNamed(juce::Component& pane, const juce::String& text)
{
    for (auto* child : pane.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child))
            if (button->getButtonText() == text)
                return button;
    return nullptr;
}

// A programmatic click is posted, not delivered (Button::triggerClick →
// command message), so the loop runs a little after each one.
void press(juce::Button& button)
{
    button.triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
}

// Let the pane's read pass finish and its result land on the message
// thread: Normalize… is disabled while the reading is pending, and a press
// dropped for THAT reason would prove nothing about the card's permission.
void settle(PlayerPane& pane)
{
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (!pane.isThumbnailReady() && juce::Time::getMillisecondCounter() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100); // the posted passFinished
}

// Where the 0.2 s and 0.7 s marks fall in a 44.1 kHz file.
constexpr juce::int64 kInFrame = 8820;
constexpr juce::int64 kOutFrame = 30870;

const juce::String kTrim = "Trim";
const juce::String kReset = "Reset";
const juce::String kNormalize = juce::String::fromUTF8("Normalize\xe2\x80\xa6");

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceRuntime;

    const juce::File wavFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("loopercat_player_pane_harness.wav");
    {
        testkit::WavSpec spec;
        spec.frames = kFrames;
        const auto bytes = testkit::syntheticWav(spec);
        std::ofstream out(wavFile.getFullPathName().toStdString(), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    AudioEngine engine;
    PlayerPane pane(engine);
    pane.setSize(900, 200); // a layout, so the buttons have somewhere to be

    juce::TextButton* trim = buttonNamed(pane, kTrim);
    juce::TextButton* reset = buttonNamed(pane, kReset);
    juce::TextButton* normalize = buttonNamed(pane, kNormalize);
    CHECK(trim != nullptr && reset != nullptr && normalize != nullptr);

    std::optional<int> normalized;
    struct Trimmed {
        int slot;
        juce::int64 in, out;
    };
    std::optional<Trimmed> trimmed;
    pane.onNormalize = [&](int slot) { normalized = slot; };
    pane.onTrim = [&](int slot, juce::int64 in, juce::int64 out) { trimmed = Trimmed { slot, in, out }; };
    // The owner's half of the loudness round trip: the reading comes back as
    // words, and the button is enabled again.
    pane.onLoudnessRead = [&](int slot, const wav::LoudnessReading&) {
        pane.setLoudness(slot, "-14.0 LUFS", false, false, "");
    };

    const auto load = [&] {
        pane.setSlot(1, wavFile, "01 Loop", false, kFrames);
        CHECK(engine.hasSource());
        CHECK(pane.currentPath() == wavFile.getFullPathName());
        settle(pane);
        CHECK(normalize->isEnabled()); // so a dropped press below is the card's doing
    };

    // --- a pane never told what it may do offers nothing ---

    load();
    CHECK(!normalize->isVisible());
    CHECK(!trim->isVisible() && !reset->isVisible()); // no selection yet
    pane.setMarkers(0.2, 0.7);
    CHECK(engine.hasSection()); // the selection is heard regardless
    CHECK(reset->isVisible());
    CHECK(!trim->isVisible());
    CHECK(trim->isEnabled() && normalize->isEnabled());
    press(*trim);
    press(*normalize);
    CHECK(!trimmed.has_value());
    CHECK(!normalized.has_value());

    // --- the two-track model's card: read, never rewritten ---

    pane.permissions = [] { return CardPermissions::of("RC-500"); };
    load();
    CHECK(!normalize->isVisible());
    pane.setMarkers(0.2, 0.7);
    CHECK(engine.hasSection());
    CHECK(reset->isVisible());
    CHECK(!trim->isVisible());
    CHECK(!normalize->isVisible()); // the zone's other mode is off too
    CHECK(trim->isEnabled() && normalize->isEnabled());
    press(*trim);
    press(*normalize);
    CHECK(!trimmed.has_value());
    CHECK(!normalized.has_value());
    press(*reset); // the selection can still be dropped
    CHECK(!engine.hasSection());
    CHECK(!trim->isVisible() && !reset->isVisible());
    CHECK(!normalize->isVisible()); // and Normalize does not come back on this card

    // --- an RC-5: the same gestures reach the owner ---

    pane.permissions = [] { return CardPermissions::of("RC-5"); };
    load();
    CHECK(normalize->isVisible());
    CHECK(!trim->isVisible());
    press(*normalize);
    CHECK(normalized.has_value() && *normalized == 1);
    pane.setMarkers(0.2, 0.7);
    CHECK(trim->isVisible() && reset->isVisible());
    CHECK(!normalize->isVisible()); // a selection owns the zone
    press(*trim);
    CHECK(trimmed.has_value());
    CHECK(trimmed->slot == 1);
    CHECK_EQ(trimmed->in, kInFrame);
    CHECK_EQ(trimmed->out, kOutFrame);
    press(*reset);
    CHECK(!trim->isVisible() && normalize->isVisible());

    // --- the card is asked every time, not remembered at load ---

    std::string family = "RC-5";
    pane.permissions = [&family] { return CardPermissions::of(family); };
    load();
    pane.setMarkers(0.2, 0.7);
    CHECK(trim->isVisible());
    family = "RC-500"; // the card under the loaded loop is now one this app only reads
    trimmed.reset();
    normalized.reset();
    CHECK(trim->isEnabled());
    press(*trim); // the button is still on screen from the last layout…
    CHECK(!trimmed.has_value()); // …and still calls nobody
    pane.setMarkers(0.3, 0.8); // the next layout takes it away
    CHECK(!trim->isVisible() && reset->isVisible());
    press(*reset);
    CHECK(!normalize->isVisible());
    press(*normalize);
    CHECK(!normalized.has_value());
    family = "RC-5";
    pane.setMarkers(0.2, 0.7);
    CHECK(trim->isVisible());

    pane.clear();
    wavFile.deleteFile();
    return testkit::summary("player_pane_harness");
}
