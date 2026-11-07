// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat - see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The exact-changed-set view of a memory write: what one mutation did to a
// memory, named field by field, with every other byte of the document
// asserted identical. A field's value is the only unit a mutation may
// change; everything else about the body is the pedal's, byte for byte.
// Shared by the suites that hold a write to that standard.

#pragma once

#include "support.hpp"

#include <loopercat/Rc0.hpp>

#include <optional>
#include <string>
#include <vector>

namespace testkit {

// One line of a memory body is one field — a tab, then <Tag>value</Tag> — or
// one section's opener or closer on a line of its own: the shape every RC-5
// writes (the card fixture; golden.json's factory slot). A field line taken
// apart, or nothing for any other line.
struct FieldLine {
    std::string tag;
    long long value;
};

inline std::optional<FieldLine> parseFieldLine(const std::string& line)
{
    if (line.size() < 2 || line[0] != '\t' || line[1] != '<')
        return std::nullopt;
    const auto tagEnd = line.find('>', 2);
    if (tagEnd == std::string::npos)
        return std::nullopt;
    const std::string tag = line.substr(2, tagEnd - 2);
    const std::string close = "</" + tag + ">";
    const std::size_t valueBegin = tagEnd + 1;
    if (line.size() < valueBegin + close.size()
        || line.compare(line.size() - close.size(), close.size(), close) != 0)
        return std::nullopt;
    const std::string digits = line.substr(valueBegin, line.size() - close.size() - valueBegin);
    std::size_t i = (!digits.empty() && digits[0] == '-') ? 1 : 0;
    if (i == digits.size())
        return std::nullopt;
    for (; i < digits.size(); ++i)
        if (digits[i] < '0' || digits[i] > '9')
            return std::nullopt;
    return FieldLine { tag, std::stoll(digits) };
}

// What changed between two bodies of one memory, as "SECTION.Tag before->after"
// in the body's own order — or "shape" when the bodies differ in anything but
// field values: a line added or lost, a tag renamed, a line outside any
// section touched. A field's value is the only unit a mutation may change;
// everything else about the body is the pedal's, byte for byte.
inline std::string fieldChanges(const std::string& before, const std::string& after)
{
    const auto lines = [](const std::string& text) {
        std::vector<std::string> out;
        std::size_t from = 0;
        while (true) {
            const auto nl = text.find('\n', from);
            if (nl == std::string::npos) {
                out.push_back(text.substr(from));
                return out;
            }
            out.push_back(text.substr(from, nl - from));
            from = nl + 1;
        }
    };
    const std::vector<std::string> a = lines(before);
    const std::vector<std::string> b = lines(after);
    if (a.size() != b.size())
        return "shape";
    std::string section;
    std::string out;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) {
            // <X> on a line of its own opens section X; </X> closes it.
            if (a[i].size() > 2 && a[i].front() == '<' && a[i].back() == '>'
                && a[i].find('<', 1) == std::string::npos) {
                if (a[i][1] == '/')
                    section.clear();
                else
                    section = a[i].substr(1, a[i].size() - 2);
            }
            continue;
        }
        const auto fa = parseFieldLine(a[i]);
        const auto fb = parseFieldLine(b[i]);
        if (!fa || !fb || fa->tag != fb->tag || section.empty())
            return "shape";
        if (!out.empty())
            out += ", ";
        out += section + "." + fa->tag + " " + std::to_string(fa->value) + "->"
             + std::to_string(fb->value);
    }
    return out;
}

// What one mutation of `slot` did to the memory document: that memory's field
// changes, with every byte outside the memory asserted identical (the trailer
// aside — every write stamps it).
inline std::string slotChangesOnCard(const std::string& before, const std::string& after, int slot)
{
    const std::string afterBody = loopercat::rc0::slotBody(after, slot);
    CHECK(loopercat::rc0::splitFile(loopercat::rc0::replaceSlotBody(before, slot, afterBody)).document
          == loopercat::rc0::splitFile(after).document);
    return fieldChanges(loopercat::rc0::slotBody(before, slot), afterBody);
}

} // namespace testkit
