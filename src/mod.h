// Modulation for Glass Spheres: 2 envelopes, 2 LFOs and an 8-slot matrix onto Marbles' CV inputs.
//
// Runs once per 128-frame block. Sources are the envelopes, the LFOs, Glass Spheres' own outputs (t1-t3, X1-X3, Y,
// from the previous block, like patching an output back into an input) and note velocity. The matrix output is
// added to the knob values the way the module adds its CV inputs to the pots; T Clock and X Clock are gates
// (on above 0.5) that clock the section like a cable in the module's CLOCK jacks.
#pragma once

#include <stdint.h>

namespace mod {

const int kNumEnvs = 2;
const int kNumLfos = 2;
const int kNumSlots = 8;

enum Source { SRC_OFF, SRC_LFO1, SRC_LFO2, SRC_ENV1, SRC_ENV2, SRC_T1, SRC_T2, SRC_T3, SRC_X1, SRC_X2, SRC_X3, SRC_Y,
              SRC_VELOCITY, SRC_LAST };
enum Dest { DST_OFF, DST_T_RATE, DST_T_BIAS, DST_JITTER, DST_DEJA_VU, DST_STEPS, DST_SPREAD, DST_X_BIAS, DST_T_CLOCK,
            DST_X_CLOCK, DST_LAST };
enum LfoShape { SHAPE_SINE, SHAPE_TRIANGLE, SHAPE_SAW_UP, SHAPE_SAW_DOWN, SHAPE_SQUARE, SHAPE_SAMPLE_HOLD,
                SHAPE_SMOOTH_RANDOM, SHAPE_LAST };
enum EnvTrigger { TRIG_MIDI, TRIG_LFO1, TRIG_LFO2, TRIG_T1, TRIG_T2, TRIG_T3, TRIG_LAST };

struct EnvSettings { float attack, decay, sustain, release; int trigger; };   // knobs 0..1, trigger: EnvTrigger
struct LfoSettings { int shape; float rate; bool sync; int division; };      // rate 0..1, division: index

struct Settings {
  EnvSettings env[kNumEnvs];
  LfoSettings lfo[kNumLfos];
  int source[kNumSlots];
  int dest[kNumSlots];
  float amount[kNumSlots];   // -1..1
};

struct Inputs {
  bool midi_gate;        // a MIDI note is held
  bool midi_retrigger;   // a note-on arrived since the last block
  float velocity;        // last note-on velocity, 0..1
  bool playing;          // MPC transport
  double ppq;            // song position in quarter notes (-1: unknown)
  double bpm;
  bool gate[3];          // t1-t3 (previous block)
  float x[3];            // X1-X3 over their range, -1..1 (previous block)
  float y;               // Y, -1..1
};

class Modulation {
 public:
  void Init(uint32_t seed);
  // One control-rate step of dt seconds; afterwards out(DST_x) is the summed modulation of that destination.
  void Process(const Settings& settings, const Inputs& inputs, float dt);
  float out(int dest) const { return out_[dest]; }
  bool used(int dest) const { return used_[dest]; }   // a slot routes something there (a patched jack)

 private:
  float Lfo(int i, const LfoSettings& s, const Inputs& in, float dt);
  float Envelope(int i, const EnvSettings& s, bool gate, bool retrigger, float dt);
  float Random();

  float lfo_phase_[kNumLfos];
  float lfo_value_[kNumLfos];
  float lfo_held_[kNumLfos], lfo_from_[kNumLfos], lfo_to_[kNumLfos];
  enum Stage { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };
  Stage env_stage_[kNumEnvs];
  float env_value_[kNumEnvs];
  bool env_gate_[kNumEnvs];
  float out_[DST_LAST];
  bool used_[DST_LAST];
  uint32_t rng_;
};

}  // namespace mod
