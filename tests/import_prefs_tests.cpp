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
// the comparison allows for nothing but the representation.
bool nearly(double actual, double expected) { return std::abs(actual - expected) < 1.0e-9; }

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
                                        .convertedMark = " (pedal)" });
        } // closed: the next open reads the disk, not this object
        auto file = open(work);
        const ImportPrefs prefs = importprefs::read(*file);
        CHECK(prefs.normalizeOnUpload);
        CHECK(nearly(prefs.targetLufs, -16.5));
        CHECK_EQ(prefs.convertedMark.toStdString(), std::string(" (pedal)")); // spaces survive
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
