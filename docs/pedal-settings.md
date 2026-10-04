# The rest of the pedal: a field atlas and an editor proposal

Every RC-5 memory carries ~35 settings in `MEMORY*.RC0`, and LooperCat exposes
five of them so far (name, one-shot, tempo, bars, the wav linkage). This
document maps everything else — what each field is, how confident we are, and
which of them belong in an external editor — and the pedal's own settings in
`SYSTEM*.RC0`. Sources: the factory slot body captured from hardware (rc5cat
`lib/rc0.js`), observed values across a 15-slot live card (2026-07-24 analysis,
see issue #10), a settings file taken off hardware (fw 1.10,
`fixtures/rc5-system.RC0`), two readings of an RC-5 in use (2026-10-01), and
the RC-5 manual's parameter list. Confidence marks: **[V]** verified on
hardware, **[M]** manual semantics known / exact enum mapping to verify, **[?]**
unknown — experiment needed.

## TRACK1 — playback of the loop itself

| Field | Meaning | Values | Conf |
| --- | --- | --- | --- |
| `Rev` | Reverse playback | 0/1 | [M] |
| `PlyLvl` | Play level | 0–200, 100 = unity | [M] |
| `Pan` | **Not a parameter of this pedal** — see below | factory 50 | [V] absent |
| `One` | One Shot | 0/1 | [V] exposed |
| `StrtMod` | Start mode | immediate / fade-in | [M] |
| `StpMod` | Stop mode | immediate / fade-out / loop-end | [M] |
| `Measure` | Bar-count UI enum | `MeasLen + 7`; raw 0–6 = special modes (AUTO, FREE, fractions…) | [V] offset; [M] specials |
| `MeasMod` | Measure mode (auto/manual bar counting?) | factory 1 | [?] |
| `MeasLen` | True bar count | integer, pedal displays it | [V] exposed |
| `MeasBtLp` | ? (beat loop?) | 0 on every observed slot | [?] |
| `RecTmp` | Tempo at record time | tenths of BPM | [V] |
| `WavStat` | Audio index state | 0 = none, 1 = indexed, 2 = present but not indexed (e.g. a non-float32 file the pedal will not take); pedal-owned | [V] 0/1/2 seen |
| `WavLen` | Frames at 44.1 kHz | pedal-owned | [V] |

### `Pan` is a field, not a knob

The RC-5 has no pan. The decisive evidence is the memory parameter tables
themselves (LOOP p. 9, RHYTHM p. 10, NAME p. 11): a per-memory parameter would
be printed there, and pan is not. It is missing from the SETUP pages too, and
from the CC#80–87 assignable list on p. 14 — though that list proves less than
it looks like it does, since the manual never claims it is complete and it is
not: `MEASURE`, `BEAT` and `NAME` have no CC entry either.

So `<Pan>` is a field this format inherited from its RC-500/RC-505 relatives.
Writing it would be writing to nobody, and it must never appear in an editor as
though it did something.

What the pedal does instead is simpler, and it is what a player actually needs:
**file channel 1 goes to OUTPUT A (MONO) and channel 2 goes to OUTPUT B**,
untouched and unmixed. Measured on hardware 2026-08-25 with 441 Hz in one
channel and 1470 Hz in the other, each arriving at its own jack alone — **with
both jacks patched**, which is the condition the rest of this paragraph depends
on. That is why placing a loop on one output jack is something the app does to
the WAV (see `core/include/loopercat/Downmix.hpp`) rather than something it asks
the pedal for.

Two more things that jack carries, both from the manual: OUTPUT A doubles as
the power switch — the pedal is on because a cable is in it — and with OUTPUT B
unplugged the pedal folds its own output to mono, which hardware confirmed.

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

## SYSTEM — the pedal's own settings

`SYSTEM1/2.RC0` are not empty on a working card. Read 2026-10-01 from an RC-5
in use, both files were 716 bytes; the fw 1.10 file taken off hardware earlier,
`fixtures/rc5-system.RC0`, is 717 — one more digit in `Ctl1`. The shape is the
memory files' shape: the same `<database name="RC-5" revision="0">` header, a
single `<sys>` element holding three sections — `SETUP`, `MIDI`, `CTL` — and
after `</database>` the same trailer, `"\n"` plus a little-endian uint32 write
counter. The two files are a pair like MEMORY1/2: within one reading their
texts were identical and only the counters differed — 8 / 7 in the first
reading, 10 / 11 in the second, while the memory pair went 53 / 54 → 55 / 56.
The two pairs count independently: a fw 1.10 field pedal (2026-08-09) carried
SYSTEM at 0x0524 / 0x0525 next to a memory pair far up in the 0x3e65xxxx
range. Which file wins is decided exactly as for MEMORY1/2: the newer counter,
compared as serial numbers so a wrapped counter still reads as newer; a file
whose trailer is missing or unreadable loses the vote; with neither file
readable, SYSTEM1's error is the one reported. All of this is [V] — observed
on the card, and the rule the core reads by:
`core/include/loopercat/SystemFile.hpp` is the reader and the gate,
`core/include/loopercat/usecases/Controls.hpp` decodes CTL.

The pedal writes SYSTEM more often than "when a setting changes". Between the
two readings it was moved from memory 3 to memory 4 and one memory was saved;
the only text change in SYSTEM was `MemoryNumber` 2 → 3, and the SYSTEM
counters advanced by four writes against the memory pair's two. Switching the
current memory is a SYSTEM write [V].

LooperCat reads the pair by the rule above, its history records changes to the
three sections, and Undo writes the pair back the way the memory pair is
written — both files, counters continued past the highest one found. No screen
in the app edits one of these settings yet.

Values below are the ones observed: "field" is the pedal in use (2026-10-01),
"fixture" is `fixtures/rc5-system.RC0` (fw 1.10); where the two agree, one
value is given. Field names and their presence are [V] in every table.

### SETUP

| Field | Meaning | Values | Conf |
| --- | --- | --- | --- |
| `MemoryNumber` | The memory the pedal has selected, zero-based like `<mem id>`: 2 with the pedal on memory 3, 3 with it on memory 4. The pedal's property — an edit of anything else must leave it as found | 0–98 | [V] |
| `DisplayMode` | Display mode, by name; not read against the screen | 5 | [?] |
| `Contrast` | Display contrast, by name | 4 | [?] |
| `UndoRedo` | An undo/redo setting, by name | 0 | [?] |
| `Extent1Min` | Lower bound of a memory range, by name; 0 and 98 fit a zero-based 1–99 span, which is an inference | 0 | [?] |
| `Extent1Max` | Upper bound of the same range, by name | 98 | [?] |

### MIDI

Identical on both pedals. Whether a channel field counts from 0 or 1, and what
`RxCtlCh` = 0 means next to `Omni` = 1, has not been read off the screen;
`docs/midi-protocol/` covers the sysex dialect, not these settings. The
meanings here are the tag names expanded, nothing more.

| Field | Meaning | Values | Conf |
| --- | --- | --- | --- |
| `RxCtlCh` | Receive channel for control messages, by name | 0 | [?] |
| `Omni` | Omni receive, by name | 1 | [?] |
| `RxNoteCh` | Receive channel for notes, by name | 9 | [?] |
| `TxCh` | Transmit channel, by name | 16 | [?] |
| `Sync` | Clock sync, by name | 0 | [?] |
| `ClkOut` | Clock out, by name | 1 | [?] |
| `SyncStart` | Start together with clock, by name | 1 | [?] |
| `PcOut` | Program change out, by name | 1 | [?] |
| `MidiThru` | MIDI thru, by name | 0 | [?] |
| `UsbThru` | USB thru, by name | 0 | [?] |

### CTL

What the pedal switch, the two footswitch contacts of the STOP/MEMORY SHIFT
jack, the expression pedal and the fixed control changes CC#80–87 do. On the
RC-5 this map is global (SETUP > CONTROL on the screen), not per memory. A
value is a position in one of three lists the RC-5 Reference Manual prints
(SETUP > CONTROL, pp. 13–14), counted from zero in the manual's order. That
numbering rule is measured on an RC-500 card and consistent on the RC-5 — the
field pedal sits on exactly the three switch defaults the manual marks — but
it has not been read against the RC-5's own screen, so the names are [M].

| Field | Meaning | Values | Conf |
| --- | --- | --- | --- |
| `Pedal1` | PEDAL FUNC — the pedal switch | PEDAL/CTL1/CTL2 FUNC list, 0–18; field 2 = TRK R/P/S(C (the marked default), fixture 4 = TRK PLY/STP | [M] |
| `Ctl1` | CTL1 FUNC — footswitch 1 on the STOP/MEMORY SHIFT jack | same list; field 8 = TRK STOP(CLR (the marked default), fixture 17 = MEMORY INC | [M] |
| `Ctl2` | CTL2 FUNC — footswitch 2 | same list; field 17 = MEMORY INC (the marked default), fixture 18 = MEMORY DEC | [M] |
| `Exp` | EXP FUNC — the expression pedal | EXP FUNC list, 0–7; field 1 = TRK LEVEL2, fixture 0 = TRK LEVEL1. The manual's marked default, MEMORY LEV2, would be 7, which neither pedal carries — the numbering of this list is in doubt | [M] in doubt |
| `Cc80`–`Cc87` | CC#80 FUNC … CC#87 FUNC — what each fixed control change does | CC#80–87 FUNC list, 0–42; 0 = OFF on all eight, both pedals. The manual marks no default | [M] |

### No auto-save setting in here

Nothing in SYSTEM looks like a setting for saving a memory automatically —
the question behind issue #23, which testers keep asking. The three sections
are the pedal's state and display, its MIDI setup and its controls; no field
names a save, a write-on-change or a confirmation. Whether the pedal has such
a behaviour at all, with no stored setting behind it, is for the manual or
the hardware to answer, not this file.

## What belongs in LooperCat

**Tier 1 — obvious wins, all verified-mechanics writes** (same
transaction/history/generation discipline as every mutation; one hardware
checkpoint per enum to pin the value maps):

- Playback shaping: **Reverse**, **Play level**, **Start/Stop modes** — "how
  does this loop behave live" without touching the pedal menu. (Pan is not on
  this list and never can be: the pedal has no such parameter — see above.)
- The drums: **Rhythm on/off, Pattern, Kit, Beat, Variation, Level** — the
  "what should the rhythm play" ask. Picking a groove per memory from a table
  beats scrolling a one-line pedal display 57 times.

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
pulses; the usual "Disconnect to hear it" note stands — leaving STORAGE makes
the pedal re-read its memory, settings and audio alike (hardware-verified
2026-08-11: a rename, a tempo change and a trim all reached the pedal with no
power cycle, so "reboot to apply" was folklore).

## How to finish the enum maps (fast path)

The classic way: flip a value in the pedal's menu, re-enter storage, diff the
RC0 — tedious at ~30 seconds per value. The fast path is the #9/#22 discovery:
**RQ1 over USB-MIDI reads memory addresses live, no storage round-trip**. If
the RQ1 address space maps onto these fields (likely — Tone Studio's
handshake reads a register the same way), one session with MIDI Monitor while
someone walks the pedal menus yields every enum mapping in near-real-time.
That makes the RQ1 sweep the enabling task for Tier 1's checkpoint.
