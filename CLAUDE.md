# Glass Spheres — runbook

Everything needed to build, test, deploy and continue Glass Spheres lives in this repo.
Read this first, then `cv/README.md` (the CV protocol) and `README.md` (what the plugin is).

## What this is

Glass Spheres (renamed from "Marbles CV" on 2026-10-04; the repo was split from mpc-x-hacks `marbles/` the same day)
is a native MPC OS plugin (VST2, built with the [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)
framework) that runs Mutable Instruments Marbles' random generators (MIT code, vendored) and sends its seven outputs
(t1-t3, X1-X3, Y) to the MPC X's CV/Gate jacks or out of its own MIDI port. The plugin renders silence. The skin
follows Overcast's (github.com/FullPace/overcast): same knobs, tags, background frame, focus marks and `post_build.py`
(keep the two in sync). The user designs the background (`assets/`, copied to `skin/glass_spheres_bg.jpg`).

## Folder layout

| Path | What |
|---|---|
| `src/engine.cc` | Replaces the firmware main loop: params + modulation → Marbles generators → jacks or MIDI |
| `src/cv_out.{h,cc}` | Jack output: shared jack ownership + sender thread writing to "MPC Private" |
| `src/midi_out.{h,cc}` | MIDI output: one ALSA sequencer client per instance, "Glass Spheres N" (libasound via dlopen) |
| `src/mod.{h,cc}` | Envelopes, LFOs, 8-slot matrix (from Overcast; sources include the own outputs) |
| `src/scales.cc` | Marbles' six factory scales (copied from `settings.cc`) |
| `params.json` | Parameter list = VST parameter order (modulation part from `skin/gen_params.py`) |
| `skin/gen_layout.py` | Writes `layout.conf` (don't edit that by hand) and `skin/qlink_bounds.json` |
| `skin/post_build.py` | Run by `build.sh` on the built skin (names, sliders, focus marks, list catchers; as Overcast's) |
| `patches/` | Framework wrapper patches: host transport, option Q-Link zones, drop-down behaviour (0003) |
| `third_party/eurorack/` | Vendored Marbles DSP + stmlib subset (MIT; see its README for commits) |
| `third_party/mpc-vst-plugins/` | Framework, git submodule (keep at Overcast's commit) |
| `cv/` | CV protocol notes, `cvsniff.c` (in-process logger + injector) |
| `tools/seqdump.c` | Prints a sequencer port's events on the device (`/data/hacks/seqdump <client>:<port> [s]`) |
| `test/` | `sim_test.cc`: native engine run from the Marbles CV days; not updated for MIDI / modulation |

## Mac prerequisites (installed 2026-09-29)

Homebrew: `docker colima docker-buildx bash`. Docker runs in Colima, not Docker Desktop.

- Start: `colima start` — **outside the Bash sandbox** (`dangerouslyDisableSandbox`), otherwise the
  VM's network helper can't connect and every pull times out.
- The user's **VPN blocks the VM's internet**. Pulling images needs the VPN disconnected; building
  with images already present works with it on.
- `~/.colima/default/colima.yaml` mounts `/Users/cypher`, `/Volumes/Daten/Development`,
  `/private/tmp/claude-501` (write `/Users/cypher`, not `~`: colima rewrites `~` to an empty path).
- ARM32 emulation is lost on every VM restart; `build.sh` re-registers it (`tonistiigi/binfmt --install arm`).
- macOS `/bin/bash` 3.2 breaks the framework scripts; `build.sh` uses `/opt/homebrew/bin/bash`.
- **This repo is on an exFAT volume** and Docker's file sharing fails there (files written in the
  container "don't exist" a moment later). `build.sh` therefore mirrors the port to
  `~/.cache/glass-spheres-build` and copies the results back to `build/`.
- Zig 0.16 (`/opt/homebrew/bin/zig`) cross-compiles the small device tools (cvsniff).

## Build and test

```sh
./build.sh          # -> build/glass_spheres.so, build/skin/Padbangers - VST - Glass Spheres/, build/pluginlist-entry.xml
./build.sh test     # framework host test (native clang, ASan/UBSan) — must print PASSED
make -C test run    # engine simulation: gate rates, voltage ranges, transport stop, jack takeover
```

Expected host-test warnings: `note 60 -> rms 0.0000` (the plugin is silent by design).
Expected simulation: internal clock ≈ 1 t1 gate/s; MPC clock at 120 BPM 1/16 → t2 = 8.00/s, t1 + t3 ≈ 8/s;
no gates while stopped; instance b takes CV 1.

Skin preview (Pillow isn't installed on the Mac, so in a container):

```sh
S=~/.cache/glass-spheres-build/port
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$S":/w -w /w python:3.11-slim sh -c \
  'pip install -q --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 third_party/mpc-vst-plugins/tools/studio.py preview "build/skin/Padbangers - VST - Glass Spheres/Plugin Skins" -o /w/build/preview_%d.png'
```

## Deploy

The MPC is `ssh mpcx` (root, `~/.ssh/config`). **Always ask the user before anything restarts the MPC
app** — it kills unsaved projects.

```sh
./deploy.sh          # copies skin + .so to /sdcard/Synths/Padbangers - VST - Glass Spheres/ (staged, md5-checked)
./deploy.sh --yes    # also registers in MPC.settings if missing (stops/starts the app, backs up settings)
```

Registration is already done. For a new build: `./deploy.sh`, then the user removes Glass Spheres from
its track and inserts it again. **That often isn't enough:** MPC keeps the `.so` loaded while any instance exists
(undo history included). `deploy.sh` compares the inode MPC has mapped with the new file and says when the app has
to be restarted instead (ask the user to save first).

## Device facts that matter here

- The button-remap shim loads via `/etc/ld.so.preload` (not the launcher's `LD_PRELOAD`). Never add
  `shim_remap9.so` to an `LD_PRELOAD` as well: loaded twice, the app crashed. Check it is loaded:
  `grep -c shim_remap9 /proc/$(pidof MPC)/maps` (≈7).
- `systemctl start acvs` uses the stock launcher `/usr/bin/az01-launch-MPC` (read-only rootfs).
- App log: `journalctl -u acvs`. Crashes show as `code=dumped, status=11/SEGV`; the minidumps go
  to Akai's Sentry reporter and aren't useful.
- BusyBox userland, no python, no curl on the device.
- Settings: `/media/az01-internal/Settings/MPC/MPC.settings`; installers leave `.bak-*` copies.
- The MPC runs at 44.1 kHz, 128-frame blocks.

## Design notes

- **Jack output:** `cv_out` scans `/proc/self/fd` for a `/dev/snd/midiC*` fd whose rawmidi info is
  an output substream named "…Private" and `write()`s whole messages (`bf 2n msb bf 2n+1 lsb`). A
  sender thread does the writes (never the audio thread), coalescing changes, at most one write per
  2 ms (`kMinWriteIntervalNs`). The thread is joined when the last instance is destroyed, so MPC can
  unload the `.so`.
- **Jack ownership** is process-wide across instances; the latest assignment wins. The app zeroes
  jacks used by CV tracks on transport stop, so the plugin and CV tracks must not share jacks.
- **Gates** are evaluated per 128-frame block; a pulse that starts and ends inside one block is held
  for one block so it isn't lost. Timing resolution ≈ 2.9 ms.
- **Voltage ranges:** jacks are 0-5 V. X/Y ranges 0-2 V and 0-5 V pass through (1 V/oct holds);
  ±5 V is squeezed to 0-5 V (no longer 1 V/oct).
- **Clock = MPC:** `HAS_HOST_TRANSPORT` (our wrapper patch) sends `host_transport = "<playing> <ppq> <bpm>"`
  before each block; the engine makes a square clock at the chosen note value and feeds it to Marbles
  as an external clock (the T rate knob then selects the ratio, as on the module). Patterns reset on
  transport start.
- **Project save:** the wrapper saves only the `state` param; the engine returns `key=value;…` for
  every param and replays it on load.
- **Parameter order** = VST index. Nothing is released yet, so it may still change; once a build is
  shared, only append (saved projects store values by index). `uid` `PbMb` and `so` never change.
- Marbles code is compiled with `-DTEST` (portable C instead of Cortex-M4 asm).
- **Instances live in zeroed memory** (`calloc` + placement new): the firmware's objects are globals, and some
  members aren't set by `Init()` (Clouds' `silence_` made that plugin silent; same precaution here).
- **Clock = MIDI:** notes on the plugin's own track clock the T section (gate = note held, at least 1 ms).
  MIDI Trigger: Any Note / Learn (the next note becomes the trigger note) / One Note (`midi_note`).
- **Q-Links:** `OPTION_QLINK_ZONES` (patch 0002) — the user wanted toggles like "0-64 off, above on" instead of
  the framework's step-per-nudge, which made toggles flicker on a turning knob.
- **Skin:** `"art": "html"` (browser renderer, real Titillium Web): the default bitmap font was tiny and spaced
  out. The panel look comes from `skin/gen_layout.py` (as Overcast).

## MIDI output

- MPC lists the instance's port as a MIDI input ("Glass Spheres 1"); in MPC's MIDI settings the user set it to
  control + track. The receiving track needs that input (or All), the voice's channel and monitoring on. Without
  monitoring the MPC shows activity but plays nothing (seen 2026-10-04).
- Events go out with `snd_seq_event_output_direct` from the audio thread (non-blocking client).
- Voices: gate edge per block (2.9 ms); pitch sampled at the gate's rising edge from the block's last voltages.

## Releasing

As Overcast's runbook (bench, `release.py`, `catalog_check.py`, device test with the zip's `install.sh -y` after the
user saved, `tested.json`, push, `gh release create glass-spheres-vX.Y.Z`; `--prerelease` for betas = catalog
channel beta). Catalog entry `catalog/plugins/glass-spheres.json` in sd88me/mpc-vst-plugins.

## Next steps

- Beta feedback; the CV path with a real VCO (1 V/oct accuracy, update-rate ceiling, see `cv/README.md`).
- Update `test/sim_test.cc` for MIDI and modulation, or drop it.

## Reverse-engineering tools

To see what the app sends to the controller (e.g. for another CV feature), use `cv/`:
build `cvsniff.so` with zig, copy it and `sniff-try.sh` to `/data/hacks/`, ask the user to save, then
`ssh mpcx sh /data/hacks/sniff-try.sh` (log in `/tmp/cvsniff.log`, inject with
`echo "bf 00 40 bf 01 00" > /tmp/cvinject`), and `sh /data/hacks/sniff-try.sh stop` afterwards.
The log grows with every controller write (tmpfs = RAM): don't leave it running for hours.
