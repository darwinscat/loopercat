# LooperCat — user guide

Every feature, in the order you'll meet it. The [README](../README.md) is the
short version; this is the whole map.

## Connect and disconnect

Plug the RC-5 into the Mac over USB and press **Connect**. The app sends the
pedal one SysEx command, the pedal walks itself into STORAGE mode
("Connecting" on its display), macOS mounts the card, and the slot table
fills. No trips through the pedal's SETUP menu.

**Disconnect** is the same trip backwards: playback stops, the volume is
unmounted politely, and the pedal returns to the looper screen on its own.

Two things worth knowing:

- If you put the pedal into STORAGE by hand (SETUP → USB → STORAGE), LooperCat
  picks the volume up just the same — Connect is a convenience, not a gate.
- While in STORAGE the pedal is a card reader, not a looper. Disconnect to
  play the pedal again.

## The slot table

All 99 memory slots in one table: name, duration, bars, tempo, one-shot flag,
and the WAV file behind the slot. It refreshes live as the pedal comes and
goes; **show empty slots** in the toolbar switches between the full 1–99 list
and just the occupied ones.

A row pulses while a job runs against it, and the table re-reads the card
after every write — what you see is what the card says.

## Listening

Double-click a slot to load and play it. The player pane draws the waveform;
click anywhere in it to seek. ▶/■ starts and stops, **Loop** keeps it
rolling, and the gear opens the audio-output settings (pick your interface).

## Editing a slot

- **Rename** — right in the name cell, or right-click → **Rename…**.
- **One Shot** — the flag in the row, or the context-menu toggle: play once
  and stop instead of looping.
- **Set tempo…** — enter the loop's true tempo and LooperCat writes the
  pedal's tempo fields coherently — tempo and measure count together, the
  arithmetic the pedal itself skips on import. The Bars column shows the
  result. (Why this matters: on import the pedal assumes every loop is a
  power-of-two number of measures, so any other loop gets a wrong tempo and
  the onboard rhythm drifts. This menu is the cure.)

## Trim

Select a region in the waveform with the markers and the player previews the
cut **gaplessly** — you hear the loop exactly as it will land on the pedal.
**Trim** commits, **Reset** drops the markers. The original WAV moves to the
app's trash folder first — that is your undo.

## Moving audio

- **Push WAV here…** (empty slot) / **Replace WAV…** (occupied slot) — put a
  WAV on the pedal. The file is converted to the pedal's canonical format on
  the way in; when replacing, the old loop goes to the app's trash first.
- **Pull to folder…** — copy a loop off the pedal as a standard WAV.

## Swap

Drag a row by its grip onto another row to swap the two slots — names, audio,
settings and flags all travel together, and the card's internal file naming
follows the pedal's own rules. The table autoscrolls when you drag near an
edge.

## Clear

Right-click → **Clear slot…**, then choose: **Move to trash** (default — the
WAV is kept in the app's trash folder on this computer, your undo) or delete
outright.

## Maintenance menu

- **Backup configs** — snapshot the pedal's configuration files to the app's
  data folder, on demand. Mutations also back up what they touch, every time.
- **Clean junk** — sweep the macOS droppings (AppleDouble `._*` files and
  friends) off the card. The app also sweeps automatically after its own
  writes; this button is for cards that lived a life before LooperCat.

## The safety model

- **Backups before writes.** Every mutation backs up the slot's configuration
  first; trim, clear and replace move the original audio to the app's trash.
- **Write-generation discipline.** The pedal counts writes inside each memory
  file; LooperCat continues the pedal's own count, so the pedal never meets
  numbers from the future.
- **The doctor.** Card-consistency findings (leftovers of an interrupted
  operation, unreadable files) surface as banners with instructions, not as
  silent repairs.
- **Ghost detection.** If the pedal leaves without saying goodbye — cable
  yank, pedal-side exit — the app notices, stops playback, cleans up the
  stale mount, and waits for the pedal to return. Reconnection is automatic.

## Known issues (macOS)

The weakest layer in the whole story is macOS's own FAT volume handling
(FSKit): under rare timing it can serve stale data or hang a touch-everything
system process. LooperCat is built around this — background probes, no UI
blocked on the volume, device-truth lifecycle — but if the OS itself wedges
with the volume stuck, the reliable cure is: unplug the USB cable and plug it
back. A dedicated known-issues page with the full story ships with v0.9.

## Supported hardware

Developed and verified against a single real BOSS RC-5, firmware **Ver 1.10
build 0050** — the current official release (DEC 2023). The file-format layer
is pinned by conformance tests against values captured from that unit — the
golden fixtures checked in at `fixtures/golden.json`, shared lineage with
[rc5cat](https://github.com/AliceLafox/rc5cat).
