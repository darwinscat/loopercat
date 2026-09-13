<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Contributing to LooperCat

**Short version: issues yes, code no.**

## Bug reports, questions and ideas — welcome

Open an issue. A bug you hit, a question about how something behaves, a feature you miss — all
of these are genuinely useful and get acted on. If a fix or a feature lands because you raised
it, you are credited in the release notes.

## Code — not accepted

Pull requests containing code are closed unmerged, regardless of how good the change is. This is
a standing policy, not a verdict on your patch.

**Why.** LooperCat is AGPL-3.0-or-later and the copyright is held solely by its authors, Oleh
Tsymaienko and Alisa Lafoks (Darwin's Cat). A merged contribution leaves its author holding
copyright on those lines permanently. From that moment the project can no longer change its own
licensing terms — dual-licensing, granting an exception, shipping it inside something else —
without tracking down every past contributor and obtaining each one's agreement. Nobody can
foresee a decade of a project's life; declining code costs nothing today, so that is the side we
err on. The same policy governs
[felitronics-core](https://github.com/darwinscat/felitronics-core/blob/main/CONTRIBUTING.md),
which this project builds on.

If your patch would have been genuinely good, **describe the change in an issue instead**. Ideas
are not copyrightable — only their expression is — so a well-described problem or approach can be
implemented here freely. You get the fix you wanted, with credit, and the project keeps its
options open.

Describe it in prose, though — **please do not paste code into the issue**. A pasted patch brings
the same copyright question in through the side door, and code that has been read cannot be
unread: someone who has seen your implementation can no longer write that part independently. The
problem, the approach and the behaviour you expect are what is useful here, and none of that needs
a diff.

## What this policy does NOT do

It takes away none of the freedoms the AGPL grants you. You may use LooperCat, study it, modify
it, and distribute your own modified version under the same licence. Forking is expressly fine.
This is only about what gets merged *here*.

## Third-party code

Third-party dependencies keep their own licences and are unaffected by the policy above. New ones
must be AGPL-compatible (BSD / MIT / Apache-2.0 / MPL-2.0).

The LooperCat and Darwin's Cat names and logos are trademarks and are *not* covered by the code
licence.
