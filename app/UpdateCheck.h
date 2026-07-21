// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <felitronics/appkit/UpdateChecker.h>

#include "AppSettings.h"

//==============================================================================
// loopercat::UpdateCheck — the thin adapter over the family's shared checker
// (felitronics-appkit). All policy lives there: user-click only, silent
// failure, owned worker thread joined on destruction. This bakes in Looper
// Cat's Config. The slug points at the family org home; until the first
// public release exists there, a check is a silent no-op by design.
//==============================================================================
namespace loopercat
{

struct UpdateCheck : felitronics::appkit::UpdateChecker
{
    // `settings` must outlive this checker — the owner declares it first.
    explicit UpdateCheck(AppSettings& settings)
        : felitronics::appkit::UpdateChecker({ .ownerRepo = "darwinscat/loopercat",
                                               .productName = "LooperCat",
                                               .currentVersion = LOOPERCAT_VERSION,
                                               .settings = [&settings] { return settings.file(); } })
    {
    }
};

} // namespace loopercat
