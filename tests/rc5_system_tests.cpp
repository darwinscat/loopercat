// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// SYSTEM*.RC0 against the real thing: fixtures/rc5-system.RC0 is a file taken
// off an RC-5 (fw 1.10), 717 bytes, trailer included.
//
// The suite is written from what this file IS, and from the two ways a
// reader can be wrong about it. First, telling it apart from a memory file:
// both carry the same family header and the same trailer, so "it parsed"
// must never be mistaken for "it is the file I meant" — a mutation aimed at
// the wrong one would be a pedal rewritten by accident. Second, the write
// discipline: MemoryNumber is the pedal's own record of the memory it has
// selected, and an edit of a control must not disturb it, nor any other byte.

#include "support.hpp"

#include <loopercat/Rc0.hpp>
#include <loopercat/SystemFile.hpp>

#include <fstream>
#include <sstream>
#include <string>

using namespace loopercat;

namespace {

std::string readFixture(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error(std::string("cannot open fixture: ") + path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// The fixture with one section removed, doubled or left unclosed.
std::string without(const std::string& text, std::string_view section)
{
    const std::string open = "<" + std::string(section) + ">";
    const std::string close = "</" + std::string(section) + ">";
    const auto from = text.find(open);
    const auto to = text.find(close);
    return text.substr(0, from) + text.substr(to + close.size());
}

std::string twice(const std::string& text, std::string_view section)
{
    const std::string open = "<" + std::string(section) + ">";
    const std::string close = "</" + std::string(section) + ">";
    const auto from = text.find(open);
    const auto to = text.find(close) + close.size();
    return text.substr(0, to) + text.substr(from, to - from) + text.substr(to);
}

} // namespace

int main()
{
    const std::string system = readFixture(LOOPERCAT_RC5_SYSTEM);
    const std::string card = readFixture(LOOPERCAT_RC5_CARD);

    // --- it is a SYSTEM file, and it is not a memory file ---

    sysfile::assertSystemFile(system);
    CHECK_THROWS(sysfile::assertSystemFile(card), "memory file");
    CHECK_THROWS(rc0::assertMemoryFile(system), "<mem");

    // The family guard is the same one: an unknown model's settings are
    // refused by name before anything reads a field out of them. A model the
    // table knows but has not opened (DeviceProfile.hpp) is a sound settings
    // file of its own — it is commands::readSystem that refuses to read it,
    // in the same words (commands_tests, the two-track card).
    {
        const auto withRoot = [&system](const std::string& name) {
            std::string text = system;
            const auto at = text.find("name=\"RC-5\"");
            CHECK(at != std::string::npos);
            return text.replace(at, std::string("name=\"RC-5\"").size(), "name=\"" + name + "\"");
        };
        CHECK_THROWS(sysfile::assertSystemFile(withRoot("RC-505")), "LooperCat only speaks RC-5");
        CHECK_THROWS(sysfile::assertSystemFile(withRoot("RC-505")), "\"RC-505\" card");
        sysfile::assertSystemFile(withRoot("RC-500")); // known: sound, and not ours to read
    }

    // --- structure: each section, exactly once, closed ---

    CHECK_THROWS(sysfile::assertSystemFile(without(system, sysfile::kSectionMidi)),
                 "missing <MIDI> section");
    CHECK_THROWS(sysfile::assertSystemFile(without(system, sysfile::kSectionCtl)),
                 "missing <CTL> section");
    CHECK_THROWS(sysfile::assertSystemFile(twice(system, sysfile::kSectionCtl)),
                 "occurs more than once");
    {
        std::string truncated = system;
        const auto at = truncated.find("</CTL>");
        truncated.erase(at, std::string("</CTL>").size());
        CHECK_THROWS(sysfile::assertSystemFile(truncated), "unterminated <CTL> section");
    }

    // --- the trailer is the memory files' trailer ---

    CHECK(rc0::tailMarker(system).has_value());
    CHECK_EQ(rc0::splitFile(system).tail.size(), static_cast<std::size_t>(5));
    const std::uint32_t generation = *rc0::tailMarker(system);
    CHECK(generation > 0);
    CHECK(rc0::setTailGeneration(system, generation) == system);
    {
        const std::string bumped = rc0::setTailGeneration(system, generation + 1);
        CHECK_EQ(bumped.size(), system.size());
        CHECK_EQ(bumped.substr(0, bumped.size() - 4), system.substr(0, system.size() - 4));
        CHECK_EQ(*rc0::tailMarker(bumped), generation + 1);
    }

    // --- the fields, as the pedal left them ---

    CHECK_EQ(sysfile::currentMemory(system), 2);
    CHECK_EQ(sysfile::field(system, sysfile::kSectionSetup, "Contrast"), 4);
    CHECK_EQ(sysfile::field(system, sysfile::kSectionMidi, "RxNoteCh"), 9);
    CHECK_EQ(sysfile::field(system, sysfile::kSectionMidi, "TxCh"), 16);
    CHECK_EQ(sysfile::field(system, sysfile::kSectionCtl, "Pedal1"), 4);
    CHECK_EQ(sysfile::field(system, sysfile::kSectionCtl, "Ctl1"), 17);
    CHECK_EQ(sysfile::field(system, sysfile::kSectionCtl, "Cc80"), 0);
    CHECK_EQ(sysfile::field(system, sysfile::kSectionCtl, "Cc87"), 0);

    // A field belongs to its section and is invisible from another.
    CHECK_THROWS(sysfile::field(system, sysfile::kSectionMidi, "Contrast"), "occurs 0 times");
    CHECK_THROWS(sysfile::field(system, sysfile::kSectionCtl, "MemoryNumber"), "occurs 0 times");
    CHECK_THROWS(sysfile::field(system, sysfile::kSectionCtl, "Cc88"), "occurs 0 times");

    // --- writing one control leaves the rest of the pedal alone ---

    // Writing a field the value it already holds reproduces the file.
    CHECK(sysfile::setField(system, sysfile::kSectionCtl, "Ctl1",
                            sysfile::field(system, sysfile::kSectionCtl, "Ctl1"))
          == system);

    {
        const long long before = sysfile::field(system, sysfile::kSectionCtl, "Ctl2");
        const std::string edited = sysfile::setField(system, sysfile::kSectionCtl, "Ctl2", 25);
        CHECK_EQ(sysfile::field(edited, sysfile::kSectionCtl, "Ctl2"), 25);
        // The pedal's own business is untouched: the selected memory, the
        // other controls, the MIDI block and the write counter.
        CHECK_EQ(sysfile::currentMemory(edited), sysfile::currentMemory(system));
        CHECK_EQ(sysfile::field(edited, sysfile::kSectionCtl, "Ctl1"),
                 sysfile::field(system, sysfile::kSectionCtl, "Ctl1"));
        CHECK_EQ(sysfile::field(edited, sysfile::kSectionMidi, "RxCtlCh"),
                 sysfile::field(system, sysfile::kSectionMidi, "RxCtlCh"));
        CHECK_EQ(*rc0::tailMarker(edited), *rc0::tailMarker(system));
        // And nothing outside that one field's bytes moved.
        const std::string was = "<Ctl2>" + std::to_string(before) + "</Ctl2>";
        const auto at = system.find(was);
        CHECK(at != std::string::npos);
        CHECK_EQ(edited, system.substr(0, at) + "<Ctl2>25</Ctl2>"
                             + system.substr(at + was.size()));
        // The edited file is still a SYSTEM file.
        sysfile::assertSystemFile(edited);
    }

    // A field that is not there is a typed error, not a silent no-op.
    CHECK_THROWS(sysfile::setField(system, sysfile::kSectionCtl, "Cc99", 1), "occurs 0 times");
    CHECK_THROWS(sysfile::setField(system, "PREF", "Ctl1", 1), "missing <PREF> section");

    return testkit::summary("rc5_system");
}
