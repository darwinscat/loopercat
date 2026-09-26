// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pedal book (issue #98), from what it promises:
//
//   - an endpoint met before is named from its card; one not met is offered
//     honestly, by model and endpoint id, never by a made-up name
//   - a second sighting of an endpoint replaces the first
//   - a rename on a card reaches every endpoint that carried it
//   - the book survives a round trip through its text form byte for byte
//   - a broken book is refused with the line named, not read on a guess
//   - fields that would break the text form are refused when remembered

#include "support.hpp"

#include "../app/PedalBook.h"

using namespace loopercat;

int main()
{
    // --- empty book: first meetings only ---
    {
        pedalbook::Book book;
        CHECK(!book.find("883557304").has_value());
        const std::string label = pedalbook::choiceLabel("RC-5", "883557304", book.find("883557304"));
        CHECK(label.find("RC-5") != std::string::npos);
        CHECK(label.find("883557304") != std::string::npos);
        CHECK(label.find("first meeting") != std::string::npos);
        CHECK(label.find("Drummer") == std::string::npos);
    }

    // --- met before: the card's name, and a second sighting replaces the first ---
    {
        pedalbook::Book book;
        book.remember({ "2016199893", "734978af-ff9f-42c3-9890-cd8b58ad458f", "RC-5 Drummer", "RC-5", 1000 });
        book.remember({ "883557304", "527a2b7a-da5c-4910-bc28-24c6f6acc44b", "RC-5 Kitty", "RC-5", 2000 });
        CHECK_EQ(book.entries().size(), 2u);
        CHECK_EQ(pedalbook::choiceLabel("RC-5", "2016199893", book.find("2016199893")), std::string("RC-5 Drummer"));
        CHECK_EQ(pedalbook::choiceLabel("RC-5", "883557304", book.find("883557304")), std::string("RC-5 Kitty"));
        // the same pedal carries another card now
        book.remember({ "883557304", "d273db4a-77a8-453f-bd60-d966f8653379", "RC-500 Bob", "RC-500", 3000 });
        CHECK_EQ(book.entries().size(), 2u);
        CHECK_EQ(book.find("883557304")->name, std::string("RC-500 Bob"));
        CHECK_EQ(book.find("883557304")->seenMs, 3000);
        CHECK_EQ(book.find("2016199893")->name, std::string("RC-5 Drummer")); // untouched
    }

    // --- a rename reaches every endpoint that carried the card ---
    {
        pedalbook::Book book;
        book.remember({ "2016199893", "card-A", "RC-5 Drummer", "RC-5", 1000 });
        book.remember({ "883557304", "card-A", "RC-5 Drummer", "RC-5", 2000 }); // the card moved pedals
        book.remember({ "904629978", "card-B", "RC-500 Bob", "RC-500", 3000 });
        book.renamed("card-A", "RC-5 Groove");
        CHECK_EQ(book.find("2016199893")->name, std::string("RC-5 Groove"));
        CHECK_EQ(book.find("883557304")->name, std::string("RC-5 Groove"));
        CHECK_EQ(book.find("904629978")->name, std::string("RC-500 Bob"));
        CHECK_THROWS(book.renamed("card-A", ""), "needs its name");
    }

    // --- round trip through the text form ---
    {
        pedalbook::Book book;
        book.remember({ "2016199893", "734978af", "RC-5 Drummer", "RC-5", 1758836825000 });
        book.remember({ "904629978", "d273db4a", "RC-500 Bob \xf0\x9f\x90\xb1", "RC-500", 1758836826000 }); // utf-8 name
        const std::string text = book.serialize();
        CHECK(text.find("RC-5 Drummer") != std::string::npos);
        const pedalbook::Book back = pedalbook::Book::parse(text);
        CHECK_EQ(back.entries().size(), 2u);
        CHECK_EQ(back.serialize(), text);
        CHECK_EQ(back.find("904629978")->name, std::string("RC-500 Bob \xf0\x9f\x90\xb1"));
        CHECK_EQ(back.find("2016199893")->seenMs, 1758836825000);
        CHECK(pedalbook::Book::parse("").entries().empty());
        CHECK(pedalbook::Book::parse("\n\n").entries().empty());
    }

    // --- a broken book is refused with the line named ---
    {
        CHECK_THROWS(pedalbook::Book::parse("2016199893\tcard\tname\n"), "line 1");
        CHECK_THROWS(pedalbook::Book::parse("ok\tcard\tname\tRC-5\t1\n2016199893\tcard\tname\tRC-5\tsoon\n"), "line 2");
        CHECK_THROWS(pedalbook::Book::parse("\tcard\tname\tRC-5\t1\n"), "endpoint");
        CHECK_THROWS(pedalbook::Book::parse("e\tcard\t\tRC-5\t1\n"), "name");
        CHECK_THROWS(pedalbook::Book::parse("e\tcard\tname\tRC-5\t-1\n"), "not a number");
    }

    // --- fields that would break the text form are refused when remembered ---
    {
        pedalbook::Book book;
        CHECK_THROWS(book.remember({ "e", "c", "tab\there", "RC-5", 1 }), "control characters");
        CHECK_THROWS(book.remember({ "e", "c", "new\nline", "RC-5", 1 }), "control characters");
        CHECK_THROWS(book.remember({ "", "c", "name", "RC-5", 1 }), "endpoint");
        CHECK_THROWS(book.remember({ "e", "", "name", "RC-5", 1 }), "card id");
        CHECK_THROWS(book.remember({ "e", "c", "name", "", 1 }), "family");
        CHECK_THROWS(book.remember({ "e", "c", "name", "RC-5", -5 }), "before the epoch");
        CHECK(book.entries().empty()); // nothing half-remembered
    }

    return testkit::summary("pedal_book_tests");
}
