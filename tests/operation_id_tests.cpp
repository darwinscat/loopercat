// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The operation id, tested from what the archive needs of it (issue #72):
//
//   - two operations never share one, however close together they are minted
//     — the wall clock failed exactly here, and a bulk apply mints inside one
//     second every time
//   - the readable label leads, so a directory listing still sorts by time
//   - what the generator adds is directory-safe: the id names a folder
//   - minting is thread-safe, because the promise is uniqueness, not
//     uniqueness-on-one-thread

#include "support.hpp"

#include "../app/OperationId.h"

#include <set>
#include <string>
#include <thread>
#include <vector>

using loopercat::opid::make;

int main()
{
    // The same label, as close together as a machine can manage.
    {
        const std::string label = "2026-09-01T21-35-46";
        CHECK(make(label) != make(label));
    }

    // A bulk apply over all 99 slots, inside one second.
    {
        std::set<std::string> ids;
        for (int i = 0; i < 99; ++i)
            ids.insert(make("2026-09-01T21-35-46"));
        CHECK_EQ(ids.size(), static_cast<std::size_t>(99));
    }

    // Ten thousand, to catch a suffix that wraps or repeats.
    {
        std::set<std::string> ids;
        for (int i = 0; i < 10000; ++i)
            ids.insert(make("label"));
        CHECK_EQ(ids.size(), static_cast<std::size_t>(10000));
    }

    // The label leads: backups/ stays sorted by time in any file manager.
    {
        const std::string label = "2026-09-01T21-35-46";
        const std::string id = make(label);
        CHECK(id.rfind(label, 0) == 0);
        CHECK(id.size() > label.size()); // something was actually added
    }

    // The id names a directory, so what the generator adds must be safe in
    // one: lowercase hex, a decimal counter, and the two dashes between them.
    {
        const std::string label = "L";
        const std::string tail = make(label).substr(label.size());
        CHECK(tail.size() >= 7); // "-xxxx-0"
        CHECK(tail[0] == '-');
        CHECK(tail[5] == '-');
        for (std::size_t i = 1; i < 5; ++i)
            CHECK((tail[i] >= '0' && tail[i] <= '9') || (tail[i] >= 'a' && tail[i] <= 'f'));
        for (std::size_t i = 6; i < tail.size(); ++i)
            CHECK(tail[i] >= '0' && tail[i] <= '9');
    }

    // Different labels stay different, and neither borrows the other's tail.
    {
        CHECK(make("morning") != make("evening"));
    }

    // Eight threads minting at once: the counter is the only shared state,
    // and it is the whole reason two jobs cannot land on one directory.
    {
        constexpr int threads = 8;
        constexpr int each = 500;
        std::vector<std::vector<std::string>> minted(threads);
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; ++t)
            workers.emplace_back([&minted, t] {
                minted[static_cast<std::size_t>(t)].reserve(each);
                for (int i = 0; i < each; ++i)
                    minted[static_cast<std::size_t>(t)].push_back(make("race"));
            });
        for (auto& worker : workers)
            worker.join();

        std::set<std::string> ids;
        for (const auto& batch : minted)
            ids.insert(batch.begin(), batch.end());
        CHECK_EQ(ids.size(), static_cast<std::size_t>(threads * each));
    }

    return testkit::summary("operation_id_tests");
}
