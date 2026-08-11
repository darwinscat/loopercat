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

Four things worth knowing:

- If you put the pedal into STORAGE by hand (SETUP → USB → STORAGE), LooperCat
  picks the volume up just the same — Connect is a convenience, not a gate.
- While in STORAGE the pedal is a card reader, not a looper. Disconnect to
  play the pedal again.
- **Connect needs an idle pedal.** The firmware will not hand over the card
  while a loop is playing or recording. LooperCat re-sends the request a
  couple of times, then says so plainly on a banner — stop the loop on the
  pedal, then press Connect again.
- Right after a Disconnect the pedal re-boots its USB side for a couple of
  seconds; Connect stays disabled until the pedal is properly back on the
  bus. Quitting the app while connected also disconnects first — the pedal
  walks itself back to the looper screen.

### The first Connect on macOS

The first time the card mounts, macOS asks whether LooperCat may access
files on a removable volume. That volume is the pedal's card — LooperCat
reads and writes nothing else — so allow it. The question is asked once.

If you clicked **Don't Allow** by mistake, every later Connect will mount the
card and then show nothing. The switch lives in **System Settings → Privacy &
Security → Files & Folders → LooperCat → Removable Volumes**: turn it on,
then Connect again.

### The first run on Windows

LooperCat for Windows is a single portable exe inside the release zip — no
installer, no admin rights: unzip anywhere and run. Settings live in your
user profile; beyond that the pedal's card is all it ever touches.

The builds are not code-signed yet, so the first launch of a downloaded exe
meets SmartScreen: click **More info → Run anyway**. Signed builds are on the
roadmap; until then that one extra click is the whole ceremony.

Connect works the same as on the Mac — one press, the pedal walks itself into
STORAGE, the card comes up on a drive letter. There is no access prompt:
Windows has no removable-volume permission gate.

## The slot table

All 99 memory slots in one table: name, duration, bars, tempo, the behaviour
lamps and the WAV file behind the slot. It refreshes live as the pedal comes
and goes; **show empty slots** in the toolbar switches between the full 1–99
list and just the occupied ones.

A lit lamp means the setting is on for that slot, and clicking it flips the
setting straight from the row. Which lamps appear is up to you — see
**Settings → Columns** below; **One Shot** is shown out of the box and **Play
Count-In** is not, so the table stays as short as your playing needs.

A row pulses while a job runs against it, and the table re-reads the card
after every write — what you see is what the card says.

## Listening and setting up: the bottom pane

The pane under the table has two tabs for whichever slot is selected, and
**⌘I** flips between them.

- **Audio** — the waveform: click anywhere in it to seek, ▶/■ starts and
  stops, **Loop** keeps it rolling. Double-clicking a row loads and plays it.
- **Properties** — what the slot *is*: its name, its tempo, and one card per
  setting LooperCat can change for it. Settings arrive here as they are
  verified against real hardware, one at a time, rather than as a wall of
  fields copied out of the file format.

## Settings

The gear in the top row opens the app's own settings:

- **Audio** — which interface and outputs the preview plays through.
- **Columns** — which behaviour lamps the slot table shows. The pedal's own
  facts (name, duration, bars, tempo, file) are always there.

## Editing a slot

Select the slot and open the **Properties** tab. Everything there is written
the moment you commit it, and the note on the right says the one thing you
have to do afterwards: **Disconnect** — that is when the pedal re-reads its
memory. No power cycling.

- **Name** — twelve characters, the field is exactly that wide because the
  pedal's display is. Enter commits, Esc puts it back. (Double-clicking the
  name cell in the table edits it in place too.)
- **Tempo** — the loop's true tempo. The line beside the field shows the bar
  count that follows from it *before* you commit, and LooperCat writes both
  coherently — the arithmetic the pedal skips on import. (Why this matters:
  on import the pedal assumes every loop is a power-of-two number of measures,
  so any other loop gets a wrong tempo and the onboard rhythm drifts against
  the music. This field is the cure.)
- **Play Count-In** — one switch: the pedal plays one measure of count at the
  slot's tempo, then the loop — a backing track becomes stage-ready in one
  tap. The name is the pedal's own **PLAY COUNT**; its separate REC COUNT (a
  count-in before recording) is a different setting and is not touched. If the
  slot has a rhythm pattern you picked on the pedal, the card says so before
  you switch it on: a *silent* rhythm section is what the count borrows, and
  it will replace that pattern. A rhythm that is actually playing is left
  alone — the count simply joins it, and switching the count off later leaves
  your groove where it was.
- **One Shot** — play once and stop instead of looping.

Both switches are also the lamps in the table, if you keep those columns on:
one click there does the same thing.

Right-clicking a row is for operations — pushing, pulling and clearing audio —
not for settings.

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

Other RC-series pedals (RC-500, RC-505, RC-10R…) are **not supported**. Their
cards look deceptively similar — an RC-500 exports the same top-level
structure with the same field names — but the dialect differs exactly where
it hurts (tempo arithmetic, two-track audio layout). That is why LooperCat
checks who it is talking to: a foreign card is refused at the door by name
(#35), nothing read, nothing written.
