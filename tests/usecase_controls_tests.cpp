// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pedal's controls (SETUP > CONTROL) as a use case over the SYSTEM file,
// from the theory of the card and the manuals rather than from the code:
//
//   - the twelve controls are the twelve fields of <CTL>, in the card's order
//   - a number is a position in the manual's list: every list runs 0..n-1
//     without a gap, and the manual's marked defaults land where the rule
//     puts them — on the two-track model, where the card was measured, on
//     the positions its card really carries
//   - a real RC-5 settings file decodes to names, every field of it
//   - a number no list has is refused with the number in the message, never
//     shown as a placeholder
//   - a write changes the one field's bytes and nothing else: not
//     MemoryNumber, not another control, not the MIDI block, not the trailer
//   - a number the control's list lacks, a footswitch number on the
//     expression pedal, a CC-only number on a footswitch: refused, file
//     untouched
//   - another model's settings file is refused in the words the guard has
//     always used, for reading and for writing; a memory file is not a
//     settings file

#include "support.hpp"

#include <loopercat/DeviceProfile.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/SystemFile.hpp>
#include <loopercat/usecases/Controls.hpp>

#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace loopercat;
using namespace loopercat::usecases;

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

std::string withRoot(const std::string& text, const std::string& name)
{
    const std::string rc5 = "name=\"RC-5\"";
    const auto at = text.find(rc5);
    if (at == std::string::npos)
        throw Error("test fixture: root name not found");
    return std::string(text).replace(at, rc5.size(), "name=\"" + name + "\"");
}

// The lines of a text — the unit of "nothing else moved".
std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    while (true) {
        const auto nl = text.find('\n', from);
        if (nl == std::string::npos) {
            out.push_back(text.substr(from));
            return out;
        }
        out.push_back(text.substr(from, nl - from));
        from = nl + 1;
    }
}

// Every list runs 0..n-1 in order, with a name on every entry.
void checkContiguous(std::span<const controls::Function> list, const char* what)
{
    long long expected = 0;
    for (const auto& function : list) {
        CHECK_EQ(function.number, expected);
        CHECK(!function.name.empty());
        ++expected;
    }
    CHECK_EQ(static_cast<long long>(list.size()), expected);
    (void) what;
}

} // namespace

int main()
{
    const std::string system = readFixture(LOOPERCAT_RC5_SYSTEM);
    const std::string card = readFixture(LOOPERCAT_RC5_CARD);

    // --- the controls are <CTL>'s fields, in the card's order ---

    CHECK_EQ(controls::kControls.size(), static_cast<std::size_t>(controls::kControlCount));
    {
        // The fixture's <CTL> block, one tag per line, in the order it holds them.
        const auto open = system.find("<CTL>\n");
        const auto close = system.find("</CTL>", open);
        CHECK(open != std::string::npos && close != std::string::npos);
        const auto block = lines(system.substr(open + 6, close - open - 6));
        std::size_t i = 0;
        for (const auto& line : block) {
            if (line.empty())
                continue;
            CHECK(i < controls::kControlCount);
            if (i < controls::kControlCount) {
                const std::string tag(controls::kControls[i].tag);
                CHECK_EQ(line.substr(0, tag.size() + 2), "\t<" + tag);
            }
            ++i;
        }
        CHECK_EQ(i, static_cast<std::size_t>(controls::kControlCount));
    }
    for (unsigned i = 0; i < controls::kControlCount; ++i)
        CHECK(controls::spec(static_cast<controls::Control>(i)).control
              == static_cast<controls::Control>(i));
    CHECK(controls::spec(controls::Control::pedal).list == controls::List::footswitch);
    CHECK(controls::spec(controls::Control::exp).list == controls::List::expression);
    CHECK(controls::spec(controls::Control::cc87).list == controls::List::cc);

    // --- the lists: positions from zero, no gaps, the marked defaults in place ---

    checkContiguous(controls::kRc5Footswitch, "RC-5 footswitch");
    checkContiguous(controls::kRc5Expression, "RC-5 expression");
    checkContiguous(controls::kRc5Cc, "RC-5 CC");
    checkContiguous(controls::kRc500Switch, "two-track switch");
    checkContiguous(controls::kRc500Expression, "two-track expression");
    CHECK_EQ(controls::kRc5Footswitch.size(), 19u);
    CHECK_EQ(controls::kRc5Expression.size(), 8u);
    CHECK_EQ(controls::kRc5Cc.size(), 43u);
    CHECK_EQ(controls::kRc500Switch.size(), 58u);
    CHECK_EQ(controls::kRc500Expression.size(), 14u);

    // Names are the screen's, one per position — except the CC list, where
    // the manual prints "TRK REVERSE" twice, and that is pinned as the one
    // duplicate so a third would be noticed.
    {
        const auto unique = [](std::span<const controls::Function> list) {
            std::set<std::string_view> names;
            for (const auto& f : list)
                names.insert(f.name);
            return names.size();
        };
        CHECK_EQ(unique(controls::kRc5Footswitch), controls::kRc5Footswitch.size());
        CHECK_EQ(unique(controls::kRc5Expression), controls::kRc5Expression.size());
        CHECK_EQ(unique(controls::kRc5Cc), controls::kRc5Cc.size() - 1);
        CHECK_EQ(unique(controls::kRc500Switch), controls::kRc500Switch.size());
        CHECK_EQ(unique(controls::kRc500Expression), controls::kRc500Expression.size());
    }

    // The manual's marked defaults on the RC-5 (bold on pp. 13-14), per the
    // manual, not verified on the screen: PEDAL = TRK R/P/S(C, CTL1 =
    // TRK STOP(CLR, CTL2 = MEMORY INC, EXP = MEMORY LEV2; the CC list marks
    // none.
    CHECK_EQ(controls::functionName(controls::kRc5Footswitch, 2, "PEDAL FUNC"), "TRK R/P/S(C");
    CHECK_EQ(controls::functionName(controls::kRc5Footswitch, 8, "CTL1 FUNC"), "TRK STOP(CLR");
    CHECK_EQ(controls::functionName(controls::kRc5Footswitch, 17, "CTL2 FUNC"), "MEMORY INC");
    CHECK_EQ(controls::functionName(controls::kRc5Expression, 7, "EXP FUNC"), "MEMORY LEV2");
    CHECK_EQ(controls::functionName(controls::kRc5Cc, 0, "CC#80 FUNC"), "OFF");
    CHECK_EQ(controls::functionName(controls::kRc5Cc, 42, "CC#87 FUNC"), "TONE HIGH");

    // MEASURED on the two-track model: its card's <CTL> holds 28/36/27/44/53
    // for PDL1-3, CTL1, CTL2 and 13 for EXP — and the manual marks exactly
    // these as the defaults. The numbering rule, from a card, not a guess.
    CHECK_EQ(controls::functionName(controls::kRc500Switch, 28, "PDL1 FUNC"), "CUR REC/PLY");
    CHECK_EQ(controls::functionName(controls::kRc500Switch, 36, "PDL2 FUNC"), "CUR STP(CLR");
    CHECK_EQ(controls::functionName(controls::kRc500Switch, 27, "PDL3 FUNC"), "TRK SELECT");
    CHECK_EQ(controls::functionName(controls::kRc500Switch, 44, "CTL1 FUNC"), "LOOP FX");
    CHECK_EQ(controls::functionName(controls::kRc500Switch, 53, "CTL2 FUNC"), "MEMORY INC");
    CHECK_EQ(controls::functionName(controls::kRc500Expression, 13, "EXP FUNC"), "MEMORY LEV2");

    // A number no list has: refused with the number and the range in it.
    CHECK_THROWS(controls::functionName(controls::kRc5Footswitch, 19, "PEDAL FUNC"),
                 "no function numbered 19 for PEDAL FUNC: the list runs 0..18");
    CHECK_THROWS(controls::functionName(controls::kRc5Footswitch, -1, "PEDAL FUNC"),
                 "no function numbered -1");
    CHECK_THROWS(controls::functionName(controls::kRc5Expression, 8, "EXP FUNC"), "runs 0..7");
    CHECK_THROWS(controls::functionName(controls::kRc5Cc, 43, "CC#80 FUNC"), "runs 0..42");
    CHECK_THROWS(controls::functionName(controls::kRc5Cc, 1000000, "CC#80 FUNC"), "1000000");

    // Which list a control draws from, per model. The two-track model has no
    // CC#80-87 list at all — its MIDI controls are per-memory assigns.
    CHECK(controls::functions(profile::kRc5, controls::Control::ctl2).data()
          == controls::kRc5Footswitch.data());
    CHECK(controls::functions(profile::kRc5, controls::Control::exp).data()
          == controls::kRc5Expression.data());
    CHECK(controls::functions(profile::kRc5, controls::Control::cc80).data()
          == controls::kRc5Cc.data());
    CHECK(controls::functions(profile::kRc500, controls::Control::pedal).data()
          == controls::kRc500Switch.data());
    CHECK_THROWS(controls::functions(profile::kRc500, controls::Control::cc80),
                 "has no CC#80-87 list on record");

    // --- a real settings file, decoded ---

    // fixtures/rc5-system.RC0 (an RC-5, fw 1.10): Pedal1=4, Ctl1=17, Ctl2=18,
    // Exp=0, Cc80..87=0. Under the manual's order that is a pedal set to
    // play/stop, a two-button footswitch on memory up/down, and nothing on
    // MIDI — plausible, and to be read off the screen. Exp=0 decodes to
    // TRK LEVEL1 where the manual marks MEMORY LEV2 as the default: the one
    // reading in doubt.
    {
        const auto pedal = controls::read(system, controls::Control::pedal);
        CHECK_EQ(pedal.number, 4);
        CHECK_EQ(pedal.name, "TRK PLY/STP");
        CHECK_EQ(controls::read(system, controls::Control::ctl1).name, "MEMORY INC");
        CHECK_EQ(controls::read(system, controls::Control::ctl2).name, "MEMORY DEC");
        CHECK_EQ(controls::read(system, controls::Control::exp).name, "TRK LEVEL1");
        for (const auto control : { controls::Control::cc80, controls::Control::cc87 })
            CHECK_EQ(controls::read(system, control).name, "OFF");

        const auto all = controls::readAll(system);
        CHECK_EQ(all.size(), static_cast<std::size_t>(controls::kControlCount));
        for (unsigned i = 0; i < all.size(); ++i) {
            CHECK(all[i].control == static_cast<controls::Control>(i));
            CHECK(all[i] == controls::read(system, all[i].control));
            CHECK_EQ(all[i].number, sysfile::field(system, sysfile::kSectionCtl,
                                                   controls::kControls[i].tag));
        }
    }

    // --- writing: one field's bytes, and nothing else ---

    {
        // CTL1 from MEMORY INC (17) to TRK STOP(CLR (8), the manual's default.
        const std::string edited = controls::set(system, controls::Control::ctl1, 8);
        // Exactly the text with that one field's bytes swapped.
        const std::string was = "<Ctl1>17</Ctl1>";
        const auto at = system.find(was);
        CHECK(at != std::string::npos);
        CHECK(edited == system.substr(0, at) + "<Ctl1>8</Ctl1>" + system.substr(at + was.size()));
        // Said again in the units that matter to the pedal.
        CHECK_EQ(sysfile::currentMemory(edited), sysfile::currentMemory(system));
        CHECK_EQ(*rc0::tailMarker(edited), *rc0::tailMarker(system));
        CHECK_EQ(controls::read(edited, controls::Control::ctl1).name, "TRK STOP(CLR");
        CHECK(controls::read(edited, controls::Control::ctl2)
              == controls::read(system, controls::Control::ctl2));
        sysfile::assertSystemFile(edited);
        // Line by line: one line differs, and it is <Ctl1>'s.
        const auto before = lines(system), after = lines(edited);
        CHECK_EQ(before.size(), after.size());
        int differing = 0;
        for (std::size_t i = 0; i < before.size() && i < after.size(); ++i)
            if (before[i] != after[i]) {
                ++differing;
                CHECK_EQ(before[i], "\t<Ctl1>17</Ctl1>");
                CHECK_EQ(after[i], "\t<Ctl1>8</Ctl1>");
            }
        CHECK_EQ(differing, 1);
    }

    // Writing a control the value it holds reproduces the file, byte for byte.
    for (const auto& control : controls::kControls)
        CHECK(controls::set(system, control.control, controls::read(system, control.control).number)
              == system);

    // Every control, every number its list has: written, read back, and the
    // rest of the file untouched — every time.
    for (const auto& control : controls::kControls) {
        for (const auto& function : controls::functions(profile::kRc5, control.control)) {
            const std::string edited = controls::set(system, control.control, function.number);
            const auto got = controls::read(edited, control.control);
            CHECK_EQ(got.number, function.number);
            CHECK_EQ(got.name, function.name);
            CHECK_EQ(sysfile::currentMemory(edited), sysfile::currentMemory(system));
            CHECK_EQ(rc0::splitFile(edited).tail, rc0::splitFile(system).tail);
            // Back to the original number gives the original bytes.
            CHECK(controls::set(edited, control.control,
                                controls::read(system, control.control).number)
                  == system);
        }
    }

    // A number the control's list lacks is refused, the file untouched — the
    // list's edge, a footswitch number on the expression pedal, a CC-only
    // number on a footswitch, nonsense.
    CHECK_THROWS(controls::set(system, controls::Control::pedal, 19),
                 "no function numbered 19 for PEDAL FUNC");
    CHECK_THROWS(controls::set(system, controls::Control::exp, 8),
                 "for EXP FUNC: the list runs 0..7");
    CHECK_THROWS(controls::set(system, controls::Control::exp, 17),
                 "no function numbered 17 for EXP FUNC");
    CHECK_THROWS(controls::set(system, controls::Control::ctl1, 30),
                 "no function numbered 30 for CTL1 FUNC");
    CHECK_THROWS(controls::set(system, controls::Control::cc85, 43),
                 "for CC#85 FUNC: the list runs 0..42");
    CHECK_THROWS(controls::set(system, controls::Control::cc85, -1), "no function numbered -1");

    // --- whose file it is ---

    // Another model's settings file: the same words the guard has always used,
    // for reading and for writing. The two-track model is known, and its
    // settings file may be read — but the control table here is the RC-5's,
    // so its controls are not on record and are refused as such; an unknown
    // model is unknown; both stop before a field is read.
    {
        const std::string twoTrack = withRoot(system, "RC-500");
        const std::string notOnRecord =
            "the controls of the \"RC-500\" model are not on record \xe2\x80\x94 LooperCat only"
            " speaks RC-5";
        CHECK_THROWS(controls::read(twoTrack, controls::Control::pedal), notOnRecord);
        CHECK_THROWS(controls::readAll(twoTrack), notOnRecord);
        CHECK_THROWS(controls::set(twoTrack, controls::Control::pedal, 28),
                     "set controls refused on an \"RC-500\" card \xe2\x80\x94 LooperCat only speaks"
                     " RC-5");
        const std::string unknown = withRoot(system, "RC-505");
        CHECK_THROWS(controls::read(unknown, controls::Control::pedal),
                     "this is an \"RC-505\" card, not an RC-5");
        CHECK_THROWS(controls::set(unknown, controls::Control::pedal, 2),
                     "this is an \"RC-505\" card, not an RC-5");
    }
    // A memory file is not a settings file, whatever it is asked.
    CHECK_THROWS(controls::read(card, controls::Control::pedal),
                 "memory file, not a SYSTEM file");
    CHECK_THROWS(controls::set(card, controls::Control::pedal, 2),
                 "memory file, not a SYSTEM file");
    // Nor is a settings file missing its <CTL>.
    {
        std::string noCtl = system;
        const auto from = noCtl.find("<CTL>");
        const auto to = noCtl.find("</CTL>") + std::string("</CTL>").size();
        noCtl.erase(from, to - from);
        CHECK_THROWS(controls::read(noCtl, controls::Control::pedal), "missing <CTL> section");
    }

    return testkit::summary("usecase_controls");
}
