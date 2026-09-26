// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/StorageRegister.hpp>

#include <optional>
#include <string>

//==============================================================================
// loopercat::connectgate — what Connect does with the pedal's answer to
// "can you hand over your card?" (issue #85), and only that decision.
//
// Before the first enter-storage frame the app reads the storage register.
// One answer short-circuits the whole attempt: a pedal that is playing or
// holds an unsaved take refuses the frame on the spot, and no resend budget
// changes that — only the musician can. Everything else keeps today's
// attempt: idle means go; no answer (or an exchange that failed) means the
// pedal's MIDI side may still be re-enumerating, which is exactly the case
// the resend budget exists for; in storage already means the frame is not
// needed at all — the medium is on its way or already here.
//
// Pure over the answer, so the rule is tested without a pedal.
//==============================================================================
namespace loopercat::connectgate
{

enum class Verdict {
    sendFrame,        // begin the supervised attempt and send enter-storage
    alreadyInStorage, // send nothing; wait for the medium the pedal is already offering
    refuse            // send nothing; tell the player what to do, in `reason`
};

struct Decision {
    Verdict verdict;
    std::string reason; // the player's sentence; empty unless refused
};

// The sentence for the one case the register settles. Both causes are
// named because the register cannot tell them apart (measured 2026-09-25:
// 02 while playing AND 02 with an unsaved take, on both RC-5s).
inline constexpr const char* kBusySentence =
    "The pedal is playing or holds an unsaved take \xe2\x80\x94 stop it and save (WRITE), "
    "then press Connect.";

// `answer` is what the register said, or nothing when the pedal did not
// answer in time or the exchange itself failed — silence is not a state.
inline Decision decide(std::optional<storage::State> answer)
{
    if (!answer)
        return { Verdict::sendFrame, {} };
    switch (*answer) {
    case storage::State::idle: return { Verdict::sendFrame, {} };
    case storage::State::inStorage: return { Verdict::alreadyInStorage, {} };
    case storage::State::busy: return { Verdict::refuse, kBusySentence };
    }
    throw Error("connectgate: a storage state without a decision");
}

} // namespace loopercat::connectgate
