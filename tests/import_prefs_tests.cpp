// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The upload preferences in the app's own PropertiesFile (issues #53, #139),
// against what Settings promises: a file nobody has touched reads as the
// shipped defaults, what is written is what is read back by a fresh open of
// the same file — a mark cleared to nothing included — and the keys sit
// beside each other under their own names, so a hand-edited settings file
// reads the way it looks.

#include "support.hpp"

#include "../app/ImportPrefs.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cmath>
#include <memory>
#include <optional>
#include <string>

using namespace loopercat;

namespace
{

// A PropertiesFile in `dir`, the way AppSettings opens it. Every open is a
// fresh object, so a value that survives is one that reached the disk.
std::unique_ptr<juce::PropertiesFile> open(const juce::File& dir)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "LooperCat";
    options.filenameSuffix = ".settings";
    options.folderName = dir.getFullPathName();
    options.osxLibrarySubFolder = "Application Support";
    return std::make_unique<juce::PropertiesFile>(options);
}

// A target that went through the file as text comes back as the same number;
// the comparison allows for nothing but the representation. No target is not
// near anything.
bool nearly(std::optional<double> actual, double expected)
{
    return actual.has_value() && std::abs(*actual - expected) < 1.0e-9;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::File work = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("import_prefs_tests")
                                .getNonexistentSibling();
    work.createDirectory();

    // --- a file nobody touched reads as the shipped defaults ---
    {
        auto file = open(work);
        const ImportPrefs prefs = importprefs::read(*file);
        CHECK(!prefs.normalizeOnUpload);
        CHECK(nearly(prefs.targetLufs, -18.0));
        CHECK_EQ(prefs.convertedMark.toStdString(), std::string("-pedal"));
        CHECK(!file->containsKey(importprefs::kConvertedMarkKey)); // reading wrote nothing
    }

    // --- what is written is what a fresh open reads back ---
    {
        {
            auto file = open(work);
            importprefs::write(*file, { .normalizeOnUpload = true,
                                        .targetLufs = -16.5,
                                        .convertedMark = "(pedal) v2" });
        } // closed: the next open reads the disk, not this object
        auto file = open(work);
        const ImportPrefs prefs = importprefs::read(*file);
        CHECK(prefs.normalizeOnUpload);
        CHECK(nearly(prefs.targetLufs, -16.5));
        CHECK_EQ(prefs.convertedMark.toStdString(), std::string("(pedal) v2")); // the space inside survives
    }

    // --- a mark cleared to nothing stays nothing: "" is a value, not an absence ---
    {
        {
            auto file = open(work);
            ImportPrefs prefs = importprefs::read(*file);
            prefs.convertedMark = "";
            importprefs::write(*file, prefs);
        }
        auto file = open(work);
        const ImportPrefs prefs = importprefs::read(*file);
        CHECK(file->containsKey(importprefs::kConvertedMarkKey));
        CHECK_EQ(prefs.convertedMark.toStdString(), std::string(""));
        CHECK(prefs.normalizeOnUpload);  // the other two keys were not touched
        CHECK(nearly(prefs.targetLufs, -16.5));
    }

    // --- a mark of only whitespace reads as empty; every other mark reads as written:
    //     one rule (markAsTyped), shared with the Settings field, and nothing else rewritten ---
    {
        {
            auto file = open(work);
            file->setValue(importprefs::kConvertedMarkKey, "   ");
            file->saveIfNeeded();
        }
        auto file = open(work);
        CHECK_EQ(importprefs::read(*file).convertedMark.toStdString(), std::string(""));
        file->setValue(importprefs::kConvertedMarkKey, "\t \n");
        CHECK_EQ(importprefs::read(*file).convertedMark.toStdString(), std::string(""));
        file->setValue(importprefs::kConvertedMarkKey, " -live "); // spaces around stay: as written
        CHECK_EQ(importprefs::read(*file).convertedMark.toStdString(), std::string(" -live "));
        file->setValue(importprefs::kConvertedMarkKey, " pedal");
        CHECK_EQ(importprefs::read(*file).convertedMark.toStdString(), std::string(" pedal"));
        file->setValue(importprefs::kConvertedMarkKey, "- live -"); // and inside
        CHECK_EQ(importprefs::read(*file).convertedMark.toStdString(), std::string("- live -"));
        file->setValue(importprefs::kConvertedMarkKey, "");
        file->saveIfNeeded();
        // the rule itself
        CHECK_EQ(importprefs::markAsTyped("").toStdString(), std::string(""));
        CHECK_EQ(importprefs::markAsTyped("  ").toStdString(), std::string(""));
        CHECK_EQ(importprefs::markAsTyped(" x ").toStdString(), std::string(" x "));
    }

    // --- the keys are the ones a settings file shows, beside each other ---
    {
        auto file = open(work);
        CHECK_EQ(std::string(importprefs::kNormalizeOnUploadKey), std::string("normalizeOnUpload"));
        CHECK_EQ(std::string(importprefs::kTargetLufsKey), std::string("normalizeTargetLufs"));
        CHECK_EQ(std::string(importprefs::kConvertedMarkKey), std::string("convertedUploadMark"));
        CHECK(file->containsKey("normalizeOnUpload"));
        CHECK(file->containsKey("normalizeTargetLufs"));
        CHECK(file->containsKey("convertedUploadMark"));
    }

    // --- stored text that is not a target reads as no target, and nothing is put in its place
    //     (#142 review): the field's window is -30..-8, NaN and infinity are not in it ---
    {
        const auto readTarget = [&](const juce::String& stored) {
            auto file = open(work);
            file->setValue(importprefs::kTargetLufsKey, stored);
            return importprefs::read(*file).targetLufs;
        };
        CHECK(!readTarget("nan").has_value());
        CHECK(!readTarget("inf").has_value());
        CHECK(!readTarget("-inf").has_value());
        CHECK(!readTarget("abc").has_value());
        CHECK(!readTarget("").has_value());     // present but empty: not the default either
        CHECK(!readTarget("0").has_value());
        CHECK(!readTarget("-40").has_value());
        CHECK(!readTarget("-30.01").has_value());
        CHECK(!readTarget("-7.9").has_value());
        CHECK(nearly(readTarget("-30"), -30.0)); // both ends of the window are targets
        CHECK(nearly(readTarget("-8"), -8.0));
        CHECK(nearly(readTarget(" -14 "), -14.0));

        // writing with no target leaves the stored text exactly as it was,
        // while the keys that have values are written
        {
            auto file = open(work);
            file->setValue(importprefs::kTargetLufsKey, "nan");
            file->saveIfNeeded();
            ImportPrefs prefs = importprefs::read(*file);
            CHECK(!prefs.targetLufs.has_value());
            prefs.normalizeOnUpload = false;
            prefs.convertedMark = "-x";
            importprefs::write(*file, prefs);
        }
        {
            auto file = open(work);
            CHECK_EQ(file->getValue(importprefs::kTargetLufsKey).toStdString(), std::string("nan"));
            const ImportPrefs prefs = importprefs::read(*file);
            CHECK(!prefs.normalizeOnUpload);
            CHECK_EQ(prefs.convertedMark.toStdString(), std::string("-x"));
            // and a target typed later replaces it
            ImportPrefs typed = prefs;
            typed.targetLufs = -20.0;
            typed.normalizeOnUpload = true;
            typed.convertedMark = "-pedal";
            importprefs::write(*file, typed);
        }
        {
            auto file = open(work);
            CHECK(nearly(importprefs::read(*file).targetLufs, -20.0));
            // an absent target key is the shipped default, not "no target"
            file->removeValue(importprefs::kTargetLufsKey);
            CHECK(nearly(importprefs::read(*file).targetLufs, -18.0));
            importprefs::write(*file, { .normalizeOnUpload = true, .targetLufs = -16.5,
                                        .convertedMark = "-pedal" });
        }
    }

    // --- an absent mark key reads as the default even when the others are set ---
    {
        {
            auto file = open(work);
            file->removeValue(importprefs::kConvertedMarkKey);
            file->saveIfNeeded();
        }
        auto file = open(work);
        const ImportPrefs prefs = importprefs::read(*file);
        CHECK_EQ(prefs.convertedMark.toStdString(), std::string("-pedal"));
        CHECK(prefs.normalizeOnUpload);
    }

    // Every PropertiesFile above is closed; a cleanup that fails is a failure.
    CHECK(work.deleteRecursively());
    return testkit::summary("import_prefs");
}
