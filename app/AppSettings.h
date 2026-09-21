// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_data_structures/juce_data_structures.h>

//==============================================================================
// loopercat::AppSettings — the app's per-user PropertiesFile, family layout:
// ~/Library/Application Support/Darwin's Cat/LooperCat/LooperCat.settings
// (same shape as OrbitCab's). Today its only client is the update badge
// persistence (see UpdateCheck.h); app preferences join it when they exist.
//
// `dataOverride` (the --data CLI seam) moves the whole home — settings,
// history, trash, backups — to another directory, so a verification run on a
// synthetic pedal never writes into the player's own data. Empty = the
// platform location, which is the only one a normal launch ever uses.
//==============================================================================
namespace loopercat
{

class AppSettings
{
public:
    explicit AppSettings(const juce::File& dataOverride = {})
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "LooperCat";
        options.filenameSuffix = ".settings";
        // An absolute folderName is taken as-is by PropertiesFile (File::getChildFile).
        options.folderName = dataOverride == juce::File() ? juce::String("Darwin's Cat/LooperCat")
                                                          : dataOverride.getFullPathName();
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters(options);
    }

    juce::PropertiesFile* file() { return properties.getUserSettings(); }

    // The app's data home (…/Darwin's Cat/LooperCat) — backups and the
    // clear-command trash live under it.
    juce::File dataDir()
    {
        auto* settings = file();
        jassert(settings != nullptr); // storage parameters are set in the ctor
        return settings->getFile().getParentDirectory();
    }

private:
    juce::ApplicationProperties properties;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppSettings)
};

} // namespace loopercat
