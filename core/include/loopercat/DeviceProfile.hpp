// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The RC family as a table: what each model calls itself in its card's root
// element, how it is addressed over USB and MIDI, how many tracks one of its
// memories holds, what a never-touched memory of it looks like, and which
// operations this app has opened for it.
//
// A profile is data, not a class. Every model seen so far differs from the
// RC-5 in numbers and strings — a name, an id, a track count — never in
// behaviour: the same card layout, the same field names, the same
// `Measure = MeasLen + 7`, the same boot-indexing arithmetic (hardware,
// 2026-09). The day a model does something differently is the day a profile
// grows behaviour; until then a struct of constants is the honest shape, and
// every constant here names the measurement it came from.
//
// The operation set is the guard's other half. Knowing a model is not the
// same as being allowed to touch its card: the two-track model is known here
// and only read, because two of our operations would corrupt such a card
// silently — a swap carries only the first track's audio across, a clear
// writes a one-track factory body. On this table they are not untested on
// it; they are unreachable.

#pragma once

#include "Error.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace loopercat::profile {

// Everything the app does to a card, by name. `read` is the browser's view:
// the memory files, the slot table, the doctor's report. `pull` copies audio
// off the card and changes nothing on it; every other one writes.
enum class Operation : unsigned {
    read,
    rename,
    setOneShot,
    setTempo,
    setCountIn,
    push,
    pull,
    trim,
    downmix,
    normalize,
    clear,
    restore,
    swap,
    setControls, // the pedal's own settings: what its footswitches and CC#80..87 do
};

inline constexpr unsigned kOperationCount = 14;

inline constexpr std::string_view operationName(Operation op)
{
    switch (op) {
    case Operation::read: return "read";
    case Operation::rename: return "rename";
    case Operation::setOneShot: return "set one-shot";
    case Operation::setTempo: return "set tempo";
    case Operation::setCountIn: return "set count-in";
    case Operation::push: return "push";
    case Operation::pull: return "pull";
    case Operation::trim: return "trim";
    case Operation::downmix: return "downmix";
    case Operation::normalize: return "normalize";
    case Operation::clear: return "clear";
    case Operation::restore: return "restore";
    case Operation::swap: return "swap";
    case Operation::setControls: return "set controls";
    }
    throw Error("unknown operation"); // a value no enumerator has
}

// A set of operations: one bit per enumerator.
using Operations = std::uint32_t;

constexpr Operations bit(Operation op) { return Operations { 1 } << static_cast<unsigned>(op); }

constexpr Operations operations(std::initializer_list<Operation> ops)
{
    Operations set = 0;
    for (const Operation op : ops)
        set |= bit(op);
    return set;
}

inline constexpr Operations kNoOperation = 0;
inline constexpr Operations kEveryOperation = (Operations { 1 } << kOperationCount) - 1;

// The operations that change a card: everything but the two that only look.
inline constexpr Operations kWrites =
    kEveryOperation & ~bit(Operation::read) & ~bit(Operation::pull);

// Roland's USB vendor id; the product id in a profile is under it.
inline constexpr std::uint16_t kUsbVendorRoland = 0x0582;

struct DeviceProfile {
    std::string_view familyName;         // the root element's name: <database name="RC-5" ...>
    std::array<std::uint8_t, 4> modelId; // Roland DT1/RQ1 model id, the 4 bytes after the device id
    std::uint16_t usbProductId;          // under kUsbVendorRoland
    int trackCount;                      // <TRACK1>..<TRACKn> per memory, NNN_1..NNN_n folders
    int slotCount;                       // <mem id="0..n-1"> per memory file
    std::string_view factorySections;    // a never-touched memory after its <NAME> block,
                                         // or empty: not on record
    Operations operations;               // what this app has opened for the model

    constexpr bool allows(Operation op) const { return (operations & bit(op)) != 0; }
    constexpr bool allowsWrites() const { return (operations & kWrites) != 0; }
    constexpr bool hasFactoryBody() const { return !factorySections.empty(); }
};

// The RC-5's never-touched memory after its name block, exactly as the pedal
// formats it: every one of the 57 factory-empty memories of a real card
// (fixtures/rc5-card.RC0) is byte-identical to the default name plus this.
// Note the non-obvious factory values: Measure=1, Reverb=30, Fill=1, Part4=0,
// rhythm Stop=1. This is what MEMORY CLEAR on the device leaves.
inline constexpr std::string_view kRc5FactorySections = R"(
<TRACK1>
	<Rev>0</Rev>
	<PlyLvl>100</PlyLvl>
	<Pan>50</Pan>
	<One>0</One>
	<StrtMod>0</StrtMod>
	<StpMod>0</StpMod>
	<Measure>1</Measure>
	<MeasMod>1</MeasMod>
	<MeasLen>0</MeasLen>
	<MeasBtLp>0</MeasBtLp>
	<RecTmp>1200</RecTmp>
	<WavStat>0</WavStat>
	<WavLen>0</WavLen>
</TRACK1>
<MASTER>
	<Tempo>1200</Tempo>
	<DubMode>0</DubMode>
	<RecAction>1</RecAction>
	<AutoRec>0</AutoRec>
	<FadeTime>5</FadeTime>
	<Level>100</Level>
	<LpMod>0</LpMod>
	<LpLen>0</LpLen>
	<TrkMod>1</TrkMod>
	<Sync>0</Sync>
</MASTER>
<RHYTHM>
	<Level>100</Level>
	<Reverb>30</Reverb>
	<Pattern>0</Pattern>
	<Variation>0</Variation>
	<VariationChange>0</VariationChange>
	<Kit>0</Kit>
	<Beat>2</Beat>
	<Fill>1</Fill>
	<Part1>1</Part1>
	<Part2>1</Part2>
	<Part3>1</Part3>
	<Part4>0</Part4>
	<RecCount>0</RecCount>
	<PlayCount>0</PlayCount>
	<Start>0</Start>
	<Stop>1</Stop>
	<ToneLow>10</ToneLow>
	<ToneHigh>10</ToneHigh>
	<State>0</State>
</RHYTHM>
)";

// RC-5 — the pedal this app was written for. Sources: fixtures/rc5-card.RC0
// (the root name, one <TRACK1> per memory, the factory body, the 99 memories),
// the sysex probe on the pedal (model id 00 00 00 76; its Identity Reply
// family 0x0376 ends in the same byte), ioreg on the connected pedal (product
// id 0x0251, the id app/LinuxDeviceRules.h matches on).
inline constexpr DeviceProfile kRc5 {
    "RC-5", { 0x00, 0x00, 0x00, 0x76 }, 0x0251, 1, 99, kRc5FactorySections, kEveryOperation
};

// The two-track RC model, "RC-500" in its card's root element. Sources: its
// card (the root name, <TRACK1> and <TRACK2> in every memory, audio folders
// NNN_1 and NNN_2, 99 memories), the sysex probe with both pedals on the bus
// (model id 00 00 00 77 answers, 76 and 78 are silent; Identity Reply family
// 0x0377), ioreg (product id 0x0252). Its factory body is not on record here,
// and reading is the one thing open on it: the browser lists such a card,
// and nothing changes it — every write, pull included, is still refused.
inline constexpr DeviceProfile kRc500 {
    "RC-500", { 0x00, 0x00, 0x00, 0x77 }, 0x0252, 2, 99, {}, bit(Operation::read)
};

inline constexpr std::array<const DeviceProfile*, 2> kAll { &kRc5, &kRc500 };

// The sentence every refusal ends with. It is deliberate: it names what the
// app does and promises nothing about what it might do. Only the RC-5 is
// open for reading today; the day another model is, this sentence changes
// with the table, not before.
inline std::string onlySpeaks()
{
    return "LooperCat only speaks " + std::string(kRc5.familyName);
}

// A card this app does not read, said the way it has always been said: the
// model named, the RC-5 named, nothing promised. An unknown model and a known
// model that is not open for reading get the same words — to the player they
// are the same fact.
inline std::string notOurs(std::string_view familyName)
{
    return "this is an \"" + std::string(familyName) + "\" card, not an "
         + std::string(kRc5.familyName) + " \xe2\x80\x94 " + onlySpeaks();
}

// The profile whose card writes this root name — or a refusal by name.
inline const DeviceProfile& byFamilyName(std::string_view familyName)
{
    for (const DeviceProfile* candidate : kAll)
        if (candidate->familyName == familyName)
            return *candidate;
    throw Error(notOurs(familyName));
}

// The gate every operation passes. A closed read is refused as a card that is
// not ours; a closed mutation is refused in the mutation's own name, so the
// gate that fired can be traced from its words.
inline void require(const DeviceProfile& family, Operation op)
{
    if (family.allows(op))
        return;
    if (op == Operation::read)
        throw Error(notOurs(family.familyName));
    throw Error(std::string(operationName(op)) + " refused on an \""
                + std::string(family.familyName) + "\" card \xe2\x80\x94 " + onlySpeaks());
}

// The backstop under every write: a model with no write open gets none,
// whichever command forgot to ask.
inline void requireWrites(const DeviceProfile& family)
{
    if (!family.allowsWrites())
        throw Error("no write on an \"" + std::string(family.familyName) + "\" card \xe2\x80\x94 "
                    + onlySpeaks());
}

} // namespace loopercat::profile
