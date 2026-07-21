// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_data_structures/juce_data_structures.h>

//==============================================================================
// loopercat::AppSettings — the app's per-user PropertiesFile, family layout:
// ~/Library/Application Support/Darwin's Cat/LooperCat/LooperCat.settings
// (same shape as OrbitCab's). Today its only client is the update badge
// persistence (see UpdateCheck.h); app preferences join it when they exist.
//==============================================================================
namespace loopercat
{

class AppSettings
{
public:
    AppSettings()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "LooperCat";
        options.filenameSuffix = ".settings";
        options.folderName = "Darwin's Cat/LooperCat";
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters(options);
    }

    juce::PropertiesFile* file() { return properties.getUserSettings(); }

private:
    juce::ApplicationProperties properties;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppSettings)
};

} // namespace loopercat
