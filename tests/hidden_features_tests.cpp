// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Two doors 0.9.6 closed can only be proved shut by reading the window's own
// source. Both are about something NOT happening in MainComponent, which no
// headless suite can build: it wants a pedal, a worker thread and a screen.
// So this suite reads app/ as text, and fails with the line that opened a
// door again:
//
//   - Undo and Redo have no keys anywhere in the app. The logic stays
//     (history::undo, the --undo / --redo seam); the keystrokes do not, on
//     any platform.
//   - Nothing CALLS the global History window open. Its code stays, and so
//     does the --history-window seam, which feeds the view without a window
//     — but no key, click or command in the main window reaches it.
//
// A lint is only as good as its reach, so each rule says what it covers:
// these two read every .h/.cpp under app/, and both are deliberately wider
// than the code they guard.

#include "support.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Sighting {
    std::string file; // the file's own name, enough to point a reader at it
    std::string line; // the line, trimmed
};

std::string trimmed(const std::string& line)
{
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return line.substr(first, line.find_last_not_of(" \t\r\n") - first + 1);
}

std::vector<fs::path> sources()
{
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(LOOPERCAT_APP_DIR)) {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext == ".h" || ext == ".hpp" || ext == ".cpp" || ext == ".mm")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<Sighting> linesWith(const std::string& needle)
{
    std::vector<Sighting> found;
    for (const auto& file : sources()) {
        std::ifstream in(file);
        if (!in)
            throw loopercat::Error("cannot read " + file.string());
        std::string line;
        while (std::getline(in, line))
            if (line.find(needle) != std::string::npos)
                found.push_back({ file.filename().string(), trimmed(line) });
    }
    return found;
}

void report(const char* what, const std::vector<Sighting>& found)
{
    for (const auto& sighting : found)
        testkit::fail(std::string(what) + " -- " + sighting.file + ": " + sighting.line, __FILE__,
                      __LINE__);
}

} // namespace

int main()
{
    // The sources are there to be read at all: an empty sweep would make
    // every rule below pass by saying nothing.
    const std::size_t files = sources().size();
    CHECK(files > 20);

    // --- no Z key binding, on any platform ---
    // Every binding this app ever had was a juce::KeyPress('z', ...). The
    // rule is any line naming a key press and that character, so a Ctrl-Z
    // added for Windows fails here too.
    std::vector<Sighting> keys;
    for (const auto& sighting : linesWith("'z'"))
        if (sighting.line.find("KeyPress") != std::string::npos)
            keys.push_back(sighting);
    report("a Z key binding is back", keys);
    CHECK(keys.empty());

    // --- nothing calls the global History window open ---
    // Exactly two mentions of the call: where it is declared, and where it
    // is defined. A third is a caller, which is the door reopening.
    std::vector<std::string> openers;
    for (const auto& sighting : linesWith("openHistoryWindow("))
        openers.push_back(sighting.file + ": " + sighting.line);
    const std::vector<std::string> expected {
        "MainComponent.cpp: void MainComponent::openHistoryWindow()",
        "MainComponent.h: void openHistoryWindow();",
    };
    const auto knows = [&expected](const std::string& line) {
        return std::find(expected.begin(), expected.end(), line) != expected.end();
    };
    for (const auto& line : openers)
        if (!knows(line))
            testkit::fail("the global History window has a way in -- " + line, __FILE__, __LINE__);
    // And the other way round: if the declaration or the definition moved,
    // this rule is guarding a shape the code no longer has, and saying so
    // beats passing quietly.
    for (const auto& line : expected)
        if (std::find(openers.begin(), openers.end(), line) == openers.end())
            testkit::fail("this rule lost its anchor -- no line reads \"" + line + "\"", __FILE__,
                          __LINE__);
    CHECK(openers == expected);

    return testkit::summary("hidden_features_tests");
}
