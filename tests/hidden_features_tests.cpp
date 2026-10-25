// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The doors 0.9.6 closed, proved shut by reading app/ as text. Both rules are
// about something NOT happening in MainComponent, which no headless suite can
// build: it wants a pedal, a worker thread and a screen before it exists.
//
//   - no key binding for Undo or Redo survives anywhere in the app, however
//     it might be spelled. The logic stays (history::undo, the --undo /
//     --redo seam); the keystrokes do not, on any platform.
//   - the things this release hid have exactly the sightings they are
//     supposed to have, and no more: a door is a stray sighting, and a
//     reworded anchor is a rule that has stopped guarding anything. Both
//     fail, and both name the file and line.
//
// A lint is only as strong as the rewrites it survives, so the key rule is
// not a search for the line that was deleted. Any 'z' or 'Z' character
// literal in app/ fails it — the only two in the tree are an alphabet range
// in PedalPortName.h, which name 'a' on the same line — and so does any
// spelled-out cmd+z / ctrl+z in a string. That covers KeyPress('Z', ...),
// a reflowed call, getKeyCode() == 'z', addDefaultKeypress('z', ...) and
// KeyPress::createFromDescription("cmd + z"), each of which is how someone
// would really write the binding back.
//
// Both rules read EVERY file under app/, whatever its extension, and read it
// with comments stripped: a new .inl is not a way out, and a comment naming
// an opener is not an opening.
//
// The anchors are exact text, so a reflow of a declaration fails this suite.
// That is the trade: the rule is coupled to the shape of five lines, and in
// exchange nothing can quietly reach what is hidden.

#include "support.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Line {
    std::string file; // the file's own name, enough to point a reader at it
    int number = 0;
    std::string text; // comments stripped, trimmed
};

std::string trimmed(const std::string& line)
{
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return line.substr(first, line.find_last_not_of(" \t\r\n") - first + 1);
}

std::string lowercased(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string withoutSpaces(const std::string& text)
{
    std::string out;
    for (const char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            out += c;
    return out;
}

// Every file under app/, in a stable order. No extension filter: a binding
// moved into a new kind of file would otherwise walk straight out of reach.
// A file carrying a NUL byte is not source and is skipped.
std::vector<fs::path> sources()
{
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(LOOPERCAT_APP_DIR)) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream probe(entry.path(), std::ios::binary);
        if (!probe)
            throw loopercat::Error("cannot read " + entry.path().string());
        const std::string head((std::istreambuf_iterator<char>(probe)),
                               std::istreambuf_iterator<char>());
        if (head.find('\0') != std::string::npos)
            continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The whole tree as lines with their comments removed, so a rule is about
// code and a comment can say anything.
std::vector<Line> code()
{
    std::vector<Line> lines;
    for (const auto& file : sources()) {
        std::ifstream in(file);
        if (!in)
            throw loopercat::Error("cannot read " + file.string());
        std::string raw;
        int number = 0;
        bool inBlockComment = false;
        while (std::getline(in, raw)) {
            ++number;
            std::string text;
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (inBlockComment) {
                    if (raw.compare(i, 2, "*/") == 0) {
                        inBlockComment = false;
                        ++i;
                    }
                    continue;
                }
                if (raw.compare(i, 2, "//") == 0)
                    break;
                if (raw.compare(i, 2, "/*") == 0) {
                    inBlockComment = true;
                    ++i;
                    continue;
                }
                text += raw[i];
            }
            lines.push_back({ file.filename().string(), number, trimmed(text) });
        }
    }
    return lines;
}

std::string where(const Line& line) { return line.file + ":" + std::to_string(line.number); }

// A needle's sightings must be exactly these lines. A stray is a door; a
// missing one means the rule is pointing at code that has moved.
void anchor(const std::vector<Line>& lines, const std::string& needle,
            const std::vector<std::string>& expected)
{
    std::vector<std::string> seen;
    for (const Line& line : lines)
        if (line.text.find(needle) != std::string::npos)
            seen.push_back(line.text);
    for (const std::string& stray : seen)
        if (std::find(expected.begin(), expected.end(), stray) == expected.end())
            testkit::fail("\"" + needle + "\" has a sighting nobody asked for: " + stray, __FILE__,
                          __LINE__);
    for (const std::string& lost : expected)
        if (std::find(seen.begin(), seen.end(), lost) == seen.end())
            testkit::fail("this rule lost its anchor: no line of app/ reads \"" + lost + "\"",
                          __FILE__, __LINE__);
    testkit::check(seen.size() == expected.size(), needle.c_str(), __FILE__, __LINE__);
}

} // namespace

int main()
{
    const std::vector<Line> lines = code();
    // The sweep is there at all: an empty one would make every rule below
    // pass by saying nothing.
    const std::size_t files = sources().size();
    CHECK(files > 20);
    CHECK(lines.size() > 2000);

    // --- Undo and Redo have no key, in any spelling, on any platform ---
    std::vector<const Line*> keys;
    for (const Line& line : lines) {
        const bool namesZ = line.text.find("'z'") != std::string::npos
            || line.text.find("'Z'") != std::string::npos;
        // An alphabet range names both of its ends; a key binding names one.
        const bool isRange = line.text.find("'a'") != std::string::npos
            || line.text.find("'A'") != std::string::npos;
        const std::string flat = withoutSpaces(lowercased(line.text));
        const bool spellsIt = flat.find("cmd+z") != std::string::npos
            || flat.find("command+z") != std::string::npos
            || flat.find("ctrl+z") != std::string::npos
            || flat.find("control+z") != std::string::npos;
        if ((namesZ && !isRange) || spellsIt)
            keys.push_back(&line);
    }
    for (const Line* key : keys)
        testkit::fail("a key for Undo or Redo is back at " + where(*key) + ": " + key->text,
                      __FILE__, __LINE__);
    CHECK(keys.empty());

    // --- the hidden things have exactly the sightings they should ---
    // The global History window: declared, defined, and opened by nobody.
    // Its frame counts too, because constructing and showing that is the
    // whole of what opening it does.
    anchor(lines, "openHistoryWindow(",
           { "void openHistoryWindow();", "void MainComponent::openHistoryWindow()" });
    anchor(lines, "HistoryWindowHost(", { "explicit HistoryWindowHost(HistoryWindow& view)" });
    anchor(lines, "HistoryWindowHost>(",
           { "historyHost = std::make_unique<HistoryWindowHost>(" });
    anchor(lines, "historyHost->setVisible(", { "historyHost->setVisible(true);" });
    // The bottom pane's strip is built from the three titles, and the tab a
    // player clicks is turned into a face by the one cast that may exist.
    anchor(lines, "TabStrip bottomTabs",
           { "TabStrip bottomTabs { bottompane::tabTitles(), TabStrip::Lead::label };" });
    anchor(lines, "static_cast<bottompane::Face>",
           { "showBottomTab(static_cast<bottompane::Face>(index));" });
    // The menu is one action: About. A second one is a menu growing back.
    anchor(lines, "AppMenu::Actions",
           { "appMenu = std::make_unique<AppMenu>(AppMenu::Actions { .about = [this] { showAbout(); } });" });

    return testkit::summary("hidden_features_tests");
}
