// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The read-only slot view derived from a memory file: what the browser table
// shows. Pure — memory text in, model out; file I/O stays with the caller.
//
// A memory is a name, a playback tempo, a rhythm, and one track per track the
// model has: one on the RC-5, two on the two-track model, each track with its
// own take, length and recording tempo (hardware: memories with a 4-bar
// track 1 beside an 8-bar track 2). The view lists every track; the flat
// fields repeat the first track's facts, because the RC-5 has exactly that
// one loop and every caller so far reads a memory as one loop.
//
// Field semantics source: rc5cat lib/commands.js listSlots (WavStat=1 means
// indexed audio, WavLen is the frame count, Tempo is tenths of BPM, One is
// the One Shot flag), plus the 2026-07-24 hardware analysis (#10): MeasLen is
// the true bar count the pedal displays; the stored Measure field is
// MeasLen + 7 (the first seven values are the UI's special modes).

#pragma once

#include "DeviceProfile.hpp"
#include "Rc0.hpp"
#include "usecases/CountIn.hpp"

#include <string>
#include <utility>
#include <vector>

namespace loopercat::catalog {

// One track of a memory: what its <TRACKn> section says.
struct TrackInfo {
    int track;                // 1-based: the pedal's TRACK number, and the audio folder's suffix
    bool hasAudio;            // WavStat == 1: the pedal has indexed a take here
    long long frames;         // WavLen: sample frames at 44.1 kHz
    bool oneShot;             // One == 1
    long long measures;       // MeasLen: whole bars, as the pedal displays them
    long long recTempoTenths; // RecTmp: tenths of BPM the take was recorded at

    bool operator==(const TrackInfo&) const = default;
};

// WARNING on the flat fields hasAudio, frames, oneShot, measures and
// recTempoTenths: they are TRACK1's facts, not the memory's. On the RC-5 the
// two are the same thing, because a memory holds one track; on a multi-track
// card they are not — a memory whose take sits on track 2 reads as empty
// here, and one with a longer track 2 reads as the shorter loop. They stay
// flat because every caller today reads a memory as one loop; a caller that
// shows a multi-track card reads `tracks`, or shows a wrong number.
struct SlotInfo {
    int slot;               // 1-based, as shown on the pedal display
    std::string name;       // 12 chars, space-padded as stored
    bool hasAudio;          // WavStat == 1: the pedal has indexed a loop here
    long long frames;       // WavLen: sample frames at 44.1 kHz
    bool oneShot;           // One == 1
    bool countIn;           // a count will be heard before this memory plays
                            // (usecases::countin::isOn — State on + PLAY COUNT
                            // 1MEAS; the pattern is none of its business)
    bool countInTakesPattern; // switching the count on here would replace a
                              // rhythm pattern picked on the pedal — the one
                              // thing the UI has to say out loud beforehand.
                              // A bool, not the pattern: its names are not
                              // hardware-verified yet, so we cannot name it.
    long long tempoTenths;  // Tempo: tenths of BPM the pedal will play at
    long long measures;     // MeasLen: whole bars, as the pedal displays them
    long long recTempoTenths; // RecTmp: tenths of BPM the take was recorded at —
                              // when Tempo differs, the pedal time-stretches on playback
    // Every track of the memory, TRACK1 first — as many as the model has. The
    // flat fields above are tracks.front()'s.
    std::vector<TrackInfo> tracks;

    bool operator==(const SlotInfo&) const = default;
};

// One track's facts, read from its own section (rc0::sectionField). A memory
// that lacks a section its model promises is refused by name, not read as
// empty.
inline TrackInfo readTrack(std::string_view body, int track)
{
    const std::string section = rc0::trackSectionName(track);
    const auto field = [&body, &section](std::string_view tag) {
        return rc0::sectionField(body, section, tag);
    };
    return { track,      field("WavStat") == 1, field("WavLen"),
             field("One") == 1, field("MeasLen"), field("RecTmp") };
}

// A memory as the model it belongs to lays it out: the profile says how many
// tracks to read; the playback tempo is MASTER's.
inline SlotInfo readSlot(std::string_view memoryText, const profile::DeviceProfile& family,
                         int slot)
{
    const std::string body = rc0::slotBody(memoryText, slot);
    std::vector<TrackInfo> tracks;
    tracks.reserve(static_cast<std::size_t>(family.trackCount));
    for (int track = 1; track <= family.trackCount; ++track)
        tracks.push_back(readTrack(body, track));
    const TrackInfo& first = tracks.front();
    return { slot,
             rc0::decodeName(body),
             first.hasAudio,
             first.frames,
             first.oneShot,
             usecases::countin::isOn(body),
             usecases::countin::patternAtRisk(body).has_value(),
             rc0::sectionField(body, rc0::kSectionMaster, "Tempo"),
             first.measures,
             first.recTempoTenths,
             std::move(tracks) };
}

// The same, with the model read off the document's own root element.
inline SlotInfo readSlot(std::string_view memoryText, int slot)
{
    return readSlot(memoryText, rc0::profileOf(memoryText), slot);
}

inline std::vector<SlotInfo> listSlots(std::string_view memoryText)
{
    const profile::DeviceProfile& family = rc0::profileOf(memoryText);
    std::vector<SlotInfo> out;
    out.reserve(rc0::kSlotCount);
    for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
        out.push_back(readSlot(memoryText, family, slot));
    return out;
}

} // namespace loopercat::catalog
