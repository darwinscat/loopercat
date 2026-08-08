// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Commands.hpp>
#include <loopercat/Error.hpp>
#include <loopercat/Lifecycle.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

//==============================================================================
// loopercat::banners — the banner strip's single owner (issue #3).
//
// Before this model the strip and its owner each kept a copy of the last
// error, and any re-render (a lifecycle transition, a scan) resurrected a
// line the user had dismissed, while a ghost-cleanup warning survived the
// reconnect that healed it. The model fixes both by construction: findings
// and the lifecycle line are REPLACED by every scan, error lines change only
// through explicit events — show, lane recovery, the user's click — and a
// re-render is just lines() again, never a mutation.
//
// JUCE-free on purpose: the policy is tested by theory in banner_tests, the
// strip only draws lines().
//==============================================================================
namespace loopercat::banners {

// Who put a transient error line up. The lane decides which recovery clears
// it: connection errors belong to the connect/mount/eject story and clear
// when that story recovers; job errors belong to the last mutation and clear
// when a later mutation succeeds.
enum class Source { connection, job };

struct Line {
    commands::Level level = commands::Level::info;
    std::string text;
    bool dismissible = false;

    bool operator==(const Line&) const = default;
};

class Model {
public:
    // Fold one scan in: the lifecycle state it reported and the doctor
    // findings, replaced wholesale. Recovery policy lives here — an honest
    // (re)mount or a clean eject closes the connection story and takes its
    // error line with it. The clean eject is observed either as `ejected`,
    // or — when the walk-out coalesces eject-finished with device-lost
    // between two scans, which the app-driven disconnect always does — as
    // ejecting -> disconnected; from ejecting the machine reaches
    // disconnected through a successful eject only (hardware find,
    // 2026-08-08). ejecting -> connected is an eject REFUSAL and
    // ejecting -> ghost a mid-eject yank: both keep the line explaining them.
    void scan(lifecycle::State state, std::vector<commands::Finding> findings)
    {
        const bool remounted = state == lifecycle::State::connected
                            && state_ != lifecycle::State::connected
                            && state_ != lifecycle::State::ejecting;
        const bool ejectedCleanly =
            (state == lifecycle::State::ejected && state_ != lifecycle::State::ejected)
            || (state == lifecycle::State::disconnected
                && state_ == lifecycle::State::ejecting);
        if (remounted || ejectedCleanly)
            connectionError_.reset();
        state_ = state;
        findings_ = std::move(findings);
    }

    void showError(Source source, std::string text)
    {
        if (text.empty())
            throw Error("banner error text must not be empty");
        (source == Source::connection ? connectionError_ : jobError_) = std::move(text);
    }

    // A later mutation succeeded — the job-error story moved on.
    void clearJobError() { jobError_.reset(); }

    // The user's click: error lines go and STAY gone. Only a new showError —
    // a new event, never a re-render — may bring one back.
    void dismiss()
    {
        connectionError_.reset();
        jobError_.reset();
    }

    bool hasDismissible() const
    {
        return connectionError_.has_value() || jobError_.has_value();
    }

    // Render order: the actionable errors first, then the lifecycle line (it
    // explains the table below), then the findings.
    std::vector<Line> lines() const
    {
        std::vector<Line> out;
        if (connectionError_)
            out.push_back({ commands::Level::error, *connectionError_, true });
        if (jobError_)
            out.push_back({ commands::Level::error, *jobError_, true });
        if (const auto line = lifecycleLine())
            out.push_back(*line);
        for (const auto& finding : findings_)
            out.push_back({ finding.level, finding.message, false });
        return out;
    }

private:
    // The state's own banner: what the connection story means for the table,
    // told to a musician.
    std::optional<Line> lifecycleLine() const
    {
        switch (state_) {
        case lifecycle::State::ghost:
            return Line { commands::Level::error,
                          "The pedal left without an eject \xe2\x80\x94 what you see is a stale "
                          "copy and nothing reaches the pedal. Cleaning up\xe2\x80\xa6" };
        case lifecycle::State::ejected:
            return Line { commands::Level::info,
                          "Volume ejected \xe2\x80\x94 safe to disconnect the pedal." };
        case lifecycle::State::disconnected:
        case lifecycle::State::connected:
        case lifecycle::State::ejecting:
            return std::nullopt;
        }
        throw Error("unknown lifecycle state");
    }

    lifecycle::State state_ = lifecycle::State::disconnected;
    std::vector<commands::Finding> findings_;
    std::optional<std::string> connectionError_;
    std::optional<std::string> jobError_;
};

} // namespace loopercat::banners
