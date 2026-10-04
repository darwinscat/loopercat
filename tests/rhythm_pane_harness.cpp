// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless harness for the Rhythm tab against what the card permits
// (CardPermissions.h) and what the slot holds: the REAL RhythmPane, its rows
// from the format's synthetic file, its switch pressed and its fields
// changed the way JUCE delivers them — no test-only seam. The theory:
//
//   - a tab never told what it may do offers nothing: every control a lamp,
//     no callback from any gesture
//   - on a card this app only reads (the two-track model's) the same, and
//     the footer names what this app writes to
//   - on an RC-5 the switch reports the opposite of what the slot has, a
//     choice reports its number, a typed number reports itself — each as
//     ONE Edits with ONE field, for the selected slot
//   - a value typed equal to the slot's own is nothing; so is a re-selected
//     choice
//   - a slot with a take fixes BEAT and only BEAT
//   - a slot at a beat whose pattern list is not charted (anything but 4/4,
//     hardware 2026-10-01) shows PATTERN as the number the card holds, with
//     the caption saying why, and only PATTERN is a lamp; the switch is a
//     lamp exactly where its click would need a pattern number, and the
//     line beside it says why before the click
//   - a slot at a beat the manual's list does not even have (BEAT is
//     inferred past its one anchor) is shown, not thrown on: the caption
//     says the number is not in the list, and a State-only switch is offered
//   - a busy tab is a lamp until the job is done; no slot means no controls

#include "support.hpp"

#include "../app/CardPermissions.h"
#include "../app/RhythmPane.h"

#include <loopercat/Catalog.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

using namespace loopercat;
namespace rhythm = loopercat::usecases::rhythm;

namespace {

template <typename T>
T* controlOf(juce::Component& root, const juce::String& id)
{
    if (auto* found = dynamic_cast<T*>(root.findChildWithID(id)))
        return found;
    for (auto* child : root.getChildren())
        if (auto* found = controlOf<T>(*child, id))
            return found;
    return nullptr;
}

juce::MouseEvent clickOn(juce::Component& on)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), { 4.0f, 4.0f },
                            juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            &on, &on, now, { 4.0f, 4.0f }, now, 1, false);
}

// A caption somewhere under the pane with exactly this text. Captions sit
// inside the controls, so the search descends — and asks for the whole text,
// since a ComboBox keeps a Label of its own that reads "6/4" or "3".
bool captionReads(juce::Component& root, const juce::String& text)
{
    for (auto* child : root.getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child))
            if (label->getText() == text)
                return true;
        if (captionReads(*child, text))
            return true;
    }
    return false;
}

// The pane's own line (the orange cost, the footer): a direct child only,
// so a control's inner label cannot answer for it.
bool paneSays(juce::Component& pane, const juce::String& fragment)
{
    for (auto* child : pane.getChildren())
        if (auto* label = dynamic_cast<juce::Label*>(child))
            if (label->getText().contains(fragment))
                return true;
    return false;
}

// The pane's rows: slot 7 with drums on (Rock1, Jazz), slot 8 with a take
// recorded (BEAT fixed) and a count-in in front of its groove, slot 9 factory.
// Then four at 6/4 (Beat 4), the beat whose list the hardware showed is not
// ours (#147): slot 10 holds 3 — the pedal's own Rock2 at 6/4 — with drums
// on; slot 11 the same with a count-in in front; slot 12 holds 57, off; slot
// 13 holds 19 — the number the pedal could not name — off. Then two at Beat
// 17, a number the manual's list lacks: slot 14 holds 3, off; slot 15 holds
// 57, off. Slot 16 is 4/4 in the count-in's own shape (State on, Blank,
// count on), where BEAT must stay.
std::vector<SlotRow> rowsOf()
{
    std::string text = testkit::syntheticMemoryText();
    const auto set = [&text](int slot, const char* section, const char* tag, long long value) {
        text = rc0::replaceSlotBody(
            text, slot, rc0::setSectionField(rc0::slotBody(text, slot), section, tag, value));
    };
    set(7, "RHYTHM", "State", rc0::kRhythmStateOn);
    set(7, "RHYTHM", "Pattern", 11);
    set(7, "RHYTHM", "Kit", 2);
    set(7, "RHYTHM", "Level", 120);
    set(8, "TRACK1", "WavStat", rc0::kWavStatIndexed);
    set(8, "TRACK1", "WavLen", 88200);
    set(8, "RHYTHM", "State", rc0::kRhythmStateOn);
    set(8, "RHYTHM", "Pattern", 45);
    set(8, "RHYTHM", "PlayCount", rc0::kRhythmPlayCount1Meas);
    for (int slot = 10; slot <= 13; ++slot)
        set(slot, "RHYTHM", "Beat", 4);
    set(10, "RHYTHM", "State", rc0::kRhythmStateOn);
    set(10, "RHYTHM", "Pattern", 3);
    set(11, "RHYTHM", "State", rc0::kRhythmStateOn);
    set(11, "RHYTHM", "Pattern", 3);
    set(11, "RHYTHM", "PlayCount", rc0::kRhythmPlayCount1Meas);
    set(12, "RHYTHM", "Pattern", rc0::kRhythmPatternBlank);
    set(13, "RHYTHM", "Pattern", 19);
    set(14, "RHYTHM", "Beat", 17);
    set(14, "RHYTHM", "Pattern", 3);
    set(15, "RHYTHM", "Beat", 17);
    set(15, "RHYTHM", "Pattern", rc0::kRhythmPatternBlank);
    set(16, "RHYTHM", "State", rc0::kRhythmStateOn);
    set(16, "RHYTHM", "Pattern", rc0::kRhythmPatternBlank);
    set(16, "RHYTHM", "PlayCount", rc0::kRhythmPlayCount1Meas);
    std::vector<SlotRow> rows;
    for (auto& info : catalog::listSlots(text))
        rows.push_back({ std::move(info), "", "", { "" } });
    return rows;
}

struct Heard {
    int calls = 0;
    int slot = 0;
    rhythm::Edits last;
};

int fieldsSet(const rhythm::Edits& e)
{
    return int(e.on.has_value()) + int(e.pattern.has_value()) + int(e.kit.has_value())
        + int(e.beat.has_value()) + int(e.variation.has_value()) + int(e.level.has_value())
        + int(e.reverb.has_value()) + int(e.toneLow.has_value()) + int(e.toneHigh.has_value());
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceRuntime;

    const std::vector<SlotRow> rows = rowsOf();
    RhythmPane pane;
    pane.setSize(880, 148);

    auto* toggle = controlOf<RhythmSwitch>(pane, "switch");
    auto* pattern = controlOf<juce::ComboBox>(pane, "pattern");
    auto* kit = controlOf<juce::ComboBox>(pane, "kit");
    auto* beat = controlOf<juce::ComboBox>(pane, "beat");
    auto* variation = controlOf<juce::ComboBox>(pane, "variation");
    auto* level = controlOf<juce::TextEditor>(pane, "level");
    auto* reverb = controlOf<juce::TextEditor>(pane, "reverb");
    auto* toneLow = controlOf<juce::TextEditor>(pane, "toneLow");
    auto* toneHigh = controlOf<juce::TextEditor>(pane, "toneHigh");
    CHECK(toggle && pattern && kit && beat && variation && level && reverb && toneLow && toneHigh);
    if (!(toggle && pattern && kit && beat && variation && level && reverb && toneLow && toneHigh))
        return testkit::summary("rhythm_pane_harness");

    Heard heard;
    pane.onEdit = [&heard](int slot, rhythm::Edits edits) {
        ++heard.calls;
        heard.slot = slot;
        heard.last = std::move(edits);
    };

    const auto press = [&] { toggle->mouseDown(clickOn(*toggle)); };
    const auto choose = [](juce::ComboBox& box, long long number) {
        box.setSelectedId(static_cast<int>(number) + 1, juce::sendNotificationSync);
    };
    // Return in a TextEditor is delivered as a command message, so the loop
    // has to turn once before onReturnKey fires — as it does for a player.
    const auto type = [](juce::TextEditor& editor, const juce::String& text) {
        editor.setText(text, juce::dontSendNotification);
        editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    };
    const auto allLamps = [&] {
        return !toggle->isEnabled() && !pattern->isEnabled() && !kit->isEnabled()
            && !beat->isEnabled() && !variation->isEnabled() && !level->isEnabled()
            && !reverb->isEnabled() && !toneLow->isEnabled() && !toneHigh->isEnabled();
    };

    // --- no slot: nothing to show, nothing to press ---
    CHECK(!toggle->isVisible());
    CHECK(!pattern->isShowing());
    press();
    CHECK_EQ(heard.calls, 0);

    // --- never told what it may do: every control a lamp, every gesture mute ---
    pane.setSlot(&rows[6]);
    CHECK(toggle->isVisible());
    CHECK(allLamps());
    press();
    choose(*kit, 3);
    type(*level, "90");
    CHECK_EQ(heard.calls, 0);

    // --- a card this app only reads: the same lamps, and the sentence ---
    pane.permissions = [] { return CardPermissions::of("RC-500"); };
    pane.setSlot(&rows[6]);
    CHECK(allLamps());
    press();
    choose(*pattern, 3);
    type(*reverb, "50");
    CHECK_EQ(heard.calls, 0);
    {
        bool sentence = false;
        for (auto* child : pane.getChildren())
            if (auto* label = dynamic_cast<juce::Label*>(child))
                if (label->getText() == juce::String(CardPermissions::writesOnlyTo()))
                    sentence = true;
        CHECK(sentence);
    }

    // --- an RC-5: the slot's own values on the controls ---
    pane.permissions = [] { return CardPermissions::of("RC-5"); };
    pane.setSlot(&rows[6]);
    CHECK(toggle->isEnabled() && toggle->isOn());
    CHECK(pattern->isEnabled() && beat->isEnabled());
    CHECK_EQ(pattern->getSelectedId(), 12); // Rock1 is 11, ids are numbers plus one
    CHECK_EQ(pattern->getText(), "Rock1");
    CHECK_EQ(kit->getText(), "Jazz");
    CHECK_EQ(beat->getText(), "4/4");
    CHECK_EQ(variation->getText(), "A");
    CHECK_EQ(level->getText(), "120");
    CHECK_EQ(reverb->getText(), "0");
    CHECK_EQ(toneLow->getText(), "0");
    CHECK_EQ(toneHigh->getText(), "0");
    // Blank is not on offer: 57 grooves, no 58th item.
    CHECK_EQ(pattern->getNumItems(), 57);
    CHECK_EQ(kit->getNumItems(), 7);
    CHECK_EQ(beat->getNumItems(), 17);
    CHECK_EQ(variation->getNumItems(), 2);

    // The switch reports the opposite of what the slot has, for that slot,
    // and nothing else in the same Edits.
    press();
    CHECK_EQ(heard.calls, 1);
    CHECK_EQ(heard.slot, 7);
    CHECK(heard.last.on.has_value() && !*heard.last.on);
    CHECK_EQ(fieldsSet(heard.last), 1);

    // A choice reports its number — the manual's, not JUCE's id.
    choose(*kit, 6);
    CHECK_EQ(heard.calls, 2);
    CHECK(heard.last.kit.has_value() && *heard.last.kit == 6);
    CHECK_EQ(fieldsSet(heard.last), 1);
    choose(*beat, 6);
    CHECK_EQ(heard.calls, 3);
    CHECK(heard.last.beat.has_value() && *heard.last.beat == 6);
    choose(*variation, 1);
    CHECK_EQ(heard.calls, 4);
    CHECK(heard.last.variation.has_value() && *heard.last.variation == 1);
    choose(*pattern, 56);
    CHECK_EQ(heard.calls, 5);
    CHECK(heard.last.pattern.has_value() && *heard.last.pattern == 56);

    // A typed number reports itself, as the screen reads it; the sign of a
    // tone survives; the unchanged value is nothing.
    type(*level, "200");
    CHECK_EQ(heard.calls, 6);
    CHECK(heard.last.level.has_value() && *heard.last.level == 200);
    CHECK_EQ(fieldsSet(heard.last), 1);
    type(*toneLow, "-10");
    CHECK_EQ(heard.calls, 7);
    CHECK(heard.last.toneLow.has_value() && *heard.last.toneLow == -10);
    type(*toneHigh, "7");
    CHECK_EQ(heard.calls, 8);
    CHECK(heard.last.toneHigh.has_value() && *heard.last.toneHigh == 7);
    type(*reverb, "0"); // what the slot already has
    type(*level, "120");
    type(*toneLow, "");
    type(*toneLow, "-");
    CHECK_EQ(heard.calls, 8);

    // A snapshot landing puts the slot's values back on the controls.
    pane.setSlot(&rows[6]);
    CHECK_EQ(kit->getText(), "Jazz");
    CHECK_EQ(level->getText(), "120");
    CHECK_EQ(toneLow->getText(), "0");

    // --- a slot with a take: BEAT is fixed, and only BEAT ---
    pane.setSlot(&rows[7]);
    CHECK(toggle->isEnabled() && toggle->isOn());
    CHECK(!beat->isEnabled());
    CHECK(pattern->isEnabled() && kit->isEnabled() && level->isEnabled());
    CHECK_EQ(pattern->getText(), "Bossa1");
    choose(*beat, 0); // a disabled box still fires onChange in JUCE: the pane must not pass it on
    CHECK_EQ(heard.calls, 8);
    choose(*kit, 1);
    CHECK_EQ(heard.calls, 9);
    CHECK_EQ(heard.slot, 8);
    // The cost of switching off, said on the tab: a count-in in front of it.
    {
        bool warned = false;
        for (auto* child : pane.getChildren())
            if (auto* label = dynamic_cast<juce::Label*>(child))
                if (label->getText().contains("forgets Bossa1"))
                    warned = true;
        CHECK(warned);
    }

    // --- a factory slot: off, the printed defaults, no cost ---
    pane.setSlot(&rows[8]);
    CHECK(!toggle->isOn());
    CHECK_EQ(pattern->getText(), "SimpleBeat1");
    CHECK(beat->isEnabled());
    press();
    CHECK_EQ(heard.calls, 10);
    CHECK(heard.last.on.has_value() && *heard.last.on);
    {
        bool warned = false;
        for (auto* child : pane.getChildren())
            if (auto* label = dynamic_cast<juce::Label*>(child))
                if (label->getText().contains("forgets"))
                    warned = true;
        CHECK(!warned);
    }

    // --- busy: a lamp until the job is done ---
    pane.setBusy(true);
    CHECK(allLamps());
    press();
    choose(*kit, 4);
    type(*level, "10");
    CHECK_EQ(heard.calls, 10);
    // A greyed CHOICE keeps nothing either: a dropdown left open when the job
    // started still commits its pick, and the tab must snap back to the
    // memory's own value rather than name a groove it does not have. (A
    // greyed field takes no typing at all, so only a choice can drift.)
    CHECK_EQ(kit->getText(), "Studio");
    pane.setBusy(false);
    CHECK(toggle->isEnabled() && kit->isEnabled());
    // Whatever the greying left behind, the tab shows the memory again the
    // moment it can be touched — the controls' own invariant, not the
    // owner's: nothing pushed values in between.
    CHECK_EQ(kit->getText(), "Studio");
    CHECK_EQ(level->getText(), "100");

    // --- a 6/4 slot: PATTERN is the card's number and a lamp; the rest is the player's ---
    pane.setSlot(&rows[9]);
    CHECK(!pattern->isEnabled());
    CHECK_EQ(pattern->getSelectedId(), 0); // no item behind the text: 3 is not SimpleBeat4 here
    CHECK_EQ(pattern->getText(), "3");
    CHECK(captionReads(pane, "PATTERN (list at 6/4 not charted)"));
    CHECK(!captionReads(pane, "PATTERN"));
    CHECK(beat->isEnabled() && kit->isEnabled() && variation->isEnabled() && level->isEnabled()
          && reverb->isEnabled() && toneLow->isEnabled() && toneHigh->isEnabled());
    CHECK_EQ(beat->getText(), "6/4");
    CHECK(captionReads(pane, "BEAT"));
    // The lamp reports nothing and keeps nothing: a pick made past its greyed
    // face snaps back to the card's number.
    choose(*pattern, 12);
    CHECK_EQ(heard.calls, 10);
    CHECK_EQ(pattern->getText(), "3");
    CHECK_EQ(pattern->getSelectedId(), 0);
    // Off here is State alone (no count-in to keep): the switch is offered,
    // and no name is said for the number.
    CHECK(toggle->isEnabled() && toggle->isOn());
    CHECK(!paneSays(pane, "forgets"));
    CHECK(!paneSays(pane, "6/4"));
    press();
    CHECK_EQ(heard.calls, 11);
    CHECK_EQ(heard.slot, 10);
    CHECK(heard.last.on.has_value() && !*heard.last.on);
    CHECK_EQ(fieldsSet(heard.last), 1);
    choose(*kit, 5); // KIT is not beat-relative (#149): still one Edits with one field
    CHECK_EQ(heard.calls, 12);
    CHECK(heard.last.kit.has_value() && *heard.last.kit == 5);
    CHECK_EQ(fieldsSet(heard.last), 1);
    choose(*beat, 2); // and the way out: BEAT itself is still the player's
    CHECK_EQ(heard.calls, 13);
    CHECK(heard.last.beat.has_value() && *heard.last.beat == 2);

    // --- 6/4 with a count-in in front: off would write Blank's number — a lamp, and the line says why ---
    pane.setSlot(&rows[10]);
    CHECK(toggle->isOn());
    CHECK(!toggle->isEnabled());
    CHECK(paneSays(pane, "6/4"));
    CHECK(paneSays(pane, "count-in"));
    CHECK(!paneSays(pane, "forgets")); // no 4/4 name for a 6/4 number
    CHECK(!paneSays(pane, "SimpleBeat4"));
    press();
    CHECK_EQ(heard.calls, 13);
    CHECK(kit->isEnabled() && !pattern->isEnabled()); // only the switch and PATTERN are lamps

    // --- 6/4 holding 57, off: on would write the 4/4 default — a lamp ---
    pane.setSlot(&rows[11]);
    CHECK(!toggle->isOn());
    CHECK(!toggle->isEnabled());
    CHECK(paneSays(pane, "6/4"));
    CHECK(paneSays(pane, "on the pedal"));
    CHECK_EQ(pattern->getText(), "57");
    CHECK(!pattern->isEnabled());
    press();
    CHECK_EQ(heard.calls, 13);

    // --- 6/4 holding 19, off: on is State alone — offered, the number shown ---
    pane.setSlot(&rows[12]);
    CHECK(!toggle->isOn());
    CHECK(toggle->isEnabled());
    CHECK(!paneSays(pane, "6/4"));
    CHECK_EQ(pattern->getText(), "19");
    press();
    CHECK_EQ(heard.calls, 14);
    CHECK_EQ(heard.slot, 13);
    CHECK(heard.last.on.has_value() && *heard.last.on);

    // --- busy at 6/4 and back: the PATTERN lock is re-applied, like BEAT's ---
    pane.setBusy(true);
    CHECK(allLamps());
    choose(*pattern, 12);
    CHECK_EQ(heard.calls, 14);
    CHECK_EQ(pattern->getText(), "19");
    pane.setBusy(false);
    CHECK(toggle->isEnabled() && kit->isEnabled() && beat->isEnabled());
    CHECK(!pattern->isEnabled());
    CHECK_EQ(pattern->getText(), "19");
    CHECK_EQ(pattern->getSelectedId(), 0);
    CHECK(captionReads(pane, "PATTERN (list at 6/4 not charted)"));

    // --- back at 4/4: everything as before, the name on the box again ---
    pane.setSlot(&rows[6]);
    CHECK(pattern->isEnabled());
    CHECK_EQ(pattern->getSelectedId(), 12);
    CHECK_EQ(pattern->getText(), "Rock1");
    CHECK(captionReads(pane, "PATTERN"));
    CHECK(!captionReads(pane, "PATTERN (list at 6/4 not charted)"));
    CHECK(toggle->isEnabled());
    CHECK(!paneSays(pane, "6/4"));
    choose(*pattern, 12);
    CHECK_EQ(heard.calls, 15);
    CHECK(heard.last.pattern.has_value() && *heard.last.pattern == 12);

    // --- a beat the list lacks: shown and said, never thrown on ---
    // Reaching the next line at all is the first check: setSlot paints the
    // caption and the cost line, and a throw there would end the harness.
    pane.setSlot(&rows[13]);
    CHECK(captionReads(pane, "PATTERN (BEAT 17 is not in the manual's list)"));
    CHECK(!pattern->isEnabled());
    CHECK_EQ(pattern->getText(), "3");
    CHECK_EQ(beat->getSelectedId(), 0); // no item for 17: the box names nothing
    CHECK(beat->isEnabled());           // and the way out is still the player's
    CHECK(toggle->isEnabled() && !toggle->isOn()); // State alone: offered
    CHECK(!paneSays(pane, "BEAT 17"));
    press();
    CHECK_EQ(heard.calls, 16);
    CHECK_EQ(heard.slot, 14);
    CHECK(heard.last.on.has_value() && *heard.last.on);
    // Holding 57 there, "on" would need a pattern number: a lamp, and the
    // line tells the truth about the number.
    pane.setSlot(&rows[14]);
    CHECK(!toggle->isEnabled());
    CHECK(paneSays(pane, "BEAT 17 (not in the manual's list)"));
    CHECK(paneSays(pane, "on the pedal"));
    press();
    CHECK_EQ(heard.calls, 16);

    // --- 4/4 with the count-in's own shape in the section: BEAT stays ---
    // State on, Blank, count on: moved away from 4/4 the memory would be
    // stuck, so BEAT is a lamp with the way out on the line; PATTERN and the
    // switch are still the player's at 4/4.
    pane.setSlot(&rows[15]);
    CHECK(!beat->isEnabled());
    CHECK(captionReads(pane, "BEAT (held by the count-in)"));
    CHECK(paneSays(pane, "switch the count-in off first"));
    CHECK(pattern->isEnabled() && kit->isEnabled());
    CHECK(toggle->isEnabled() && !toggle->isOn());
    choose(*beat, 4); // a lamp reports nothing, whoever reaches past its greyed face
    CHECK_EQ(heard.calls, 16);
    CHECK_EQ(beat->getText(), "4/4");

    // --- no slot again: the controls go away ---
    pane.setSlot(nullptr);
    CHECK(!toggle->isVisible() && !pattern->isShowing());

    return testkit::summary("rhythm_pane_harness");
}
