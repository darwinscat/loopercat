// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "UploadMark.h"

#include <juce_data_structures/juce_data_structures.h>

//==============================================================================
// loopercat::ImportPrefs — what happens to an upload on its way to the card,
// and where the app keeps the answer: three keys in the per-user
// PropertiesFile (AppSettings), beside the column and history preferences.
//
// One reader and one writer, so the Settings dialog that edits them, the push
// job that acts on them and a test that round-trips them all see the same
// keys with the same defaults. The push job reads them when the player acts
// — as the job is queued — and carries them with it: a settings change
// after that must not rewrite a job already promised.
//==============================================================================
namespace loopercat
{

struct ImportPrefs {
    // Normalize-on-upload (issue #53): OFF by default so every existing
    // workflow keeps the byte-exact pass-through; -18 LUFS is ReplayGain
    // 2.0's reference — the modern spelling of the "89 dB" the request
    // arrived in.
    bool normalizeOnUpload;
    double targetLufs;
    // The mark a converted upload lands with (issue #139): "-pedal" out of
    // the box, which is the spelling existing cards already carry; empty
    // means converted uploads land under their own name.
    juce::String convertedMark;
};

namespace importprefs
{
    // The one rewrite a mark gets, typed or stored: a mark of only
    // whitespace is the empty mark — "Empty: no mark", as the hint says.
    // Any other mark stays as typed, spaces before, inside and after
    // included: a rewritten mark is a different mark (UploadMark.h).
    inline juce::String markAsTyped(const juce::String& text)
    {
        return text.trim().isEmpty() ? juce::String() : text;
    }

    inline constexpr auto kNormalizeOnUploadKey = "normalizeOnUpload";
    inline constexpr auto kTargetLufsKey = "normalizeTargetLufs";
    inline constexpr auto kConvertedMarkKey = "convertedUploadMark";
    inline constexpr double kDefaultTargetLufs = -18.0;

    // What a player who never opened Settings gets.
    inline ImportPrefs defaults()
    {
        return { .normalizeOnUpload = false,
                 .targetLufs = kDefaultTargetLufs,
                 .convertedMark = juce::String(uploadmark::kDefaultSuffix.data(),
                                               uploadmark::kDefaultSuffix.size()) };
    }

    // A key that is absent reads as its default; a key that is present reads
    // as stored, so a mark cleared to "" stays cleared — PropertySet::getValue
    // falls back only when the key is missing, not when its value is empty.
    // The mark gets the field's one rule (markAsTyped), so "only spaces is
    // the empty mark" holds whichever hand wrote the file — and nothing else
    // about it is rewritten.
    inline ImportPrefs read(juce::PropertiesFile& file)
    {
        const ImportPrefs shipped = defaults();
        return { .normalizeOnUpload =
                     file.getBoolValue(kNormalizeOnUploadKey, shipped.normalizeOnUpload),
                 .targetLufs = file.getDoubleValue(kTargetLufsKey, shipped.targetLufs),
                 .convertedMark = markAsTyped(file.getValue(kConvertedMarkKey, shipped.convertedMark)) };
    }

    inline void write(juce::PropertiesFile& file, const ImportPrefs& prefs)
    {
        file.setValue(kNormalizeOnUploadKey, prefs.normalizeOnUpload);
        file.setValue(kTargetLufsKey, prefs.targetLufs);
        file.setValue(kConvertedMarkKey, prefs.convertedMark);
        file.saveIfNeeded();
    }
} // namespace importprefs

} // namespace loopercat
