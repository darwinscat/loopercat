// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What the screen may offer on a card, read off the profile table by the
// card's own name (app/CardPermissions.h). The rules, and what these try to
// break:
//
//   - every switch follows the one operation it runs, and nothing else: a
//     model with rename open and tempo closed edits the name and not the tempo
//   - a model opened in part says, in one sentence built from the table,
//     what is open — the same words wherever a closed control stands
//   - a model the table does not know offers nothing and says nothing
//   - a pane that can write under its controls promises the change will be
//     heard; one that cannot says why instead

#include "support.hpp"

#include "../app/CardPermissions.h"

#include <string>

using namespace loopercat;

int main()
{
    const CardPermissions rc5 = CardPermissions::of("RC-5");
    CHECK(rc5.rename && rc5.tempo && rc5.oneShot && rc5.countIn && rc5.push && rc5.swap
          && rc5.normalize && rc5.clear && rc5.trim && rc5.rhythm && rc5.playStop && rc5.pull
          && rc5.restore && rc5.downmix);
    CHECK(rc5.closed.empty());
    CHECK_EQ(rc5.footer(true), std::string("Disconnect to hear the changes."));

    const CardPermissions rc500 = CardPermissions::of("RC-500");
    CHECK(rc500.rename && rc500.pull && rc500.clear && rc500.restore);
    CHECK(!rc500.tempo && !rc500.oneShot && !rc500.countIn && !rc500.push && !rc500.swap
          && !rc500.normalize && !rc500.trim && !rc500.rhythm && !rc500.playStop && !rc500.downmix);
    CHECK(rc500.anyWrite()); // a rename is a write; the sweep and the face follow it
    const std::string sentence = "On the RC-500, LooperCat does rename, pull, clear and restore only";
    CHECK_EQ(rc500.closed, sentence);
    // The inspector can rename: it promises the change, and says what stays closed.
    CHECK_EQ(rc500.footer(true), "Disconnect to hear the changes. " + sentence + ".");
    // The drums are closed: no promise, only why.
    CHECK_EQ(rc500.footer(false), sentence);

    for (const char* name : { "RC-505", "", "rc-500", "RC-500 " }) {
        const CardPermissions none = CardPermissions::of(name);
        CHECK(!none.anyWrite() && !none.pull && !none.restore);
        CHECK(none.closed.empty());
        CHECK_EQ(none.footer(false), CardPermissions::writesOnlyTo());
    }
    CHECK_EQ(CardPermissions::writesOnlyTo(), std::string("LooperCat writes only to the RC-5 and the RC-500"));

    return testkit::summary("card_permissions");
}
