// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The middle cut of a long detail, from what it promises (#143):
//
//   - what fits is left alone, to the pixel
//   - what does not is cut once, in the middle: the head is still the
//     sentence's head, the tail its tail, and the whole measures within
//     the width — and keeps as much as the width allows
//   - the tail is never shorter than the head: "nothing to do" is the part
//     that matters
//   - the fit is measured, not counted: a wide letter takes its room
//   - when not even the ellipsis fits, the ellipsis alone comes back
//   - an empty sentence stays empty; a negative width is a mistake
//   - the cut lands between grapheme clusters, never inside one: an accent
//     stays on its letter, a joined emoji stays whole, a flag keeps both
//     its letters
//   - a sentence with its own ellipsis keeps it: one is put in, not one
//     in all

#include "support.hpp"

#include "../app/MiddleEllipsis.h"

#include <cstring>

using loopercat::elideMiddle;

namespace {

const juce::String ellipsis = juce::String::fromUTF8("\xe2\x80\xa6");

// A typewriter: every character ten wide, so the theory says exactly how
// many characters a width holds.
int typewriter(const juce::String& s) { return 10 * s.length(); }

// A face where W and M are twice as wide as the rest.
int proportional(const juce::String& s)
{
    int width = 0;
    for (const juce::juce_wchar c : s)
        width += (c == 'W' || c == 'M') ? 20 : 10;
    return width;
}

// One character by its code point, so a cluster reads as its parts.
juce::String cp(unsigned codePoint)
{
    return juce::String::charToString(static_cast<juce::juce_wchar>(codePoint));
}

// A string listed cluster by cluster, as Unicode has them (UAX #29), so the
// test can ask whether a cut kept whole clusters without segmenting
// anything itself.
using Clusters = std::vector<juce::String>;

juce::String join(const Clusters& clusters)
{
    juce::String out;
    for (const juce::String& cluster : clusters)
        out << cluster;
    return out;
}

// The head is a whole number of clusters from the start; the tail, from
// the end.
bool wholeClustersAtStart(const juce::String& head, const Clusters& clusters)
{
    juce::String built;
    for (const juce::String& cluster : clusters) {
        if (built == head)
            return true;
        built << cluster;
    }
    return built == head;
}
bool wholeClustersAtEnd(const juce::String& tail, const Clusters& clusters)
{
    juce::String built;
    for (auto it = clusters.rbegin(); it != clusters.rend(); ++it) {
        if (built == tail)
            return true;
        built = *it + built;
    }
    return built == tail;
}

int count(const juce::String& s, const juce::String& needle)
{
    int n = 0;
    for (int at = s.indexOf(needle); at >= 0; at = s.indexOf(at + 1, needle))
        ++n;
    return n;
}

// The one ellipsis splits the cut into the head and the tail it kept.
struct Pieces {
    juce::String head, tail;
};
Pieces pieces(const juce::String& cut)
{
    const int at = cut.indexOf(ellipsis);
    return { cut.substring(0, at), cut.substring(at + 1) };
}

} // namespace

int main()
{
    const juce::String detail =
        "already peaking at the -1 dBTP ceiling (measured -19.2 LUFS), nothing to do";
    const int full = typewriter(detail);

    // What fits is left alone: exactly at the width, and with room to spare.
    CHECK_EQ(elideMiddle(detail, full, typewriter), detail);
    CHECK_EQ(elideMiddle(detail, full + 1000, typewriter), detail);

    // One pixel short: one cut in the middle; one character goes and the
    // ellipsis takes its place, the tail still reads "nothing to do".
    {
        const juce::String cut = elideMiddle(detail, full - 1, typewriter);
        CHECK_EQ(count(cut, ellipsis), 1);
        const Pieces p = pieces(cut);
        CHECK(detail.startsWith(p.head));
        CHECK(detail.endsWith(p.tail));
        CHECK(p.head.isNotEmpty() && p.tail.isNotEmpty());
        CHECK(p.tail.length() >= p.head.length());
        CHECK(typewriter(cut) <= full - 1);
        CHECK_EQ(cut.length(), detail.length() - 1); // two characters out, the ellipsis in
        CHECK(cut.endsWith("nothing to do"));
    }

    // Room for thirty characters: twenty-nine of the sentence and the
    // ellipsis, the tail one longer than the head.
    {
        const juce::String cut = elideMiddle(detail, 300, typewriter);
        CHECK_EQ(count(cut, ellipsis), 1);
        CHECK_EQ(cut.length(), 30);
        CHECK_EQ(typewriter(cut), 300);
        const Pieces p = pieces(cut);
        CHECK(detail.startsWith(p.head));
        CHECK(detail.endsWith(p.tail));
        CHECK_EQ(p.head.length(), 14);
        CHECK_EQ(p.tail.length(), 15);
        CHECK(cut.endsWith("nothing to do"));
    }

    // Measured, not counted: ten W at twenty each need two hundred; a count
    // of characters would let nine through at a hundred, the face lets four.
    {
        const juce::String wide = "WWWWWWWWWW";
        const juce::String cut = elideMiddle(wide, 100, proportional);
        CHECK_EQ(count(cut, ellipsis), 1);
        CHECK(proportional(cut) <= 100);
        CHECK_EQ(cut.length(), 5); // four W and the ellipsis: 4 * 20 + 10
        const juce::String narrow = "iiiiiiiiii";
        CHECK_EQ(elideMiddle(narrow, 100, proportional), narrow); // ten i fit exactly
    }

    // Room for the ellipsis and nothing else: the ellipsis alone.
    CHECK_EQ(elideMiddle(detail, 10, typewriter), ellipsis);
    CHECK_EQ(elideMiddle(detail, 19, typewriter), ellipsis);

    // Not even the ellipsis fits: still the ellipsis alone, never the text.
    CHECK_EQ(elideMiddle(detail, 9, typewriter), ellipsis);
    CHECK_EQ(elideMiddle(detail, 0, typewriter), ellipsis);
    CHECK_EQ(elideMiddle("x", 0, typewriter), ellipsis);

    // Empty stays empty, at any width.
    CHECK_EQ(elideMiddle(juce::String(), 0, typewriter), juce::String());
    CHECK_EQ(elideMiddle(juce::String(), 500, typewriter), juce::String());

    // A negative width is a mistake, whatever the text.
    CHECK_THROWS(elideMiddle(detail, -1, typewriter), "negative");
    CHECK_THROWS(elideMiddle(juce::String(), -1, typewriter), "negative");

    // Characters, not bytes: a cut through accented letters and a dash is
    // still valid text, every kept character one of the sentence's own.
    {
        const juce::String accented = juce::String::fromUTF8(
            "na\xc3\xafve caf\xc3\xa9 \xe2\x80\x94 d\xc3\xa9j\xc3\xa0 vu, encore une fois");
        for (int width = 0; width <= typewriter(accented); width += 10) {
            const juce::String cut = elideMiddle(accented, width, typewriter);
            CHECK(typewriter(cut) <= width || cut == ellipsis);
            const char* bytes = cut.toRawUTF8();
            CHECK(juce::CharPointer_UTF8::isValidString(bytes, static_cast<int>(std::strlen(bytes))));
            CHECK_EQ(count(cut, ellipsis), cut == accented ? 0 : 1);
            if (cut != accented) {
                const Pieces p = pieces(cut);
                CHECK(accented.startsWith(p.head));
                CHECK(accented.endsWith(p.tail));
            }
        }
    }

    // The cut lands between grapheme clusters, never inside one, at every
    // width: a decomposed accent (the letter and its combining mark), a
    // family joined by zero-width joiners, two flags of regional indicators.
    {
        const juce::String acute = cp(0x0301), grave = cp(0x0300), zwj = cp(0x200D);
        const Clusters decomposed = { "d", "e" + acute, "j", "a" + grave, " ", "v", "u", " ",
                                      "c", "a", "f", "e" + acute };
        const Clusters family = { "t", "a", "k", "e", " ",
                                  cp(0x1F468) + zwj + cp(0x1F469) + zwj + cp(0x1F467),
                                  " ", "l", "o", "o", "p" };
        const Clusters flags = { cp(0x1F1EB) + cp(0x1F1F7), cp(0x1F1E9) + cp(0x1F1EA), " ",
                                 "t", "o", "u", "r" };
        for (const Clusters& clusters : { decomposed, family, flags }) {
            const juce::String whole = join(clusters);
            const int wholeWidth = typewriter(whole);
            for (int width = 0; width <= wholeWidth; ++width) {
                const juce::String cut = elideMiddle(whole, width, typewriter);
                if (width == wholeWidth) {
                    CHECK_EQ(cut, whole);
                    continue;
                }
                CHECK(cut != whole);
                CHECK_EQ(count(cut, ellipsis), 1);
                CHECK(typewriter(cut) <= width || cut == ellipsis);
                const Pieces p = pieces(cut);
                CHECK(wholeClustersAtStart(p.head, clusters));
                CHECK(wholeClustersAtEnd(p.tail, clusters));
            }
            // and it still keeps as much as the clusters allow: one character
            // short of the whole leaves most of the sentence, not the ellipsis
            CHECK(elideMiddle(whole, wholeWidth - 10, typewriter).length() >= whole.length() - 6);
        }
        // the family is one cluster: a width that cannot hold all of it holds
        // none of it
        const juce::String one = family[5];
        CHECK_EQ(elideMiddle(one, typewriter(one) - 1, typewriter), ellipsis);
        // seven characters' room out of nine: the first flag whole, the
        // ellipsis, and "tour" — never half a flag
        const juce::String two = join(flags);
        CHECK_EQ(elideMiddle(two, 70, typewriter), flags[0] + ellipsis + "tour");
        CHECK(!elideMiddle(two, 30, typewriter).startsWith(cp(0x1F1EB) + ellipsis));
    }

    // A sentence with its own ellipsis keeps it: exactly one is put in.
    {
        const juce::String own = "x" + ellipsis + "y0123456789abcdefghij";
        const juce::String cut = elideMiddle(own, 150, typewriter);
        CHECK_EQ(count(cut, ellipsis), 2);
        CHECK(cut.startsWith("x" + ellipsis + "y0123"));
        CHECK(cut.endsWith("defghij"));
        CHECK_EQ(typewriter(cut), 150);
        CHECK_EQ(elideMiddle(own, typewriter(own), typewriter), own);
    }

    return testkit::summary("middle_ellipsis_tests");
}
