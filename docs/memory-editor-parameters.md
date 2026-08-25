# What a memory editor may write, and what it must not

Working notes for the per-slot settings editor. This is not the field atlas
(`pedal-settings.md`, which maps the whole `MEMORY*.RC0` document); it is the
narrower question the editor has to answer for every control it shows: **does
this parameter exist on an RC-5, what are its real values, and is writing it
safe?**

Sources are the BOSS RC-5 Reference Manual with page numbers, the factory slot
body captured from hardware, and named hardware measurements. Firmware of the
verified unit: Ver 1.10 build 0050.

## How to read the numbers below — the manual has none

**The manual assigns no raw value to anything.** Its parameter tables have three
columns — Parameter, Value (bold: default), Explanation — and every value is a
named string: `OFF`, `ON`, `LOOP STOP`, `Studio`, `4/4`. Nowhere does it say
that `Studio` is stored as 0 or that `LOOP STOP` is stored as 1.

So every raw index in this document comes from one of three places, and they
are not equal:

1. **Hardware.** A value read out of a real card after setting it on the pedal.
   This is the only kind that proves an encoding.
2. **The factory slot body.** Every field of a never-touched slot, captured from
   hardware. Where a factory value equals the manual's printed default, it
   anchors exactly one point of that field's encoding — no more.
3. **The manual's printed order**, read as zero-based. This is inference. It has
   been right everywhere a factory value could test it, which is why it is worth
   writing down, but it is not evidence.

Anything resting only on (3) is marked *inferred* below and needs a diff before
the editor relies on it. The distinction matters because the failure mode is
silent: an enum whose indices are off by one produces a control that writes a
legal-looking wrong value.

## The provenance rule

The atlas marks confidence `[V]` verified on hardware, `[M]` semantics known
from the manual, `[?]` unknown. Three entries carried `[M]` or a value list the
RC-5 manual does not contain — `Pan`, `Sync`, and a drum kit named `Live`. All
three are RC-500/RC-505 knowledge that drifted into marks claiming to come from
the RC-5 manual.

**So: `[M]` requires a page number**, and a page reference supports only what is
printed on it. A parameter list remembered from a related pedal is not manual
semantics; neither is a raw encoding read off a page that prints no numbers.

## Fields that are not RC-5 parameters

These exist in the file and have no counterpart in the pedal's parameter tables:
MEMORY (LOOP p. 9, RHYTHM p. 10, NAME p. 11) or SETUP (GENERAL p. 12, CONTROL
p. 13, CC#80–87 p. 14, MIDI/STORAGE/F.RESET p. 15).

| Field | Factory | Note |
| --- | --- | --- |
| `TRACK1/Pan` | 50 | No pan parameter exists anywhere in the manual. Hardware then showed why none is needed — see below. |
| `MASTER/Sync` | 0 | Synchronisation on an RC-5 is system-wide: `SYNC CLOCK` (AUTO/INTERNAL/USB/MIDI) and `SYNC START` (OFF/ALL/RHYTHM), p. 15. There is no per-memory sync parameter. |
| `TRACK1/MeasMod` | 1 | No documented parameter. |
| `TRACK1/MeasBtLp` | 0 | No documented parameter. |
| `MASTER/LpMod` | 0 | No documented parameter. |
| `MASTER/LpLen` | 0 | No documented parameter. |
| `MASTER/TrkMod` | 1 | No documented parameter. |

**Editor policy for all of them: preserve the integer exactly.** Do not show
them, do not normalise them, do not reset them to a guessed default. "No
documented parameter" proves the pedal offers no way to change the field; it
does not prove the firmware never reads it.

### What replaces Pan

The decisive evidence for Pan's absence is the memory parameter tables
themselves (pp. 9–11): a per-memory parameter would be printed there, and pan is
not. Its absence from the CC#80–87 list (p. 14) points the same way but proves
less than it seems to — the manual never claims that list is complete, and it is
not: `MEASURE`, `BEAT` and `NAME` have no CC entry either.

What settles it is hardware. Measured 2026-08-25 with 441 Hz in one channel and
1470 Hz in the other, **and both output jacks patched**: file channel 1 arrived
at OUTPUT A (MONO) alone and channel 2 at OUTPUT B alone, unmixed. The
qualifier is not decoration — p. 2 states that with only the A (MONO) jack in
use, "even sound that is input in stereo is output in mono", and hardware
confirmed the pedal folds its own output down in that case.

Placement is therefore done to the WAV, not asked of the pedal — see
`core/include/loopercat/Downmix.hpp`.

## Enumerations

### `RHYTHM/Pattern`

The atlas said 57 values, 0–56. The manual (p. 10) lists 57 named patterns
**plus `Blank`** — 58 selectable values. The specification page (p. 22) prints
"57 Patterns x 2 Variations" without saying what it counts, but the named groups
on p. 10 sum to exactly 57, so that figure evidently excludes `Blank`; taking it
as the enum size is where the off-by-one came from.

Names and order are from p. 10. **The indices are inferred** from that order —
p. 10 prints no numbers.

| Raw | Pattern | Raw | Pattern |
| ---: | --- | ---: | --- |
| 0–3 | SimpleBeat1–4 | 34–37 | PercusBeat1–4 |
| 4–10 | GrooveBeat1–7 | 38–41 | LatinBeat1–4 |
| 11–14 | Rock1–4 | 42–44 | Conga1–3 |
| 15–18 | Funk1–4 | 45–46 | Bossa1–2 |
| 19–23 | Shuffle1–5 | 47–48 | Samba1–2 |
| 24–28 | Swing1–5 | 49–52 | DanceBeat1–4 |
| 29–33 | SideStick1–5 | 53–56 | Metronome1–4 |
|  |  | 57 | Blank |

Both ends are anchored independently: raw 0 is the factory value and p. 10's
printed default SimpleBeat1, and `Pattern=57 ↔ Blank` is **verified on
hardware** (`core/include/loopercat/Rc0.hpp`, `kRhythmPatternBlank`). The
interior boundaries follow from the group sizes and are inferred.

### `RHYTHM/Kit`

Menu order, p. 10, confirmed by the specification's "Rhythm Kit: 7 types"
(p. 22): Studio · Rock · Jazz · Brush · Cajon · R&B · 808+909. There is no kit
named `Live` on this pedal.

Neither page prints a raw value. Mapping `0` Studio … `6` 808+909 is **inferred**
from the menu order; `0` = Studio is anchored by the factory body.

### `RHYTHM/Beat`

Careful here — this one is weaker than it looks, and every independent review of
it made the same mistake.

p. 10 prints the value as `2/4–4/4–7/4, 5/8–15/8`, with `4/4` bold as the
default. That is **range notation with endpoints only**. The manual never
enumerates the individual settings, never counts them, and the specification
page carries no beat count to cross-check. The intermediate settings usually
quoted — 5/4, 6/4, 6/8, 7/8, 8/8, 9/8, 10/8, 11/8, 12/8, 13/8, 14/8 — appear
nowhere in the manual.

If every step inside both printed ranges exists and the /8 block follows the /4
block, the list is `0` 2/4 · `1` 3/4 · `2` 4/4 · `3` 5/4 · `4` 6/4 · `5` 7/4 ·
`6` 5/8 · `7` 6/8 · `8` 7/8 · `9` 8/8 · `10` 9/8 · `11` 10/8 · `12` 11/8 ·
`13` 12/8 · `14` 13/8 · `15` 14/8 · `16` 15/8 — seventeen values, 0–16. **All of
that is inference from dash notation.** Factory 2 = 4/4 is verified and anchors
4/4 at raw 2; it fixes nothing else.

### `MASTER/FadeTime`

p. 9 prints four note lengths followed by `1MEAS–2MEAS–64MEAS`, with 2MEAS bold
as the default. Rendered, the four notes are a sixteenth, an eighth, a quarter
and a half, in that order — they read correctly off the page even though
Roland's symbol font does not survive text extraction from the PDF.

The measure half is endpoints and default only: p. 9 never states the step size
and never counts the values. So `raw = measures + 3` — giving `4` = 1MEAS,
`5` = 2MEAS (the factory value, matching the bold default) and `67` = 64MEAS for
68 values total — is **inferred** from the factory anchor plus the assumption
that the notes occupy 0–3 and the measures step by one.

### Two-value and three-value fields

Names, order and the bold default are from pp. 9–10. **The 0/1/2 columns are
inferred** — those pages print no encodings — and the Factory column is from the
captured factory body, not the manual.

| Field | 0 | 1 | 2 | Factory | Page |
| --- | --- | --- | --- | ---: | ---: |
| `TRACK1/Rev` | OFF | ON | | 0 | 9 |
| `TRACK1/One` | OFF | ON | | 0 | 9 |
| `TRACK1/StrtMod` | IMMEDIATE | FADE IN | | 0 | 9 |
| `TRACK1/StpMod` | IMMEDIATE | FADE OUT | LOOP END | 0 | 9 |
| `MASTER/DubMode` | OVERDUB | REPLACE | | 0 | 9 |
| `MASTER/RecAction` | REC→DUB | REC→PLAY | | 1 | 9 |
| `MASTER/AutoRec` | OFF | ON | | 0 | 9 |
| `RHYTHM/Variation` | A | B | | 0 | 10 |
| `RHYTHM/VariationChange` | MEASURE | LOOP END | | 0 | 10 |
| `RHYTHM/Fill` | OFF | ON | | 1 | 10 |
| `RHYTHM/Part1`–`Part4` | OFF | ON | | 1,1,1,0 | 10 |
| `RHYTHM/RecCount` | OFF | 1MEAS | | 0 | 10 |
| `RHYTHM/PlayCount` | OFF | 1MEAS | | 0 | 10 |
| `RHYTHM/Start` | LOOP START | REC END | BEFORE LOOP | 0 | 10 |
| `RHYTHM/Stop` | OFF | LOOP STOP | REC END | 1 | 10 |

Where a factory value equals the bold default — `RecAction=1` against REC→PLAY,
`Stop=1` against LOOP STOP, `Fill=1` against ON, `Part4=0` against OFF — the
zero-based reading is anchored at that one point. The others rest on order
alone.

`RHYTHM/Start`: LOOP START is **one** value covering both cases — "the rhythm
plays when loop recording or playback starts". The atlas read it as two.

`RHYTHM/Part1`–`Part4` were marked `[?]`. They are documented: each switches one
of the drum kit's four parts on or off, and the factory body matches the
manual's defaults exactly.

### `RHYTHM/ToneLow`, `RHYTHM/ToneHigh`

The pedal shows −10…0…+10 with 0 as the default; the file stores 10. So
`display = raw − 10`, raw range 0–20. Inferred from the single factory anchor —
the endpoints deserve a confirming diff.

### `TRACK1/PlyLvl`

The manual gives 0–200 with a default of 100. It does **not** say that 100 is
unity gain or 0 dB — the atlas claims more than the source supports. Call it
"playback level, 100 = the pedal's default".

## The unresolved one: which field is LOOP LEVEL

The manual documents two per-memory LEVELs with identical range and default:
LOOP `LEVEL` (p. 9, "adjusts the playback level of the track") and RHYTHM
`LEVEL` (p. 10, "adjusts the volume of the rhythm"). Both are 0–200, default
100. `RHYTHM/Level` in the file plainly answers the second.

The problem is the first. Two file fields sit at 100 and both could be it:
`TRACK1/PlyLvl` and `MASTER/Level`.

The expression-pedal and CC assignments do not separate them. p. 13 and p. 14
list `TRK LEVEL1` as "control the volume of track (LOOP LEVEL) in the range of
0–200" and `MEMORY LEV1` as "control the 'LEVEL' (p. 9) of memory/LOOP in the
range of 0–200" — differently worded, both pointing back to the same p. 9
parameter. That they are one parameter is a reasonable reading, not a statement
the manual makes.

**Neither field may be exposed until a diff says which one the pedal writes.** A
slider bound to the wrong one is a control that appears to work and changes
nothing.

## Constraints the editor has to respect

- **`Beat` cannot be changed after the track is recorded** (p. 10, stated
  outright). The control must be disabled for a slot that holds audio.
- **`Measure` is not an enum** and must not be exposed raw. Factory `Measure=1`
  with `MeasLen=0` is FREE, while an explicit count of N measures stores
  `MeasLen=N` and `Measure=N+7` (verified on hardware). If explicit counts start
  at `1MEAS → 8`, then raw 0 and 2–7 are unidentified — a range of **eight**
  slots, not the seven the atlas assumes, or raw 7 is left as a hole. Expose
  FREE / N measures and let the writer keep the backing fields consistent.
- **Changing `MASTER/Tempo` must not rewrite `TRACK1/RecTmp`.** The manual
  treats a memory's current tempo and the tempo a phrase was recorded at as
  different things: `TEMPO TOO FAST` and `TEMPO TOO SLOW` (p. 20) both say the
  track "is being played at a much faster/slower tempo than when it was
  recorded", and the remedy is to adjust the tempo, not the recording.
- **Treat `WavStat` and `WavLen` as pedal-owned.** The manual names no such
  fields and describes no binding, so this is our own rule rather than a
  documented one: their meaning is hardware-derived, they are written by the
  pedal's indexer, and the errors around damaged or unreadable memory content
  (`DATA DAMAGED`, `DATA READ ERR`, p. 20) are exactly the class of failure a
  wrong value here would produce.
- Worth surfacing as hints rather than blocks, both from p. 9: `REVERSE = ON`
  means overdubbing cannot be switched to after a recording completes, and
  `1SHOT = ON` plays once and stops, retriggers from the beginning if the pedal
  switch is pressed during playback, and disallows overdubbing. (p. 17 adds only
  that a one-shot memory does not send MIDI Start/Stop.)

## Experiments that close the remainder

One STORAGE session. For each: set the value on the pedal, WRITE, reconnect,
diff the slot body. Every one of these exists because the manual prints no raw
values — they are how inference becomes knowledge.

1. **LEVEL to 73** — decides `PlyLvl` versus `MASTER/Level`. This one blocks the
   editor; the rest only sharpen it.
2. **MEASURE to 1MEAS, then 2MEAS** — expects raw 8 and 9, and settles whether
   the special range is 0–7.
3. **KIT to 808+909** — expects 6, confirming the inferred kit order end to end.
4. **BEAT to 5/8, then 15/8** — establishes the enumeration the manual does not
   print. This is the weakest-sourced field in the document, so it is worth
   walking the whole range rather than sampling it.
5. **FADE TIME to its two shortest values** — expects 0 and 1, confirming the
   notes occupy the bottom of the range.
6. **RHYTHM START to BEFORE LOOP, STOP to OFF** — expects 2 and 0.
7. **PATTERN to Blank** — expects 57, already known from hardware.
8. **TONE LOW to −10, 0, +10** — expects 0, 10, 20.
9. **Rhythm in play-standby, then WRITE** — expects `State` 1 (p. 8 describes
   this as how a "rhythm: on" memory is saved).
10. **BEAT on a slot that holds audio** — records what the pedal actually does,
    which the editor's disabled state should match.

A faster route to the same answers is the RQ1 sweep over USB-MIDI: it reads
memory addresses live, with no STORAGE round trip at all.
