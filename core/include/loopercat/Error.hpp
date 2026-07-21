// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <stdexcept>
#include <string>

namespace loopercat {

// The core's single error type. Every violated format invariant or rejected
// input throws this with a message specific enough to act on — the core never
// masks bad input with defaults.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

} // namespace loopercat
