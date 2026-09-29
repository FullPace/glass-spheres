# Marbles CV

Mutable Instruments Marbles' random generators as a native MPC OS plugin that plays the **MPC X's
CV/Gate jacks**: random gates (t1-t3) and random, optionally quantized voltages (X1-X3, Y), free
running or locked to the MPC's tempo. Each of the seven outputs can go to any of CV 1-8, and
several instances can run side by side on different jacks.

Status: first build, installed on the device, device testing in progress. See [`CLAUDE.md`](CLAUDE.md)
for the full procedure (build, test, deploy, design notes, next steps) and [`cv/README.md`](cv/README.md)
for how the MPC drives its CV jacks.

```sh
./build.sh           # needs Docker (Colima) — see CLAUDE.md
./deploy.sh          # copy to the MPC over ssh; --yes also registers it (restarts the MPC app)
```

## Using it

Insert **Marbles CV** from *Instrument plugins* on any track (it makes no sound itself).

- **T / X page:** T Rate, Bias, Jitter, mode and range as on the module; Gate Length/Random;
  Clock = Internal or MPC (with the note value in MPC Clock); X Spread, Bias, Steps, mode, range,
  scale and which t output clocks X.
- **Deja Vu / Y page:** deja vu amount and loop length, per-section Off/On/Locked; Y settings.
- **Outputs page:** where each output goes (Off, CV 1-8). Defaults: X1 → CV 1, t1 → CV 2.

The jacks are 0-5 V at 1 V/octave; the ±5 V ranges are squeezed into 0-5 V. Don't use the same jacks
in a CV track: the MPC zeroes those on stop.

## Credits and licenses

- Marbles and stmlib by Emilie Gillet, MIT license — vendored in `third_party/eurorack/`.
- Plugin framework: [sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (submodule).
- Not affiliated with Mutable Instruments or Akai Professional.
