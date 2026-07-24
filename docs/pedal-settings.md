# The rest of the pedal: a field atlas and an editor proposal

Every RC-5 memory carries ~35 settings in `MEMORY*.RC0`, and LooperCat exposes
five of them so far (name, one-shot, tempo, bars, the wav linkage). This
document maps everything else — what each field is, how confident we are, and
which of them belong in an external editor. Sources: the factory slot body
captured from hardware (rc5cat `lib/rc0.js`), observed values across a
15-slot live card (2026-07-24 analysis, see issue #10), and the RC-5 manual's
parameter list. Confidence marks: **[V]** verified on hardware, **[M]** manual
semantics known / exact enum mapping to verify, **[?]** unknown — experiment
needed.

## TRACK1 — playback of the loop itself

| Field | Meaning | Values | Conf |
| --- | --- | --- | --- |
| `Rev` | Reverse playback | 0/1 | [M] |
| `PlyLvl` | Play level | 0–200, 100 = unity | [M] |
| `Pan` | Pan | 0–100, 50 = center | [M] |
| `One` | One Shot | 0/1 | [V] exposed |
| `StrtMod` | Start mode | immediate / fade-in | [M] |
| `StpMod` | Stop mode | immediate / fade-out / loop-end | [M] |
| `Measure` | Bar-count UI enum | `MeasLen + 7`; raw 0–6 = special modes (AUTO, FREE, fractions…) | [V] offset; [M] specials |
| `MeasMod` | Measure mode (auto/manual bar counting?) | factory 1 | [?] |
| `MeasLen` | True bar count | integer, pedal displays it | [V] exposed |
| `MeasBtLp` | ? (beat loop?) | 0 on every observed slot | [?] |
| `RecTmp` | Tempo at record time | tenths of BPM | [V] |
| `WavStat` | Audio indexed | 0/1, pedal-owned | [V] |
| `WavLen` | Frames at 44.1 kHz | pedal-owned | [V] |

## MASTER — memory-level behavior

| Field | Meaning | Values | Conf |
| --- | --- | --- | --- |
| `Tempo` | Memory tempo | tenths, 400–3000 | [V] exposed |
| `DubMode` | Overdub mode | overdub / replace | [M] |
| `RecAction` | What follows REC | rec→overdub / rec→play | [M] |
| `AutoRec` | Auto-record on input | 0/1 | [M] |
| `FadeTime` | Fade length | factory 5; units to verify (measures?) | [M] |
| `Level` | Memory level | 0–200 | [M] |
| `LpMod` | Loop mode? | factory 0 | [?] |
| `LpLen` | Loop length (sync-related, 0 = auto?) | factory 0 | [?] |
| `TrkMod` | ? | factory 1 | [?] |
| `Sync` | Tempo sync (MIDI/USB) | 0/1 | [M] |

## RHYTHM — the onboard drums (the headline for an editor)

| Field | Meaning | Values | Conf |
| --- | --- | --- | --- |
| `State` | Rhythm on/off for this memory | 0/1 | [M] |
| `Level` | Drum level | 0–200 | [M] |
| `Reverb` | Drum reverb send | 0–100, factory 30 | [M] |
| `Pattern` | Rhythm pattern | enum 0–56 (57 patterns) | [M] map to verify |
| `Variation` | Variation A/B | 0/1 | [M] |
| `VariationChange` | When A↔B switches | measure / loop end | [M] |
| `Kit` | Drum kit | enum (Studio, Live, Rock, Jazz, Brush, Cajon…) | [M] map to verify |
| `Beat` | Time signature | enum; **2 = 4/4 verified**, full map to harvest | [V] partial |
| `Fill` | Fill on variation change | 0/1, factory 1 | [M] |
| `Part1`–`Part4` | Pattern parts enabled? factory 1,1,1,0 | 0/1 ×4 | [?] |
| `RecCount` | Count-in before recording | off / 1 measure | [M] |
| `PlayCount` | Count-in before playback | off / 1 measure | [M] |
| `Start` | How the rhythm starts | with rec / with play / intro… | [M] |
| `Stop` | How the rhythm stops | factory 1; enum to verify | [M] |
| `ToneLow` / `ToneHigh` | Drum tone EQ | factory 10/10 (±10 around 10?) | [M] |

`SYSTEM1/2.RC0` (device-wide settings) are zero bytes on the observed card —
the pedal appears to write them only when system settings change. Uncharted;
out of scope for the memory editor.

## What belongs in LooperCat

**Tier 1 — obvious wins, all verified-mechanics writes** (same
transaction/backup/generation discipline as every mutation; one hardware
checkpoint per enum to pin the value maps):

- Playback shaping: **Reverse**, **Play level**, **Pan**, **Start/Stop
  modes** — "how does this loop behave live" without touching the pedal menu.
- The drums: **Rhythm on/off, Pattern, Kit, Beat, Variation, Level** — the
  scrolling a one-line pedal display 57 times.

**Tier 2 — quality of life:** count-ins (`RecCount`/`PlayCount`), recording
behavior (`DubMode`, `RecAction`, `AutoRec`), drum `Reverb`/`Tone`, `Fill`,
`VariationChange`, `Sync`, `FadeTime`.

**Tier 3 — research first:** `MeasMod`, `MeasBtLp`, `LpMod`, `LpLen`,
`TrkMod`, `Part1–4` — meaning unconfirmed; decode before exposing anything.

## UI concept

A per-slot **Memory Settings drawer** (opens from the context menu / a gear in
the row), three groups mirroring the file: Playback / Recording / Rhythm.
Enums as combo boxes with verified value lists only — an unverified enum stays
out rather than guessing labels. Every change is one worker job; the row
pulses; the usual "reboot the pedal to apply" note stands.

## How to finish the enum maps (fast path)

The classic way: flip a value in the pedal's menu, re-enter storage, diff the
RC0 — tedious at ~30 seconds per value. The fast path is the #9/#22 discovery:
**RQ1 over USB-MIDI reads memory addresses live, no storage round-trip**. If
the RQ1 address space maps onto these fields (likely — Tone Studio's
handshake reads a register the same way), one session with MIDI Monitor while
someone walks the pedal menus yields every enum mapping in near-real-time.
That makes the RQ1 sweep the enabling task for Tier 1's checkpoint.
