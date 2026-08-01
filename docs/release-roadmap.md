# Road to the first public release

Where LooperCat stands, what remains before v1.0, and how the repo flips
public. Milestones are sprint-sized; order within a milestone is flexible,
order between milestones mostly is not.

## Where we are (v0.1.0-dev, 2026-07-25)

Working today, all hardware-checkpointed: slot browser with live refresh,
playback with waveform + markers, the full mutation set (rename, one-shot,
tempo+bars, push/pull, trim with gapless preview, clear, drag-to-swap), config backups,
junk hygiene, device-truth lifecycle (ghost detection, auto-cleanup,
auto-mount), **Connect/Disconnect driving the pedal over cracked sysex**, the
write-generation discipline, native menu bar, About. Format knowledge that
does not exist anywhere else in public: the storage-mode register, the
trailer generation counter, the import-tempo arithmetic.

## M1 — Feature-complete core → moved to v1.x

**Scope decision (2026-07-26): the first release ships the current feature
set.** Everything in "Where we are" is hardware-checkpointed and complete on
its own; the items below move out of v0.9 and become the first feature release
after the flip.

- [ ] #26 remainder: BPM field in the push flow (filename prefill).
- [ ] #28: audio tempo detection via felitronics-core#57 (LoopTempo).
- [ ] #21: error-observation ghost detection (the medium-pull case).
- [ ] Memory Settings editor, Tier 1 (see `pedal-settings.md` and its issue):
      playback shaping + the drums. Gated on enum verification.
- [ ] #9 RQ1 address sweep — enables the enum harvesting for the editor and
      possibly live state reads.
- [ ] rc5cat#9: port the generation-counter fix back (family hygiene; the
      public format reference must not keep teaching the misread).

## M2 — Hardening

- [ ] Hardware QA matrix: every mutation × (empty slot / occupied / slot 99 /
      full card), Connect/Disconnect cycles, yank drills, wedge recovery.
- [ ] #35: family guard — a non-RC-5 card is recognized and refused with an
      honest banner, hands-off otherwise. **Release blocker** (Alisa's audit):
      a real RC-500 MEMORY1.RC0 parses clean — same 99-slot dialect; the
      `<database name>` attribute is the discriminator.
- [ ] Long-session soak: hours-connected stability, poll cadence, memory.
- [ ] Error copy pass: every banner/toast reads as an instruction, not a log.
- [ ] Known-issues doc for the macOS FSKit fragility (wedges, ghost mounts)
      with the app's mitigations — users will hit it; the app should look
      smarter than the OS, and the doc proves it is deliberate.
- [ ] Crew review over core/ before the flip.

## M3 — Packaging (the original Sprint 5)

- [ ] Developer ID signing + notarization + stapled DMG.
- [ ] Per-build version stamping generator (the TabbyEQ pattern) replacing
      the configure-time stamp.
- [ ] UpdateChecker feed wired to GitHub Releases (appkit convention).
- [ ] App icon: .icns from the EARS mark ✓ (landed with #15/#20); verify
      Retina sizes in Finder/Dock.
- [ ] README for humans: what it does, screenshots, install, the STORAGE
      story, supported hardware (RC-5 v1), building from source.
- [ ] `docs/midi-protocol/` and `docs/pedal-settings.md` re-read as public
      documents (they were written future-public; verify once more).

## M4 — The public flip

- [ ] History audit: the future-public discipline held from day 0 — verify
      with a final sweep (no secrets, no local paths, no third-party code
      without license headers, LICENSE = AGPL-3.0 present).
- [ ] Licensing note in README: AGPL-3.0 app + JUCE under GPLv3 — one-way
      compatible (GPLv3 code may be conveyed inside an AGPLv3 work, GPLv3 §13);
      state it explicitly so nobody has to re-derive it.
- [ ] Trademark wording: "LooperCat — companion app for the BOSS RC-5" is
      nominative use; BOSS/Roland marks stay out of names, icons, bundle IDs.
- [ ] Repo transfer AliceLafox → darwinscat org (agreed: after Sprint 5),
      then flip to public. Update the appkit/core FetchContent URLs if any
      still point at private remotes.
- [ ] darwinscat.com/loopercat page + cross-links from the Felitronics suite
      (business side lives in the darwinscat.com repo, label scope:loopercat).
- [ ] v0.9 public beta release → feedback window → v1.0.

## Release criteria (v1.0)

1. Every shipped feature hardware-checkpointed; zero known data-loss paths.
2. A fresh Mac (no dev tools) installs the DMG and browses a pedal in under
   a minute — including the Connect button doing the STORAGE dance for them.
3. The doctor never cries wolf on a healthy pedal-written card.
4. Docs answer the three real questions: install, connect, "why does my
   tempo look weird" (it's the pedal; we fix it — that is the pitch).

## Risks

- **macOS FSKit** remains the wobbliest layer (wedges, ghost mounts,
  cache lies). Mitigated by the lifecycle machine + #21; documented in M2.
  A macOS update can shift this ground — the known-issues doc keeps us honest.
- **Single hardware sample**: everything is verified against one RC-5 on one
  firmware. Beta feedback is the real matrix; the doctor + backups keep
  early users safe.
- **Sysex writes on shared registers**: the storage-mode register is
  understood; anything from the future RQ1 sweep ships read-only first.
- **Notarization logistics**: Developer ID account and signing secrets stay
  outside the repo; CI signing is a later luxury, manual signing is fine for
  v1.
