// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The banner model, tested from the theory of issue #3 — the two observed
// leaks replayed as tests, plus the ownership rules that make them
// impossible: a scan re-render must never resurrect a dismissed error, and
// recovery clears exactly its own lane, nothing else's.

#include "support.hpp"

#include "../app/BannerModel.h"

#include <algorithm>

using namespace loopercat;
using banners::Line;
using banners::Model;
using banners::Source;
using commands::Finding;
using lifecycle::State;

namespace {

bool anyLineContains(const Model& model, const std::string& needle)
{
    const std::vector<Line> lines = model.lines();
    return std::any_of(lines.begin(), lines.end(), [&needle](const Line& line) {
        return line.text.find(needle) != std::string::npos;
    });
}

} // namespace

int main()
{
    // --- leak #1 replay: a dismissed job error must not come back when a
    // lifecycle transition re-renders the strip ---

    {
        Model m;
        m.scan(State::connected, {});
        m.showError(Source::job, "Push loop.wav to slot 3: disk full");
        CHECK(anyLineContains(m, "disk full"));

        m.dismiss();
        CHECK(!anyLineContains(m, "disk full"));

        // The observed resurrection path: disconnect, then reconnect — each
        // transition re-renders the strip. Dismissed stays dismissed.
        m.scan(State::disconnected, {});
        CHECK(!anyLineContains(m, "disk full"));
        m.scan(State::connected, {});
        CHECK(!anyLineContains(m, "disk full"));

        // A NEW error event (not a re-render) does show again.
        m.showError(Source::job, "Push loop.wav to slot 3: disk full");
        CHECK(anyLineContains(m, "disk full"));
    }

    // --- leak #2 replay: the ghost-cleanup warning must not survive the
    // reconnect that healed it ---

    {
        Model m;
        m.scan(State::connected, {});
        m.scan(State::ghost, {});
        CHECK(anyLineContains(m, "without an eject")); // the lifecycle line explains the ghost

        m.showError(Source::connection, "Could not clear the stale mount");
        m.scan(State::disconnected, {}); // cleanup finally unmounted the ghost
        CHECK(anyLineContains(m, "stale mount")); // the cure is still pending — line stays

        m.scan(State::connected, {}); // the pedal is honestly back
        CHECK(!anyLineContains(m, "stale mount"));
        CHECK(!anyLineContains(m, "without an eject"));
    }

    // --- recovery clears exactly its own lane ---

    {
        Model m;
        m.scan(State::connected, {});
        m.showError(Source::job, "Trim slot 5: file vanished");
        m.showError(Source::connection, "The pedal's card would not mount");
        m.scan(State::disconnected, {});
        m.scan(State::connected, {}); // reconnect: connection recovery
        CHECK(!anyLineContains(m, "would not mount"));
        CHECK(anyLineContains(m, "file vanished")); // the mutation story is not resolved by a remount

        m.clearJobError(); // a later mutation succeeded
        CHECK(!anyLineContains(m, "file vanished"));
    }

    // --- an eject refusal must NOT count as recovery: ejecting -> connected
    // keeps the line explaining why Disconnect failed ---

    {
        Model m;
        m.scan(State::connected, {});
        m.scan(State::ejecting, {});
        m.showError(Source::connection, "The volume would not eject");
        m.scan(State::connected, {}); // the machine lands back in connected
        CHECK(anyLineContains(m, "would not eject"));

        // The next eject succeeds: a clean eject IS recovery for this lane.
        m.scan(State::ejecting, {});
        m.scan(State::ejected, {});
        CHECK(!anyLineContains(m, "would not eject"));
    }

    // --- one line per lane, latest wins ---

    {
        Model m;
        m.showError(Source::job, "first");
        m.showError(Source::job, "second");
        const std::vector<Line> lines = m.lines();
        CHECK_EQ(lines.size(), static_cast<std::size_t>(1));
        CHECK(!anyLineContains(m, "first"));
        CHECK(anyLineContains(m, "second"));
    }

    // --- render order and flags: errors first (connection, then job), then
    // the lifecycle line, then findings; only the error lines dismiss ---

    {
        Model m;
        m.scan(State::ejected, { { commands::Level::warn, "junk on the card" } });
        m.showError(Source::job, "job line");
        m.showError(Source::connection, "connection line");
        const std::vector<Line> lines = m.lines();
        CHECK_EQ(lines.size(), static_cast<std::size_t>(4));
        CHECK_EQ(lines.at(0).text, "connection line");
        CHECK(lines.at(0).dismissible);
        CHECK_EQ(lines.at(1).text, "job line");
        CHECK(lines.at(1).dismissible);
        CHECK(lines.at(2).text.find("safe to disconnect") != std::string::npos);
        CHECK(!lines.at(2).dismissible);
        CHECK_EQ(lines.at(3).text, "junk on the card");
        CHECK(!lines.at(3).dismissible);
        CHECK(m.hasDismissible());

        m.dismiss();
        CHECK_EQ(m.lines().size(), static_cast<std::size_t>(2));
        CHECK(!m.hasDismissible());
    }

    // --- the lifecycle line follows the state: ghost and ejected speak, the
    // quiet states stay silent ---

    {
        Model m;
        m.scan(State::ghost, {});
        CHECK_EQ(m.lines().size(), static_cast<std::size_t>(1));
        CHECK(m.lines().at(0).level == commands::Level::error);
        m.scan(State::ejected, {});
        CHECK_EQ(m.lines().size(), static_cast<std::size_t>(1));
        CHECK(m.lines().at(0).level == commands::Level::info);
        for (const State quiet : { State::disconnected, State::connected, State::ejecting }) {
            m.scan(quiet, {});
            CHECK_EQ(m.lines().size(), static_cast<std::size_t>(0));
        }
    }

    // --- findings are replaced wholesale by every scan, never accumulated ---

    {
        Model m;
        m.scan(State::connected, { { commands::Level::warn, "one" },
                                   { commands::Level::warn, "two" } });
        CHECK_EQ(m.lines().size(), static_cast<std::size_t>(2));
        m.scan(State::connected, { { commands::Level::warn, "three" } });
        CHECK_EQ(m.lines().size(), static_cast<std::size_t>(1));
        CHECK(anyLineContains(m, "three"));
        CHECK(!anyLineContains(m, "one"));
        m.scan(State::connected, {});
        CHECK_EQ(m.lines().size(), static_cast<std::size_t>(0));
    }

    // --- fail-fast: an empty error text is a caller bug, not a blank line ---

    {
        Model m;
        CHECK_THROWS(m.showError(Source::job, ""), "must not be empty");
        CHECK_THROWS(m.showError(Source::connection, ""), "must not be empty");
        CHECK_EQ(m.lines().size(), static_cast<std::size_t>(0));
    }

    return testkit::summary("banners");
}
