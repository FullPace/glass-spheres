// A MIDI output port of our own: MPC OS has no MIDI output for plugins.
//
// Each instance opens an ALSA sequencer client ("Glass Spheres 1", "Glass Spheres 2", ...) with one readable port.
// MPC lists it as a MIDI input, so a MIDI track can take the notes and play any instrument. libasound is loaded at
// runtime (it is already in the MPC process), so the build needs no ALSA headers; where it's missing (the Mac test
// build) the port simply isn't there.
#pragma once

#include <stdint.h>

namespace midi_out {

struct Port;

Port* Open();                  // NULL if ALSA isn't available
void Close(Port* port);

// Non-blocking sends, straight to the port's subscribers. channel 0..15.
void NoteOn(Port* port, int channel, int note, int velocity);
void NoteOff(Port* port, int channel, int note);
void Control(Port* port, int channel, int cc, int value);

}  // namespace midi_out
