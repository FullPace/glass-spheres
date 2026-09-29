# Mutable Instruments code (vendored)

Unmodified subset of Emilie Gillet's Eurorack firmware, MIT license (see `stmlib/LICENSE`;
the Marbles files carry the same notice in their headers).

- `marbles/` — from https://github.com/pichenettes/eurorack @ 08460a6 (random/, ramp/, resources.*)
- `stmlib/` — from https://github.com/pichenettes/stmlib @ d18def8 (only the headers/sources Marbles' DSP uses)

The hardware parts of the firmware (drivers, UI, CV reader, settings storage) are not included;
`src/engine.cc` replaces the firmware main loop.
