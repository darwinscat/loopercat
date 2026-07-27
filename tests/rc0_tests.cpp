// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The byte-surgical memory model, attacked from the format's theory: strict
// structure checks, exact-tag matching, the 12-char name rules, the factory
// slot pinned to golden values, and the module's central invariant — any byte
// we did not explicitly change is reproduced exactly.

#include "support.hpp"

#include <loopercat/Rc0.hpp>

using namespace loopercat;

static const std::string FILE_TEXT = testkit::syntheticMemoryText();

// The canonical RC-5 root opener, exactly as hardware writes it (mirrored by
// syntheticMemoryText). The family-guard tests swap it for foreign variants.
static const std::string RC5_HEADER = "<database name=\"RC-5\" revision=\"0\">";

// FILE_TEXT with the root opener replaced — everything else stays byte-valid,
// so any refusal is attributable to the header alone.
static std::string withHeader(const std::string& header)
{
    std::string text = FILE_TEXT;
    const auto at = text.find(RC5_HEADER);
    if (at == std::string::npos)
        throw loopercat::Error("test fixture: canonical header not found");
    return text.replace(at, RC5_HEADER.size(), header);
}

static std::string section(const std::string& body, const std::string& tag)
{
    const auto open = body.find("<" + tag + ">");
    const auto close = body.find("</" + tag + ">", open);
    if (open == std::string::npos || close == std::string::npos)
        throw loopercat::Error("test fixture: missing section <" + tag + ">");
    return body.substr(open, close - open + tag.size() + 3);
}

int main()
{
    // --- constants pinned to golden ---

    {
        const auto& g = testkit::golden();
        CHECK_EQ(rc0::kNameLength, g.at("constants").at("nameLength").get<int>());
        CHECK_EQ(rc0::kSlotCount, g.at("constants").at("slotCount").get<int>());
        CHECK_EQ(static_cast<int>(rc0::tailMarkerFor(1)), g.at("tailMarkers").at("memory1").get<int>());
        CHECK_EQ(static_cast<int>(rc0::tailMarkerFor(2)), g.at("tailMarkers").at("memory2").get<int>());
        CHECK_THROWS(rc0::tailMarkerFor(3), "must be 1 or 2");
        CHECK_THROWS(rc0::tailMarkerFor(0), "must be 1 or 2");
    }

    // --- structure ---

    CHECK_THROWS(rc0::assertMemoryFile(testkit::syntheticMemoryText(0x38, 98)), "expected 99");
    CHECK_THROWS(rc0::assertMemoryFile("<database>"), "missing </database>");
    rc0::assertMemoryFile(FILE_TEXT); // the synthetic file itself is sound

    CHECK_THROWS(rc0::slotBody(FILE_TEXT, 0), "out of range");
    CHECK_THROWS(rc0::slotBody(FILE_TEXT, 100), "out of range");

    // --- family guard (issue #35): a foreign RC-series card is refused by name ---

    // An RC-500 card is byte-plausible RC-5: same 99 mems, same field names.
    // Only the root name attribute gives it away (header shape from the
    // boss-rc500-editor template quoted in issue #35) — the refusal must name
    // the intruder, not report a "broken" card.
    CHECK_THROWS(rc0::assertMemoryFile(withHeader("<database name=\"RC-500\" revision=\"0\">")),
                 "RC-500");

    // The header claims RC-5 but a two-track section is present: the card
    // lies about its family, and <TRACK2> outs it.
    {
        std::string liar = FILE_TEXT;
        const auto at = liar.find("</TRACK1>");
        CHECK(at != std::string::npos);
        liar.insert(at + std::string("</TRACK1>").size(),
                    "\n<TRACK2>\n\t<Rev>0</Rev>\n</TRACK2>");
        CHECK_THROWS(rc0::assertMemoryFile(liar), "TRACK2");
    }

    // A root with no name attribute at all is not an RC0 memory file.
    CHECK_THROWS(rc0::assertMemoryFile(withHeader("<database revision=\"0\">")), "not an RC0");
    CHECK_THROWS(rc0::assertMemoryFile(withHeader("<database>")), "not an RC0");

    // Exact bytes only: trailing space, case folding, single quotes and an
    // empty name are all foreign — the pedal's XML is machine-written.
    CHECK_THROWS(rc0::assertMemoryFile(withHeader("<database name=\"RC-5 \" revision=\"0\">")),
                 "not an RC-5");
    CHECK_THROWS(rc0::assertMemoryFile(withHeader("<database name=\"rc-5\" revision=\"0\">")),
                 "not an RC-5");
    CHECK_THROWS(rc0::assertMemoryFile(withHeader("<database name=\"\" revision=\"0\">")),
                 "not an RC-5");
    CHECK_THROWS(rc0::assertMemoryFile(withHeader("<database NAME=\"RC-5\" revision=\"0\">")),
                 "not an RC0");
    CHECK_THROWS(rc0::assertMemoryFile(withHeader("<database name='RC-5' revision=\"0\">")),
                 "not an RC0");

    // A tag that merely BEGINS with "database" is not the root we trust.
    CHECK_THROWS(rc0::assertMemoryFile(withHeader("<databaseX name=\"RC-5\" revision=\"0\">")),
                 "not an RC0");

    // The attribute is read from the ROOT opener only: an RC-5-flavored tag
    // buried in a memory body cannot vouch for an RC-500 root.
    {
        std::string smuggled = withHeader("<database name=\"RC-500\" revision=\"0\">");
        const auto at = smuggled.find("<mem id=\"0\">");
        CHECK(at != std::string::npos);
        smuggled.insert(at + std::string("<mem id=\"0\">").size(), "<database name=\"RC-5\">");
        CHECK_THROWS(rc0::assertMemoryFile(smuggled), "RC-500");
    }

    // Nor can a comment smuggle the opener in front of the real root: only
    // the XML declaration and whitespace may precede <database. The pedal
    // never writes comments, so a commented prelude is foreign either way.
    {
        const std::string comment = "\n<!-- <database name=\"RC-5\" revision=\"0\"> -->";
        std::string commented = withHeader("<database name=\"RC-500\" revision=\"0\">");
        const std::string decl = "<?xml version=\"1.0\" encoding=\"utf-8\"?>";
        commented.insert(commented.find(decl) + decl.size(), comment);
        CHECK_THROWS(rc0::assertMemoryFile(commented), "not an RC0");
        std::string commentedRc5 = FILE_TEXT;
        commentedRc5.insert(commentedRc5.find(decl) + decl.size(), comment);
        CHECK_THROWS(rc0::assertMemoryFile(commentedRc5), "not an RC0");
    }

    // Arbitrary bytes before the declaration are no prelude we accept.
    CHECK_THROWS(rc0::assertMemoryFile("GARBAGE" + FILE_TEXT), "not an RC0");

    // Pinned decision: the guard checks the ROOT OPENER, not the declaration —
    // a declaration-less document whose first element is the canonical RC-5
    // root still parses (splitFile/assertMemoryFile never required the
    // declaration before the guard existed either).
    {
        std::string noDecl = FILE_TEXT.substr(FILE_TEXT.find(RC5_HEADER));
        rc0::assertMemoryFile(noDecl);
    }

    // --- fields ---

    CHECK_THROWS(rc0::field("<a>1</a>", "b"), "occurs 0 times");
    CHECK_THROWS(rc0::field("<Level>1</Level><Level>2</Level>", "Level"), "occurs 2 times");
    CHECK_EQ(rc0::field(rc0::slotBody(FILE_TEXT, 1), "Pan"), 50);

    // Negative values parse (the format allows them).
    CHECK_EQ(rc0::field("<Pan>-3</Pan>", "Pan"), -3);

    // A tag whose content is not an integer is no match at all.
    CHECK_THROWS(rc0::field("<NAME>\n<C01>65</C01>\n</NAME>", "NAME"), "occurs 0 times");

    // A digit run that overflows the value type is a malformed file, reported
    // as our own typed error.
    CHECK_THROWS(rc0::field("<WavLen>99999999999999999999</WavLen>", "WavLen"), "out of range");

    // Field name matching is exact, not a prefix: <Measure> never touches
    // <MeasLen> or <MeasMod>.
    {
        const std::string body = rc0::setField(rc0::slotBody(FILE_TEXT, 1), "Measure", 263);
        CHECK_EQ(rc0::field(body, "Measure"), 263);
        CHECK_EQ(rc0::field(body, "MeasLen"), 0);
        CHECK_EQ(rc0::field(body, "MeasMod"), 1);
    }

    // setField touches nothing but the one field's bytes.
    {
        const std::string body = rc0::slotBody(FILE_TEXT, 1);
        const std::string edited = rc0::setField(body, "Pan", 51);
        const auto at = edited.find("<Pan>51</Pan>");
        CHECK(at != std::string::npos);
        CHECK_EQ(edited.substr(0, at), body.substr(0, body.find("<Pan>50</Pan>")));
        CHECK_EQ(edited.substr(at + 13), body.substr(body.find("<Pan>50</Pan>") + 13));
    }

    // --- names ---

    CHECK_THROWS(rc0::encodeName("ThirteenChars"), "longer than 12");
    CHECK_THROWS(rc0::encodeName(""), "non-empty");
    CHECK_THROWS(rc0::encodeName("K\xd0\x9eTIK"), "ASCII"); // non-ASCII bytes
    CHECK_THROWS(rc0::encodeName("tab\there"), "ASCII");    // control chars
    CHECK_EQ(rc0::encodeName("Cold Gaze"), "Cold Gaze   ");

    // The golden name encoding: exact char codes captured from hardware.
    {
        const auto& fixture = testkit::golden().at("nameEncoding");
        const std::string name = fixture.at("name").get<std::string>();
        const std::string encoded = rc0::encodeName(name);
        const auto& codes = fixture.at("codes");
        for (std::size_t i = 0; i < codes.size(); ++i)
            CHECK_EQ(static_cast<int>(static_cast<unsigned char>(encoded[i])), codes[i].get<int>());
    }

    // Round-trip through encode/set/decode.
    {
        const std::string body = rc0::setName(rc0::slotBody(FILE_TEXT, 7), "Deep Space 1");
        CHECK_EQ(rc0::decodeName(body), "Deep Space 1");
    }

    CHECK_THROWS(rc0::decodeName("<C01>65</C01>"), "expected 12");
    CHECK_THROWS(rc0::setName(rc0::slotBody(FILE_TEXT, 1), "\xd0\x9a\xd0\x9e\xd0\xa2"), "ASCII");
    CHECK_THROWS(rc0::setName("<TRACK1></TRACK1>", "Name"), "missing <NAME>");

    // A char code that cannot be a byte is refused, not silently masked.
    {
        std::string weird = "<NAME>\n";
        for (int i = 1; i <= 12; ++i) {
            const std::string tag = "C" + std::string(i < 10 ? "0" : "") + std::to_string(i);
            weird += "\t<" + tag + ">" + (i == 1 ? "999" : "65") + "</" + tag + ">\n";
        }
        weird += "</NAME>";
        CHECK_THROWS(rc0::decodeName(weird), "out of byte range");
    }

    CHECK_EQ(rc0::defaultSlotName(7), "Memory07");
    CHECK_EQ(rc0::defaultSlotName(42), "Memory42");

    // --- the byte-untouched invariant ---

    // Renaming one slot leaves every other byte of the file untouched.
    {
        const std::string body = rc0::slotBody(FILE_TEXT, 42);
        const std::string renamed = rc0::replaceSlotBody(FILE_TEXT, 42, rc0::setName(body, "New Name"));
        CHECK(renamed != FILE_TEXT);
        int changedOtherSlots = 0;
        for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
            if (slot != 42 && rc0::slotBody(renamed, slot) != rc0::slotBody(FILE_TEXT, slot))
                ++changedOtherSlots;
        CHECK_EQ(changedOtherSlots, 0);
        CHECK_EQ(rc0::splitFile(renamed).tail, rc0::splitFile(FILE_TEXT).tail);
    }

    // Putting a slot's own body back reproduces the file byte-for-byte.
    CHECK(rc0::replaceSlotBody(FILE_TEXT, 13, rc0::slotBody(FILE_TEXT, 13)) == FILE_TEXT);

    // --- factory slot, pinned to golden ---

    {
        const auto& factory = testkit::golden().at("factorySlot");
        const std::string body = rc0::factorySlotBody(5);
        CHECK_EQ(rc0::decodeName(body), "Memory05    ");
        for (const auto& sectionName : { "TRACK1", "MASTER", "RHYTHM" }) {
            const std::string sec = section(body, sectionName);
            for (const auto& [key, value] : factory.at(sectionName).items())
                CHECK_EQ(rc0::field(sec, key), value.get<long long>());
        }
        // Setting the factory name back reproduces the factory body exactly —
        // proves our name-block formatting matches the pedal's byte-for-byte.
        CHECK(rc0::setName(body, "Memory05") == body);
    }

    CHECK_THROWS(rc0::factorySlotBody(0), "out of range");
    CHECK_THROWS(rc0::factorySlotBody(100), "out of range");

    // --- trailer ---

    {
        CHECK_EQ(static_cast<int>(rc0::tailMarker(FILE_TEXT).value()), 0x38);
        const std::string asMemory2 = rc0::setTailMarker(FILE_TEXT, 2);
        CHECK_EQ(static_cast<int>(rc0::tailMarker(asMemory2).value()), 0x39);
        CHECK_EQ(rc0::splitFile(asMemory2).document, rc0::splitFile(FILE_TEXT).document);
    }

    // The trailer byte is a write-generation counter (raw byte increment, no
    // decimal carry) — hardware-observed 2026-07-24: a pedal-side save wrote
    // 0x3a, one past '9'. Any generation must stamp and read back verbatim.
    {
        const std::string atPedalGeneration = rc0::setTailGeneration(FILE_TEXT, 0x3a);
        CHECK_EQ(static_cast<int>(rc0::tailMarker(atPedalGeneration).value()), 0x3a);
        CHECK_EQ(rc0::splitFile(atPedalGeneration).document, rc0::splitFile(FILE_TEXT).document);
        // Wrapping and non-printable generations are bytes like any other.
        CHECK_EQ(static_cast<int>(rc0::tailMarker(rc0::setTailGeneration(FILE_TEXT, 0x00)).value()),
                 0x00);
        CHECK_EQ(static_cast<int>(rc0::tailMarker(rc0::setTailGeneration(FILE_TEXT, 0xff)).value()),
                 0xff);
    }

    // An unrecognized trailer is reported and refused, never silently rewritten.
    {
        const std::string oddTail = rc0::splitFile(FILE_TEXT).document + "\nGARBAGE";
        CHECK(!rc0::tailMarker(oddTail).has_value());
        CHECK_THROWS(rc0::setTailMarker(oddTail, 1), "refusing");
        CHECK_THROWS(rc0::setTailGeneration(oddTail, 0x3a), "refusing");
    }

    CHECK_THROWS(rc0::setTailMarker(FILE_TEXT, 3), "must be 1 or 2");

    return testkit::summary("rc0");
}
