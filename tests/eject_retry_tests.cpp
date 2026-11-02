// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What Disconnect does with a busy card (app/EjectRetry.h), tested from issue
// #169 — not from the driver's own shape. The failure it answers: the first
// Disconnect after a Connect was refused as busy while Spotlight indexed the
// card, the player saw a banner with a status number, and a second press a
// moment later went through. So the suite holds the eject to these promises:
//
//   - a busy refusal is tried again, the agreed number of times, the agreed
//     pause apart, and only then reported;
//   - any other refusal is reported at once: waiting it out only delays an
//     honest banner;
//   - a try that succeeds ends the eject as a success, however late it came,
//     and the journal says how many tries it took;
//   - the journal names a busy card's cause in words, never as 49168, and the
//     banner drops the number while keeping its sentence;
//   - the tries are answered from other threads, later: nothing may assume
//     the answer comes back inside the call that asked for it;
//   - all three platforms wait through this one policy, and none keeps a
//     ladder of its own underneath it (tries would multiply).

#include "support.hpp"

#include "../app/EjectRetry.h"

#include <chrono>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

using namespace loopercat::ejectretry;
using namespace std::chrono_literals;

namespace {

// A platform that answers each try from a script and records every pause.
// The pause runs its continuation at once: the policy is under test, not the
// clock.
struct Scripted {
    std::vector<Try> script;
    int attempts = 0;
    std::vector<std::chrono::milliseconds> pauses;
    std::vector<Outcome> outcomes;

    Attempt attempt()
    {
        return [this](Report report) {
            const std::size_t i = static_cast<std::size_t>(attempts++);
            // A try past the end of the script is a try the policy should not
            // have made; answer busy, so a runaway loop is caught by the count
            // instead of by the index.
            report(i < script.size() ? script[i] : Try { Answer::busy, "past the script" });
        };
    }
    Wait wait()
    {
        return [this](std::chrono::milliseconds pause, std::function<void()> then) {
            pauses.push_back(pause);
            then();
        };
    }
    std::function<void(Outcome)> done()
    {
        return [this](Outcome outcome) { outcomes.push_back(std::move(outcome)); };
    }
    void run() { drive(attempt(), wait(), done()); }
};

Try busy() { return { Answer::busy, "refused with status 49168" }; }

std::string readApp(const char* file)
{
    std::ifstream in(std::string(LOOPERCAT_APP_DIR) + "/" + file);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool says(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

} // namespace

int main()
{
    // --- the agreed numbers are a retry at all, and a wait a quit can afford ---

    {
        CHECK(kTries >= 2);   // one try is no retry
        CHECK(kPause > 0ms);  // back to back, the holder has no time to let go
        // A Quit is a Disconnect, and the quit waits a bounded time for the
        // release before it leaves anyway (MainComponent.cpp). Retries whose
        // pauses alone outlast that bound would turn every busy quit into
        // one that leaves the pedal in STORAGE without having tried.
        const std::string owner = readApp("MainComponent.cpp");
        CHECK(owner.size() > 10000); // a file that failed to read would pass the rest
        std::smatch bound;
        CHECK(std::regex_search(owner, bound, std::regex(R"(kQuitReleaseBoundMs\s*=\s*(\d+))")));
        if (!bound.empty())
            CHECK((kTries - 1) * kPause < std::chrono::milliseconds(std::stoll(bound[1].str())));
    }

    // --- a card that stays busy: every try is made, then the banner ---

    {
        Scripted p;
        p.run();
        CHECK_EQ(p.attempts, kTries);
        CHECK_EQ(p.pauses.size(), static_cast<std::size_t>(kTries - 1));
        for (const auto pause : p.pauses)
            CHECK(pause == kPause);
        CHECK_EQ(p.outcomes.size(), 1u);
        if (!p.outcomes.empty()) {
            CHECK(!p.outcomes[0].unmounted());
            CHECK(p.outcomes[0].last.answer == Answer::busy);
            CHECK_EQ(p.outcomes[0].tries, kTries);
            // The journal line issue #169 asked for: the cause in words, and the count.
            CHECK_EQ(traceLine(p.outcomes[0]),
                     "disconnect: eject refused after " + std::to_string(kTries)
                         + " tries (busy: another program holds a file on the card)");
            CHECK(!says(traceLine(p.outcomes[0]), "49168"));
            CHECK(!says(traceLine(p.outcomes[0]), "status"));
            // The banner keeps its sentence and adds nothing: the number is gone.
            CHECK_EQ(bannerDetail(p.outcomes[0]), "");
        }
    }

    // --- any other refusal is told at once, in the OS's own words ---

    {
        Scripted p;
        p.script = { { Answer::refused, "Not privileged" } };
        p.run();
        CHECK_EQ(p.attempts, 1);
        CHECK(p.pauses.empty());
        CHECK_EQ(p.outcomes.size(), 1u);
        if (!p.outcomes.empty()) {
            CHECK(p.outcomes[0].last.answer == Answer::refused);
            CHECK_EQ(p.outcomes[0].tries, 1);
            CHECK_EQ(traceLine(p.outcomes[0]), "disconnect: eject refused (Not privileged)");
            CHECK_EQ(bannerDetail(p.outcomes[0]), "Not privileged");
        }
    }

    // --- a refusal of another kind after busy ones stops the waiting there ---

    {
        Scripted p;
        p.script = { busy(), busy(), { Answer::refused, "not a device-backed mount: /Volumes/BOSS RC-5" } };
        p.run();
        CHECK_EQ(p.attempts, 3);
        CHECK_EQ(p.pauses.size(), 2u);
        CHECK_EQ(p.outcomes.size(), 1u);
        if (!p.outcomes.empty()) {
            CHECK(p.outcomes[0].last.answer == Answer::refused);
            CHECK_EQ(p.outcomes[0].tries, 3);
            CHECK_EQ(traceLine(p.outcomes[0]),
                     "disconnect: eject refused after 3 tries (not a device-backed mount: /Volumes/BOSS RC-5)");
            CHECK_EQ(bannerDetail(p.outcomes[0]), "not a device-backed mount: /Volumes/BOSS RC-5");
        }
    }

    // --- the issue's own case: busy once, then the second try goes through ---

    {
        Scripted p;
        p.script = { busy(), { Answer::unmounted, "" } };
        p.run();
        CHECK_EQ(p.attempts, 2);
        CHECK_EQ(p.pauses.size(), 1u);
        CHECK_EQ(p.outcomes.size(), 1u);
        if (!p.outcomes.empty()) {
            CHECK(p.outcomes[0].unmounted());
            CHECK_EQ(p.outcomes[0].tries, 2);
            CHECK_EQ(traceLine(p.outcomes[0]), "disconnect: ejected after 2 tries");
        }
    }

    // --- success on the very last try is a success, not a banner ---

    {
        Scripted p;
        for (int i = 1; i < kTries; ++i)
            p.script.push_back(busy());
        p.script.push_back({ Answer::unmounted, "" });
        p.run();
        CHECK_EQ(p.attempts, kTries);
        CHECK_EQ(p.outcomes.size(), 1u);
        if (!p.outcomes.empty()) {
            CHECK(p.outcomes[0].unmounted());
            CHECK_EQ(p.outcomes[0].tries, kTries);
        }
    }

    // --- a first-try success reads as it always did, warning and all ---

    {
        Scripted p;
        p.script = { { Answer::unmounted, "" } };
        p.run();
        CHECK_EQ(p.attempts, 1);
        CHECK(p.pauses.empty());
        CHECK_EQ(p.outcomes.size(), 1u);
        if (!p.outcomes.empty())
            CHECK_EQ(traceLine(p.outcomes[0]), "disconnect: ejected");

        Scripted warned;
        warned.script = { busy(), { Answer::unmounted, "volume unmounted, but ejecting the disk was refused: X" } };
        warned.run();
        CHECK_EQ(warned.outcomes.size(), 1u);
        if (!warned.outcomes.empty()) {
            CHECK(warned.outcomes[0].unmounted());
            CHECK_EQ(traceLine(warned.outcomes[0]),
                     "disconnect: ejected after 2 tries (volume unmounted, but ejecting the disk was refused: X)");
        }
    }

    // --- answers that come back later, from elsewhere (DiskArbitration's queue) ---

    {
        std::deque<Report> pending;          // tries asked, not yet answered
        std::deque<std::function<void()>> sleeping; // pauses begun, not yet over
        std::vector<Outcome> outcomes;
        int asked = 0;
        drive([&](Report report) { ++asked; pending.push_back(std::move(report)); },
              [&](std::chrono::milliseconds, std::function<void()> then) { sleeping.push_back(std::move(then)); },
              [&](Outcome outcome) { outcomes.push_back(std::move(outcome)); });
        // Nothing is decided inside the call: one try asked, no verdict.
        CHECK_EQ(asked, 1);
        CHECK(outcomes.empty());
        CHECK(sleeping.empty());

        // Busy: the next try waits for the pause, not for the answer.
        Report first = std::move(pending.front());
        pending.pop_front();
        first(busy());
        CHECK_EQ(asked, 1);
        CHECK_EQ(sleeping.size(), 1u);
        CHECK(outcomes.empty());

        auto wake = std::move(sleeping.front());
        sleeping.pop_front();
        wake();
        CHECK_EQ(asked, 2);
        CHECK(outcomes.empty());

        // The answer to a try is the only thing that may settle it — and only once.
        Report second = std::move(pending.front());
        pending.pop_front();
        second({ Answer::unmounted, "" });
        CHECK_EQ(outcomes.size(), 1u);
        if (!outcomes.empty())
            CHECK_EQ(outcomes[0].tries, 2);
        CHECK_THROWS(second({ Answer::unmounted, "" }), "reported twice");
        CHECK_EQ(outcomes.size(), 1u);
    }

    // --- the blocking pause really waits, then goes on ---

    {
        bool went = false;
        const auto start = std::chrono::steady_clock::now();
        waitBlocking(20ms, [&] { went = true; });
        CHECK(went);
        CHECK(std::chrono::steady_clock::now() - start >= 20ms);
    }

    // --- the app and the three platforms speak through this one policy ---

    {
        const std::string owner = readApp("MainComponent.cpp");
        CHECK(owner.size() > 10000);
        // The journal line and the banner's parenthesis come from here; the
        // completion no longer builds "eject refused (…)" from a raw message.
        CHECK(says(owner, "trace(utf8(ejectretry::traceLine(outcome)))"));
        CHECK(says(owner, "ejectretry::bannerDetail(outcome)"));
        CHECK(!says(owner, "\"disconnect: eject refused\""));

        const std::string macSource = readApp("DeviceWatcherMac.cpp");
        const std::string linuxSource = readApp("DeviceWatcherLinux.cpp");
        const std::string windowsSource = readApp("DeviceWatcherWin.cpp");
        for (const std::string* backend : { &macSource, &linuxSource, &windowsSource }) {
            CHECK(backend->size() > 5000);
            CHECK(says(*backend, "ejectretry::drive("));
        }
        // The busy of issue #169 is the kernel's EBUSY as DiskArbitration
        // encodes it — not only DiskArbitration's own busy code, which it
        // was not.
        CHECK(says(macSource, "status == unix_err(EBUSY)"));
        // No second ladder underneath: five lock tries inside each of five
        // drive tries would wait out a hold five times over.
        CHECK(!says(linuxSource, "kUnmountAttempts"));
        CHECK(!says(windowsSource, "kLockAttempts"));
        CHECK(!says(windowsSource, "sleep_for"));
    }

    return testkit::summary("eject_retry_tests");
}
