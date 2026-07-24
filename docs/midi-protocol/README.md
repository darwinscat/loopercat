# RC-5 MIDI protocol notes

What we know about the RC-5's sysex dialect, learned by capturing BOSS Tone
Studio's traffic and replaying hand-built frames at the hardware. The umbrella
research issue is #9; the capture task and first decode live in #22.

## Framing

Classic Roland DT1/RQ1:

```
F0 41 <device> <model:4> <cmd> <addr:4> <payload...> <checksum> F7
```

- `41` — Roland manufacturer ID; `<device>` is `10` (factory default unit).
- Model ID for the RC-5: `00 00 00 76`.
- `cmd`: `11` = RQ1 (read request, payload is a 4-byte size), `12` = DT1
  (data set, payload is the data).
- Checksum: `128 - (sum of address+payload bytes mod 128)`, wrapping to 0.

The builder for all of this is `core/include/loopercat/Sysex.hpp`, pinned to
the captured frames byte-for-byte by `tests/sysex_tests.cpp`.

## Known registers

| Address       | Meaning                                                      |
| ------------- | ------------------------------------------------------------ |
| `7F 70 00 00` | Storage-mode switch: write `01` = enter STORAGE ("Connecting" on the display, medium exported over USB), write `00` = back to the looper screen. Reads as the current state. |
| `7F 70 00 01` | Ready flag: the pedal announces `01` here (unsolicited DT1) once storage mode is up. |

Both directions verified live on hardware (2026-07-24) with hand-rolled
frames — no Tone Studio involved.

Tone Studio itself moves all DATA over the mounted USB volume; MIDI is only
the mode switch. Parameter-level access at other addresses is unexplored —
an RQ1 sweep is future work under #9.

## Captures

`captures/` holds raw snoize MIDI Monitor logs, one file per scenario:

- `2026-07-24-handshake.mmon` — Tone Studio launch: RQ1 of the storage
  register, DT1 write `01`, the pedal's ack and ready announcement.
  Pedal firmware: unrecorded (TODO next capture); app: BOSS TONE STUDIO for RC.
