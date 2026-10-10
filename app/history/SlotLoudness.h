// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryRecorder.h"

#include <loopercat/Commands.hpp>
#include <loopercat/Loudness.hpp>
#include <loopercat/Normalize.hpp>
#include <loopercat/Volume.hpp>

#include <cmath>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <vector>

//==============================================================================
// loopercat::history::readSlotLoudness — one slot's loudness read as the
// worker runs it, for Check loudness and the background check (issue #61):
// the take's bytes come off the card once and are measured, and the reading
// is filed in the history under the hash of those very bytes (#140) before
// the caller turns it into words.
//
// The read is the job; the filing is its tail. A history with no card in
// front of it takes nothing (`kept` false, no failure), and a store that
// refuses the row is reported in `failure` with the reading still returned —
// the player asked how loud the loop is, and that answer does not depend on
// the history being able to remember it. A slot with no take, and bytes that
// are not the pedal's own stereo float, are errors as before.
//
// recallOrReadSlotLoudness is the same read with the history asked first
// (#142), for the single-slot Normalize's measure-before-asking step: a
// reading the history holds for exactly these bytes is the answer, as filed,
// and only bytes it has never measured are measured. Exactly these bytes —
// the key is the content hash, so what the history infers about a slot from
// a name, a size and a date (#141) never answers here: a guess may inform the
// eye, never a decision. Nor does a stored reading the meter could not have
// taken (plausibleReading), nor one the caller cannot use (`serves`): the
// bytes in hand are measured once and the fresh reading is filed over it.
// A history that cannot be asked — no card in front of it, a store that
// will not answer — is measured around; a store that would not answer is not
// asked a second time to file, and the trouble goes in `failure`.
//
// `failure` says why the history does not hold the reading, and only then:
// it is empty whenever `kept` is true.
//
// Worker thread, like every read of the card and every write to the store.
//==============================================================================
namespace loopercat::history
{

struct SlotLoudness {
    wav::LoudnessReading reading;
    std::string hash;      // of the bytes measured — the key the reading is filed under
    bool kept = false;     // the history holds the reading now
    bool recalled = false; // the history held it already, for these very bytes: nothing was measured
    std::string failure;   // why the history does not hold it: it could not be asked, or refused it
};

// Whether a reading could have come off the meter at all (review of #142):
// no field is NaN and no count negative; a sample peak is a magnitude,
// finite — or +inf beside impossible samples, which is how the meter
// reports a +inf sample, and what the store keeps of it; an integrated
// loudness, when there is one, is a finite number above the -70 LUFS
// absolute gate (the meter answers "unmeasurable" at the gate and below),
// and a take loud enough to measure has a true peak that is finite, or
// +inf beside impossible samples. Anything else in the store was put there
// by something other than this meter, and is measured again rather than
// believed.
inline bool plausibleReading(const wav::LoudnessReading& reading)
{
    if (reading.wildSamples < 0 || std::isnan(reading.samplePeak) || std::isnan(reading.truePeakDb))
        return false;
    const bool damaged = reading.wildSamples > 0;
    if (reading.samplePeak < 0.0f || (std::isinf(reading.samplePeak) && !damaged))
        return false;
    if (!reading.integratedLufs.has_value())
        return true;
    const double lufs = *reading.integratedLufs;
    return std::isfinite(lufs) && lufs > loudness::kAbsoluteGateLufs
        && (std::isfinite(reading.truePeakDb) || (damaged && reading.truePeakDb > 0.0));
}

namespace detail {

    // The slot's take as the worker reads it: the bytes, named as the
    // history names them.
    struct TakeBytes {
        std::string raw;
        std::string hash;
    };

    inline TakeBytes readTake(const volume::fs::path& volume, int slot)
    {
        const std::vector<std::string> files = volume::listSlotWavs(volume, slot);
        if (files.empty())
            throw Error("slot " + std::to_string(slot) + " has no audio to measure");
        TakeBytes take;
        take.raw = commands::readFileBytes(volume::wavDir(volume, slot) / files.front());
        take.hash = HistoryStore::contentHash(take.raw);
        return take;
    }

    inline void measure(const TakeBytes& take, SlotLoudness& out)
    {
        out.reading = wav::measureLoudness(wav::BytesView(
            reinterpret_cast<const unsigned char*>(take.raw.data()), take.raw.size()));
    }

    // Measure the bytes and file the reading under their hash; the filing's
    // trouble is reported beside the reading, never thrown over it.
    inline void measureAndFile(const TakeBytes& take, HistoryRecorder& recorder, SlotLoudness& out)
    {
        measure(take, out);
        try {
            out.kept = recorder.reading(out.hash, out.reading);
        } catch (const std::exception& e) {
            out.failure = e.what(); // the store's refusal, or anything else the filing threw
        }
    }

} // namespace detail

inline SlotLoudness readSlotLoudness(const volume::fs::path& volume, int slot,
                                     HistoryRecorder& recorder)
{
    const detail::TakeBytes take = detail::readTake(volume, slot);
    SlotLoudness out;
    out.hash = take.hash;
    detail::measureAndFile(take, recorder, out);
    return out;
}

// `serves`: whether the caller can use a stored reading — for Normalize,
// whether the plan can be made of it. Asked of a plausible stored reading
// only, and never of the fresh one: a rejected reading is measured over once.
inline SlotLoudness recallOrReadSlotLoudness(
    const volume::fs::path& volume, int slot, HistoryRecorder& recorder,
    const std::function<bool(const wav::LoudnessReading&)>& serves)
{
    const detail::TakeBytes take = detail::readTake(volume, slot);
    SlotLoudness out;
    out.hash = take.hash;
    // Asked only under the condition a reading is filed under — a session
    // open for this card — so the store is never opened for the asking alone.
    if (recorder.sessionOn(volume)) {
        std::optional<HistoryStore::StoredReading> known;
        try {
            known = recorder.store().readingFor(out.hash);
        } catch (const std::exception& e) {
            // The bytes can still be measured, and are; the store is not asked
            // again to file the reading — a locked one would hold the worker
            // through a second busy timeout for the same answer.
            out.failure = std::string("the history could not be asked: ") + e.what();
            detail::measure(take, out);
            return out;
        }
        if (known.has_value() && plausibleReading(known->reading) && serves(known->reading)) {
            out.reading = known->reading;
            out.kept = out.recalled = true;
            return out;
        }
    }
    detail::measureAndFile(take, recorder, out);
    return out;
}

} // namespace loopercat::history
