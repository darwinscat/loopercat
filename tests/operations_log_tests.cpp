// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat - see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// operations.log keeps a line byte for byte. A connect step's line is the
// app's words around a reason that is the core's or the gate's UTF-8, or a
// pedal's name as the OS gave it; the 0.9.6 previews wrote the dash and the
// quotes around such a reason as Latin-1 mojibake. The theory: what goes in
// as text comes out as the same UTF-8 bytes, the app's own dash included.

#include "support.hpp"

#include "../app/OperationsLog.h"
#include "../app/Strings.h"

#include <loopercat/Commands.hpp>

#include <chrono>
#include <filesystem>
#include <string>

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path()
        / ("loopercat-oplog-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { fs::create_directories(path); }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec)
            testkit::fail("scratch folder left behind: " + ec.message(), __FILE__, __LINE__);
    }
};

std::string logBytes(const fs::path& dir)
{
    return commands::readFileBytes(dir / "operations.log");
}

} // namespace

int main()
{
    TempDir tmp;
    const juce::File dir(utf8(tmp.path.u8string()));

    // A refusal's reason with the pedal's name in curly quotes, after the
    // app's own dash: the line as the connect step builds it.
    const std::string reason = "the pedal \xe2\x80\x9cKitty \xf0\x9f\x90\xb1\xe2\x80\x9d is busy"; // utf8-data: a gate's reason with a pedal's name
    oplog::append(dir, juce::String::fromUTF8("connect: refused, nothing sent \xe2\x80\x94 ") + utf8(reason));
    // A name with an accent, as the OS lists a MIDI port.
    const std::string port = "Clavier caf\xc3\xa9"; // utf8-data: a MIDI port's name
    oplog::append(dir, juce::String::fromUTF8("connect: chose \xe2\x80\x9c") + utf8(port)
                           + juce::String::fromUTF8("\xe2\x80\x9d"));

    const std::string bytes = logBytes(tmp.path);
    const std::string first = "connect: refused, nothing sent \xe2\x80\x94 " + reason + "\n"; // utf8-data: the bytes the line must hold
    const std::string second = "connect: chose \xe2\x80\x9c" + port + "\xe2\x80\x9d\n"; // utf8-data: the bytes the line must hold
    // Each line is "<date> <time>  <text>": the stamp, two spaces, the text.
    const std::size_t firstEnd = bytes.find('\n');
    CHECK(firstEnd != std::string::npos);
    if (firstEnd != std::string::npos) {
        const std::string one = bytes.substr(0, firstEnd + 1);
        const std::string two = bytes.substr(firstEnd + 1);
        CHECK(one.size() > first.size() && one.compare(one.size() - first.size(), first.size(), first) == 0);
        CHECK(two.size() > second.size() && two.compare(two.size() - second.size(), second.size(), second) == 0);
        CHECK_EQ(one.substr(19, 2), std::string("  ")); // "YYYY-MM-DD HH:MM:SS" and the gap
        CHECK_EQ(one.size(), 21 + first.size());         // nothing else on the line
    }
    // Read back as text, the line is the text that went in.
    const juce::StringArray lines = juce::StringArray::fromLines(dir.getChildFile("operations.log").loadFileAsString());
    CHECK(lines.size() >= 2 && lines[0].endsWith(juce::String::fromUTF8("nothing sent \xe2\x80\x94 ") + utf8(reason)));

    return testkit::summary("operations_log_tests");
}
