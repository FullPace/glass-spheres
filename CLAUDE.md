# Marbles CV — runbook

Everything needed to build, test, deploy and continue the Marbles CV plugin lives in this folder.
Read this first, then `cv/README.md` (the CV protocol) and `README.md` (what the plugin is).

## What this is

A native MPC OS plugin (VST2, built with the [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)
framework) that runs Mutable Instruments Marbles' random generators (MIT code, vendored) and writes
its seven outputs (t1-t3, X1-X3, Y) to the MPC X's CV/Gate jacks. The plugin renders silence.

Status (2026-09-29):
- CV protocol found and verified by ear with an oscillator on CV1: octaves 1/2/3 V are clean, the
  LSB (sub-semitone) is audible. The user's oscillator was a drone oscillator, so smooth sweeps could
  not be judged.
- Plugin v0.1 built, host test PASSED, native simulation OK, installed and registered on the device.
- **Not yet confirmed on the device:** the plugin actually moving the jacks, the skin, Q-Links,
  Clock = MPC sync, project save/reload, several instances. The user is getting a proper VCO.

## Folder layout

| Path | What |
|---|---|
| `src/engine.cc` | Replaces the firmware main loop: params → Marbles generators → jacks |
| `src/cv_out.{h,cc}` | Jack output: shared jack ownership + sender thread writing to "MPC Private" |
| `src/scales.cc` | Marbles' six factory scales (copied from `settings.cc`) |
| `params.json` | Parameter list = VST parameter order |
| `layout.conf` | Skin layout (3 tabs), edit with the framework's SkinStudio |
| `vst.json` | Framework build description |
| `patches/0001-wrapper-host-transport.patch` | Adds `HAS_HOST_TRANSPORT` to the framework wrapper |
| `third_party/eurorack/` | Vendored Marbles DSP + stmlib subset (MIT; see its README for commits) |
| `third_party/mpc-vst-plugins/` | Framework, git submodule (no license file upstream, so not vendored) |
| `test/` | `sim_test.cc`: native run of the engine with a recording cv_out |
| `cv/` | Protocol notes, `cvsniff.c` (in-process logger + injector), `sniff-try.sh`, capture log |
| `build.sh`, `deploy.sh` | Build and install |

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
  `~/.cache/marbles-cv-build` and copies the results back to `build/`.
- Zig 0.16 (`/opt/homebrew/bin/zig`) cross-compiles the small device tools (cvsniff).

## Build and test

```sh
./build.sh          # -> build/marbles_cv.so, build/skin/Padbangers - VST - Marbles CV/, build/pluginlist-entry.xml
./build.sh test     # framework host test (native clang, ASan/UBSan) — must print PASSED
make -C test run    # engine simulation: gate rates, voltage ranges, transport stop, jack takeover
```

Expected host-test warnings: `note 60 -> rms 0.0000` (the plugin is silent by design).
Expected simulation: internal clock ≈ 1 t1 gate/s; MPC clock at 120 BPM 1/16 → t2 = 8.00/s, t1 + t3 ≈ 8/s;
no gates while stopped; instance b takes CV 1.

Skin preview (Pillow isn't installed on the Mac, so in a container):

```sh
S=~/.cache/marbles-cv-build/port
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$S":/w -w /w python:3.11-slim sh -c \
  'pip install -q --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 third_party/mpc-vst-plugins/tools/studio.py preview "build/skin/Padbangers - VST - Marbles CV/Plugin Skins" -o /w/build/preview_%d.png'
```

## Deploy

The MPC is `ssh mpcx` (root, `~/.ssh/config`). **Always ask the user before anything restarts the MPC
app** — it kills unsaved projects.

```sh
./deploy.sh          # copies skin + .so to /sdcard/Synths/Padbangers - VST - Marbles CV/ (staged, md5-checked)
./deploy.sh --yes    # also registers in MPC.settings if missing (stops/starts the app, backs up settings)
```

Registration is already done. For a new build: `./deploy.sh`, then the user removes Marbles CV from
its track and inserts it again — no restart needed.

## Device facts that matter here

- The button-remap shim loads via `/etc/ld.so.preload` (not the launcher's `LD_PRELOAD`). Never add
  `shim_remap6.so` to an `LD_PRELOAD` as well: loaded twice, the app crashed. Check it is loaded:
  `grep -c shim_remap6 /proc/$(pidof MPC)/maps` (≈7).
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

## Next steps

1. With a real VCO on CV1: run the plugin, check smooth X glides (X Steps < 0.5), 1 V/oct accuracy
   per jack (add a per-jack calibration offset/scale if needed), and find the controller's update-rate
   ceiling (lower `kMinWriteIntervalNs` until glitches appear; the cvsniff injector helps).
2. Device checklist with the user: plugin in the browser, skin renders, Q-Links, Clock = MPC sync,
   save/reload project, two instances on different jacks, remove instance → jacks go to 0 V.
3. CPU check: `third_party/mpc-vst-plugins/tools/bench.sh build/marbles_cv.so <ip>` (see its docs/BENCH.md).
4. Skin design (currently the auto-layout): SkinStudio via `third_party/mpc-vst-plugins/SkinStudio.command`.
5. Later: register mode / external input via MIDI notes, per-jack calibration, offset for ±5 V.
6. Before sharing publicly: the name "Marbles" is Mutable Instruments'; the framework has no license
   file; the `HAS_HOST_TRANSPORT` patch could go upstream as a PR.

## Reverse-engineering tools

To see what the app sends to the controller (e.g. for another CV feature), use `cv/`:
build `cvsniff.so` with zig, copy it and `sniff-try.sh` to `/data/hacks/`, ask the user to save, then
`ssh mpcx sh /data/hacks/sniff-try.sh` (log in `/tmp/cvsniff.log`, inject with
`echo "bf 00 40 bf 01 00" > /tmp/cvinject`), and `sh /data/hacks/sniff-try.sh stop` afterwards.
The log grows with every controller write (tmpfs = RAM): don't leave it running for hours.
