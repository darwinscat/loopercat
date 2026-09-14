<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Third-party notices

LooperCat is AGPL-3.0-or-later and its own code is © Darwin's Cat. Everything below is someone
else's, ordered by how close it travels to the person running the app: what is **compiled into the
shipped binary** first, then what only the test tier ever links, then the libraries the app asks
the platform for and does not distribute at all.

| Component | Where it is | License | Notes |
|---|---|---|---|
| **Michroma** (font) | **embedded in the binary** by `juce_add_binary_data` (`LooperCatAssets`), taken from felitronics-appkit's `assets/Michroma-Regular.ttf` — the **unmodified original** | SIL OFL 1.1 | © 2011 The Michroma Project Authors (https://github.com/googlefonts/Michroma-font). Licence text travels with the font upstream: [`assets/Michroma-OFL.txt`](https://github.com/darwinscat/felitronics-appkit/blob/main/assets/Michroma-OFL.txt). See below. |
| **minimp3** | fetched, pinned to commit `ea99364`; two headers, compiled into the app behind `app/Mp3AudioFormat` | CC0-1.0 | © lieff (https://github.com/lieff/minimp3), dedicated to the public domain. The one bundled decoder on all three platforms: stock JUCE cannot read mp3 on Linux at all, and the platform codecs that do exist leave the LAME encoder delay and padding in place — leading junk and a gapped loop seam, which is the one defect a looper cannot accept. |
| **JUCE** 8.0.14 | fetched, pinned; the application framework | AGPLv3 (our option) | This repo being AGPL and source-public *is* the JUCE compliance — no key, no flag. JUCE compiles **SheenBidi** into `juce_graphics`; it travels inside JUCE under JUCE's own notices. |
| **nlohmann/json** 3.11.3 | fetched, pinned — **test tier only; the application never links it** | MIT | © 2013–2022 Niels Lohmann. Parses `fixtures/golden.json`, the shared conformance fixture. |
| **libudev** | linked on Linux through pkg-config | LGPL-2.1-or-later | The udev netlink monitor behind device detection. Supplied by the distribution; we link it, we do not ship it. |
| **alsa-lib** | linked on Linux through pkg-config | LGPL-2.1-or-later | The rawmidi sender uses the API directly rather than inheriting it from JUCE by luck. Supplied by the distribution. |
| **DiskArbitration**, **IOKit** | linked on macOS | Apple system frameworks | Volume liveness, unmount and eject. Part of the operating system. |

First-party and listed only so nobody has to wonder: **felitronics-core** (`felitronics::analysis`,
pinned `v0.23.0`), **felitronics-appkit** (pinned `v0.18.0`) and the Darwin's Cat mark
`catlogo.svg`, also embedded in `LooperCatAssets`. All three are ours, AGPL-3.0-or-later, no
third-party licence involved.

## The embedded Michroma

The wordmark face is compiled into every LooperCat binary, so its notice is owed to every user who
runs one, not merely to someone who reads this repository. Two points, both simpler here than they
are upstream:

- **It is the original file, byte for byte.** LooperCat embeds appkit's `assets/Michroma-Regular.ttf`
  as it stands — it does not subset, re-instance or re-flavour it. Nothing here is a *Modified
  Version* of the Font Software, so the questions that come with modification do not arise.
- **The name may stay.** The rename obligation in OFL §3 binds only names declared as *Reserved
  Font Names*, and Michroma's notice declares none — it reads simply "Copyright … Project Authors".
- **§1 permits the embedding.** Bundling the font inside software released under another licence is
  allowed, provided the copyright notice and the licence travel with it. That is what this file and
  the upstream `Michroma-OFL.txt` are for. The font stays OFL; it does not become AGPL because the
  code around it is.

## rc5cat — knowledge, not code

Several headers cite [rc5cat](https://github.com/AliceLafox/rc5cat) as the source of what is known
about the RC-5's on-card format: `Rc0.hpp`, `Wav.hpp`, `Catalog.hpp`, `Volume.hpp`, `Commands.hpp`.
Those citations are about **facts** — which byte means what, which sequence the pedal answers to —
and where a comment says "byte-for-byte where it matters" it means our bytes match the pedal's
protocol, not that any source was copied. A file format and a wire protocol are not copyrightable
subject matter; their expression in code is, and every line of that expression here is ours. The
citations are there because the knowledge was someone's work and saying so is right, not because a
licence compels it.

## Trademarks

LooperCat is an independent project: not affiliated with, endorsed, or sponsored by any hardware
manufacturer; all trademarks belong to their respective owners. **"BOSS RC-5" is used
nominatively**, to state what this software is compatible with. The application shows that sentence
in its About window, from [`resources/notice.txt`](resources/notice.txt), which is the same text
the README carries word for word — a file rather than a string literal, so the two can be diffed
against each other.

The LooperCat and Darwin's Cat names and logos are trademarks and are *not* covered by the code
licence.
