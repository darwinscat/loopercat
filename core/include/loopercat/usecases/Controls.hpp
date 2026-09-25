// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pedal's controls as one feature: what the pedal switch, the two
// footswitch contacts of the STOP/MEMORY SHIFT jack, the expression pedal and
// the eight fixed control changes CC#80..87 DO. On the RC-5 this map is
// global — SETUP > CONTROL on the screen, the <CTL> section of SYSTEM*.RC0 on
// the card (SystemFile.hpp), one number per control.
//
// A number is a position in a list the reference manual prints, and there
// are three lists on the RC-5: one for the switches (PEDAL, CTL1, CTL2), one
// for the expression pedal, one for the control changes. The numbering rule
// — the manual's order, counted from zero — is measured on the two-track
// model (its card carries the manual's marked defaults as 28/36/27/44/53/13,
// exactly the positions this rule gives them) and only READ AS PLAUSIBLE on
// the RC-5: a field pedal's Ctl1=17 and Ctl2=18 decode to MEMORY INC and
// MEMORY DEC, the natural pair for a two-button footswitch, and the switch
// defaults the manual marks land on 2, 8 and 17. What has not been done is
// the look at the pedal's own screen next to the file. Until it has, every
// RC-5 name here is "per the manual, not verified", and one of them is in
// doubt: the same field pedal carries Exp=0, which decodes to TRK LEVEL1,
// while the manual marks MEMORY LEV2 (position 7) as the default.
//
// A number the list does not have is a typed error carrying the number: a
// screen that shows "Unknown" for it would hide exactly the fact that tells
// us the rule is wrong.
//
// Sources: RC-5 Reference Manual, "Settings for the Entire RC-5 (SETUP)",
// CONTROL, pp. 13-14 (PEDAL/CTL1/CTL2 FUNC, EXP FUNC, CC#80-87 FUNC, defaults
// in bold); RC-500 Parameter Guide, "Memory Settings", CTL, pp. 7-8 (PDL1-3
// FUNC, CTL1/2 FUNC, EXP FUNC); fixtures/rc5-system.RC0 (a real settings
// file); the two-track model's card, 2026-09-21 (its measured CTL values).

#pragma once

#include "../DeviceProfile.hpp"
#include "../Error.hpp"
#include "../Rc0.hpp"
#include "../SystemFile.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loopercat::usecases::controls {

// --- the controls ---

// The three lists a control draws its function from.
enum class List { footswitch, expression, cc };

// The twelve controls of the RC-5's <CTL>, in the order the card writes them.
enum class Control { pedal, ctl1, ctl2, exp, cc80, cc81, cc82, cc83, cc84, cc85, cc86, cc87 };

inline constexpr unsigned kControlCount = 12;

struct ControlSpec {
    Control control;
    std::string_view tag;        // the field in <CTL> (fixtures/rc5-system.RC0)
    std::string_view screenName; // the parameter on SETUP > CONTROL (reference manual p. 13)
    List list;
};

inline constexpr std::array<ControlSpec, kControlCount> kControls { {
    { Control::pedal, "Pedal1", "PEDAL FUNC", List::footswitch },
    { Control::ctl1, "Ctl1", "CTL1 FUNC", List::footswitch },
    { Control::ctl2, "Ctl2", "CTL2 FUNC", List::footswitch },
    { Control::exp, "Exp", "EXP FUNC", List::expression },
    { Control::cc80, "Cc80", "CC#80 FUNC", List::cc },
    { Control::cc81, "Cc81", "CC#81 FUNC", List::cc },
    { Control::cc82, "Cc82", "CC#82 FUNC", List::cc },
    { Control::cc83, "Cc83", "CC#83 FUNC", List::cc },
    { Control::cc84, "Cc84", "CC#84 FUNC", List::cc },
    { Control::cc85, "Cc85", "CC#85 FUNC", List::cc },
    { Control::cc86, "Cc86", "CC#86 FUNC", List::cc },
    { Control::cc87, "Cc87", "CC#87 FUNC", List::cc },
} };

inline constexpr const ControlSpec& spec(Control control)
{
    return kControls[static_cast<unsigned>(control)];
}

// --- the function lists ---

// One entry of a list: the number the card stores, the name the screen shows.
struct Function {
    long long number;
    std::string_view name;
};

// RC-5, PEDAL FUNC / CTL1 FUNC / CTL2 FUNC (reference manual p. 13). Marked
// defaults: PEDAL = TRK R/P/S(C, CTL1 = TRK STOP(CLR, CTL2 = MEMORY INC. The
// manual prints "RHYHTM P/S"; the typo is the manual's. Per the manual, not
// verified on the screen.
inline constexpr std::array<Function, 19> kRc5Footswitch { {
    { 0, "TRK REC/PLY" },
    { 1, "TRK R/P/S" },
    { 2, "TRK R/P/S(C" },
    { 3, "TRK MOM R/P" },
    { 4, "TRK PLY/STP" },
    { 5, "TRK P/S(CLR" },
    { 6, "TRK STOP" },
    { 7, "TRK STOP(TAP" },
    { 8, "TRK STOP(CLR" },
    { 9, "TRK STOP(T/C" },
    { 10, "TRK CLEAR" },
    { 11, "TRK UND/RED" },
    { 12, "TRK REVERSE" },
    { 13, "TAP TEMPO" },
    { 14, "RHYTHM P/S" },
    { 15, "RHYTHM PLAY" },
    { 16, "RHYTHM STOP" },
    { 17, "MEMORY INC" },
    { 18, "MEMORY DEC" },
} };

// RC-5, EXP FUNC (p. 13). Marked default: MEMORY LEV2. Per the manual, not
// verified on the screen — and in doubt, see the header.
inline constexpr std::array<Function, 8> kRc5Expression { {
    { 0, "TRK LEVEL1" },
    { 1, "TRK LEVEL2" },
    { 2, "TEMPO UP" },
    { 3, "TEMPO DOWN" },
    { 4, "RHYTHM LEV1" },
    { 5, "RHYTHM LEV2" },
    { 6, "MEMORY LEV1" },
    { 7, "MEMORY LEV2" },
} };

// RC-5, CC#80 FUNC .. CC#87 FUNC (p. 14). The manual marks no default here; a
// field pedal carries 0 on all eight (fixtures/rc5-system.RC0), which reads
// as OFF. Two entries are printed "TRK REVERSE": the first toggles reverse
// play, the second controls the REVERSE setting of the memory — the names are
// the manual's, both. "RHY PART1-4" is one printed row read as four entries,
// one per part, in that order; an assumption until the screen is read. Per
// the manual, not verified on the screen.
inline constexpr std::array<Function, 43> kRc5Cc { {
    { 0, "OFF" },
    { 1, "TRK PLY/STP" },
    { 2, "TRK CLEAR" },
    { 3, "TRK UND/RED" },
    { 4, "TRK REVERSE" },
    { 5, "TRK LEVEL1" },
    { 6, "TRK LEVEL2" },
    { 7, "TAP TEMPO" },
    { 8, "TEMPO UP" },
    { 9, "TEMPO DOWN" },
    { 10, "RHYTHM P/S" },
    { 11, "RHYTHM PLAY" },
    { 12, "RHYTHM STOP" },
    { 13, "RHYTHM LEV1" },
    { 14, "RHYTHM LEV2" },
    { 15, "MEMORY INC" },
    { 16, "MEMORY DEC" },
    { 17, "MEMORY LEV1" },
    { 18, "MEMORY LEV2" },
    { 19, "TRK REVERSE" },
    { 20, "TRK 1SHOT" },
    { 21, "REC ACTION" },
    { 22, "DUB MODE" },
    { 23, "AUTO REC" },
    { 24, "TRK START" },
    { 25, "TRK STOP" },
    { 26, "FADE TIME" },
    { 27, "REVERB" },
    { 28, "PATTERN" },
    { 29, "VARIATION" },
    { 30, "VAR.CHANGE" },
    { 31, "KIT" },
    { 32, "RHY START" },
    { 33, "RHY STOP" },
    { 34, "REC COUNT" },
    { 35, "PLAY COUNT" },
    { 36, "RHY FILL" },
    { 37, "RHY PART1" },
    { 38, "RHY PART2" },
    { 39, "RHY PART3" },
    { 40, "RHY PART4" },
    { 41, "TONE LOW" },
    { 42, "TONE HIGH" },
} };

// The two-track model, PDL1-3 FUNC / CTL1 FUNC / CTL2 FUNC (parameter guide
// pp. 7-8). A row printed "T1, T2 X" is two entries, T1 first. MEASURED: its
// card carries the manual's marked defaults — PDL1 = CUR REC/PLY, PDL2 =
// CUR STP(CLR, PDL3 = TRK SELECT, CTL1 = LOOP FX, CTL2 = MEMORY INC — as
// 28, 36, 27, 44 and 53, the positions this list gives them. Nothing on that
// model is open (DeviceProfile.hpp); the list is here so the day it opens the
// numbers already have their names.
inline constexpr std::array<Function, 58> kRc500Switch { {
    { 0, "OFF" },
    { 1, "T1 REC/PLY" },
    { 2, "T2 REC/PLY" },
    { 3, "T1 R/P/S" },
    { 4, "T2 R/P/S" },
    { 5, "T1 R/P/S(C" },
    { 6, "T2 R/P/S(C" },
    { 7, "T1 MOM R/P" },
    { 8, "T2 MOM R/P" },
    { 9, "T1 PLY/STP" },
    { 10, "T2 PLY/STP" },
    { 11, "T1 P/S(CLR" },
    { 12, "T2 P/S(CLR" },
    { 13, "T1 STOP" },
    { 14, "T2 STOP" },
    { 15, "T1 STOP(TAP" },
    { 16, "T2 STOP(TAP" },
    { 17, "T1 STOP(CLR" },
    { 18, "T2 STOP(CLR" },
    { 19, "T1 STOP(T/C" },
    { 20, "T2 STOP(T/C" },
    { 21, "T1 CLEAR" },
    { 22, "T2 CLEAR" },
    { 23, "T1 UND/RED" },
    { 24, "T2 UND/RED" },
    { 25, "T1 REVERSE" },
    { 26, "T2 REVERSE" },
    { 27, "TRK SELECT" },
    { 28, "CUR REC/PLY" },
    { 29, "CUR R/P/S" },
    { 30, "CUR R/P/S(C" },
    { 31, "CUR MOM R/P" },
    { 32, "CUR PLY/STP" },
    { 33, "CUR P/S(CLR" },
    { 34, "CUR STOP" },
    { 35, "CUR STP(TAP" },
    { 36, "CUR STP(CLR" },
    { 37, "CUR STP(T/C" },
    { 38, "CUR CLEAR" },
    { 39, "CUR UND/RED" },
    { 40, "CUR REVERSE" },
    { 41, "UNDO/REDO" },
    { 42, "ALL START" },
    { 43, "TAP TEMPO" },
    { 44, "LOOP FX" },
    { 45, "TR1 FX" },
    { 46, "TR2 FX" },
    { 47, "CUR TR FX" },
    { 48, "FX INC" },
    { 49, "FX DEC" },
    { 50, "RHYTHM P/S" },
    { 51, "RHYTHM PLAY" },
    { 52, "RHYTHM STOP" },
    { 53, "MEMORY INC" },
    { 54, "MEMORY DEC" },
    { 55, "MIC MUTE" },
    { 56, "EXTENT INC" },
    { 57, "EXTENT DEC" },
} };

// The two-track model, EXP FUNC (p. 8). MEASURED at its end: the card carries
// the marked default MEMORY LEV2 as 13. The order inside the two "T1, T2"
// rows (1..4) follows the switch list's rule and is not separately measured.
inline constexpr std::array<Function, 14> kRc500Expression { {
    { 0, "OFF" },
    { 1, "T1 LEVEL1" },
    { 2, "T2 LEVEL1" },
    { 3, "T1 LEVEL2" },
    { 4, "T2 LEVEL2" },
    { 5, "CUR LEVEL 1" },
    { 6, "CUR LEVEL 2" },
    { 7, "TEMPO UP" },
    { 8, "TEMPO DOWN" },
    { 9, "FX CONTROL" },
    { 10, "RHYTHM LEV1" },
    { 11, "RHYTHM LEV2" },
    { 12, "MEMORY LEV1" },
    { 13, "MEMORY LEV2" },
} };

// A model's lists, DeviceProfile-style: data, one entry per model. A list a
// model does not have is empty — the two-track model has no CC#80..87 list,
// its MIDI controls are the per-memory ASSIGN1..8 (parameter guide p. 9).
struct FunctionLists {
    std::span<const Function> footswitch;
    std::span<const Function> expression;
    std::span<const Function> cc;
};

inline constexpr FunctionLists kRc5Lists { kRc5Footswitch, kRc5Expression, kRc5Cc };
inline constexpr FunctionLists kRc500Lists { kRc500Switch, kRc500Expression, {} };

inline const FunctionLists& listsOf(const profile::DeviceProfile& family)
{
    if (&family == &profile::kRc5)
        return kRc5Lists;
    if (&family == &profile::kRc500)
        return kRc500Lists;
    throw Error("no control lists on record for the \"" + std::string(family.familyName)
                + "\" model");
}

inline std::string_view listName(List list)
{
    switch (list) {
    case List::footswitch: return "footswitch list";
    case List::expression: return "expression pedal list";
    case List::cc: return "CC#80-87 list";
    }
    return "?"; // an enumerator added without a name here
}

// The list a control draws from, for a model — refused by name when the
// model has none such.
inline std::span<const Function> functions(const profile::DeviceProfile& family, Control control)
{
    const FunctionLists& lists = listsOf(family);
    const List which = spec(control).list;
    const std::span<const Function> list = which == List::footswitch ? lists.footswitch
                                         : which == List::expression ? lists.expression
                                                                     : lists.cc;
    if (list.empty())
        throw Error("the \"" + std::string(family.familyName) + "\" model has no "
                    + std::string(listName(which)) + " on record");
    return list;
}

// The name a number stands for in a list — or a typed error that carries the
// number and the range, never a placeholder name.
inline std::string_view functionName(std::span<const Function> list, long long number,
                                     std::string_view what)
{
    for (const Function& function : list)
        if (function.number == number)
            return function.name;
    throw Error("no function numbered " + std::to_string(number) + " for " + std::string(what)
                + ": the list runs 0.." + std::to_string(static_cast<long long>(list.size()) - 1));
}

// --- reading and writing the card ---

// What one control is set to: the number on the card and its name.
struct Assignment {
    Control control;
    long long number;
    std::string_view name;

    bool operator==(const Assignment&) const = default;
};

namespace detail {

    // A settings file of a model this app reads — the same gate as every
    // other read (DeviceProfile.hpp): another model's file is refused in the
    // words the guard has always used, before a field is looked at.
    inline const profile::DeviceProfile& familyForReading(std::string_view systemText)
    {
        sysfile::assertSystemFile(systemText);
        const profile::DeviceProfile& family = rc0::profileOf(systemText);
        profile::require(family, profile::Operation::read);
        return family;
    }

} // namespace detail

inline Assignment read(std::string_view systemText, Control control)
{
    const profile::DeviceProfile& family = detail::familyForReading(systemText);
    const ControlSpec& control_ = spec(control);
    const long long number = sysfile::field(systemText, sysfile::kSectionCtl, control_.tag);
    const std::string_view name =
        functionName(functions(family, control), number, control_.screenName);
    return { control, number, name };
}

inline std::vector<Assignment> readAll(std::string_view systemText)
{
    std::vector<Assignment> out;
    out.reserve(kControlCount);
    for (const ControlSpec& control : kControls)
        out.push_back(read(systemText, control.control));
    return out;
}

// Assign a function to one control: the number must be one the control's
// list has for this model, and the model must be open for it. The one field
// changes; every other byte of the file — MemoryNumber, the other controls,
// the MIDI block, the trailer — is reproduced exactly (rc0::setSectionField).
inline std::string set(std::string_view systemText, Control control, long long number)
{
    sysfile::assertSystemFile(systemText);
    const profile::DeviceProfile& family = rc0::profileOf(systemText);
    profile::require(family, profile::Operation::setControls);
    const ControlSpec& control_ = spec(control);
    // A number the control's list lacks is refused here, with the number.
    functionName(functions(family, control), number, control_.screenName);
    return sysfile::setField(systemText, sysfile::kSectionCtl, control_.tag, number);
}

} // namespace loopercat::usecases::controls
