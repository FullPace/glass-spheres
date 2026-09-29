# CV/Gate outputs — protocol

How the MPC app drives the MPC X's 8 CV/Gate jacks, found by logging the app's device writes
from inside the process ([`cvsniff.c`](cvsniff.c), capture in [`cv-capture-1.log`](cv-capture-1.log))
and confirmed by injecting our own values with an oscillator on CV1.

## Protocol

The app sends plain MIDI Control Change messages on **channel 16** (`bf`) to the controller's
**"MPC Private"** rawmidi port (`hw:2,0,1`, `/dev/snd/midiC2D0` subdevice 1). There is no
handshake or configuration: nothing is sent at startup, only these messages when a value changes.

| Jack | MSB | LSB |
|---|---|---|
| CV 1 | CC 0 | CC 1 |
| CV 2 | CC 2 | CC 3 |
| CV *n* | CC 2(*n*−1) | CC 2(*n*−1)+1 |

```
value = MSB << 7 | LSB          14 bit, 0..16383
volts = value / 3276.8          0..5 V, 1 V/octave  (one semitone = 273.07)
```

- Gate on = `7f 7f`, gate off = `00 00`.
- Notes from a CV program land exactly on 1 V/oct: note 2 V = `33 19` (6554).
- **Verified by ear:** 1 V → 2 V → 3 V are clean octaves; toggling only the LSB (MSB 25, LSB 0 ↔ 127,
  ≈46 cents) is audible, so the full 14 bits reach the DAC.
- Velocity and mod wheel in a CV program are sent 7-bit into both bytes (`bf 06 45 bf 07 45`).

Example — CV1 to 2 V, CV2 gate on, then gate off:

```
bf 00 33 bf 01 19  bf 02 7f bf 03 7f
bf 02 00 bf 03 00
```

## Behaviour to design around

- **On transport stop the app zeroes the jacks its CV tracks use** (`bf 00 00 bf 01 00 ...`).
  A plugin must not share jacks with CV tracks.
- The app holds the rawmidi device exclusively. Only code **inside the MPC process** (an
  `LD_PRELOAD` shim, or a VST plugin) can write to it: find the fd open on `/dev/snd/midiC2D0`
  whose `SNDRV_RAWMIDI_IOCTL_INFO` subdevice is 1 ("MPC Private") and `write()` complete messages.
  The app always sends full status bytes, so whole-message writes don't break its stream.
- Other traffic on the same port: LED CCs on channel 1 (`b0 ..`) and SysEx (`f0 47 7f 3a ..`)
  for pad colours and displays.
- Update rate: 108 updates/s from a shell loop was accepted without errors; that loop was the
  limit, not the controller. The controller's real ceiling is still untested.

## Tools

- [`cvsniff.c`](cvsniff.c) — `LD_PRELOAD` logger for device writes, ALSA control writes and
  sequencer events in the MPC process, plus a FIFO injector:
  `echo "bf 00 33 bf 01 19" > /tmp/cvinject` sends bytes to the controller's CV port.
  Build: `zig cc -target arm-linux-gnueabihf.2.31 -shared -fPIC -O2 -o cvsniff.so cvsniff.c`.
- [`sniff-try.sh`](sniff-try.sh) — runs it: the stock launcher `/usr/bin/az01-launch-MPC` is
  read-only, so a copy with `cvsniff.so` prepended to `LD_PRELOAD` is bind-mounted over it
  (auto-reverts if the app doesn't come up; `sniff-try.sh stop` undoes it; gone after a reboot).
  Copy both files to `/data/hacks/`. Don't also list `shim_remap6.so` in `LD_PRELOAD`: it already
  loads via `/etc/ld.so.preload`, and loading it twice crashed the app.
- A hook must resolve the real function lazily (`dlsym(RTLD_NEXT, ...)` on first call): the other
  preloaded libraries' constructors call `write()` before ours has run, and an unresolved pointer
  there segfaults the app at startup.
- [`cv-capture-1.log`](cv-capture-1.log) — the capture the protocol was worked out from (notes on
  CV1 = pitch, CV2 = gate; later CV4 = velocity).
