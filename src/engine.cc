// Glass Spheres: Mutable Instruments Marbles' random generators as an MPC plugin whose outputs drive the MPC X's
// CV/Gate jacks or play MIDI notes.
//
// Replaces the firmware main loop (marbles/marbles.cc): parameters come from plugin params instead of
// pots/CV, the T section clocks from its own rate or from the MPC transport, and the seven outputs
// (t1-t3, X1-X3, Y) go to any of the eight jacks through cv_out (Output = CV), or out of the instance's own MIDI
// port (Output = MIDI): t1-t3 are three voices playing the notes of X1-X3, Y is a CC. The plugin renders silence.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <new>

#include "cv_out.h"
#include "midi_out.h"
#include "mod.h"

#include "marbles/random/random_generator.h"
#include "marbles/random/random_stream.h"
#include "marbles/random/t_generator.h"
#include "marbles/random/x_y_generator.h"
#include "stmlib/utils/gate_flags.h"

extern "C" {
#include "engine.h"
}

namespace marbles {
extern const Scale preset_scales[6];
}

using namespace marbles;
using stmlib::GateFlags;

namespace {

const float kSampleRate = 44100.0f;
const size_t kSubBlock = 8;  // Marbles' generators run in short blocks, as on the module

enum Param {
  P_T_RATE, P_T_BIAS, P_T_JITTER, P_T_MODEL, P_T_RANGE, P_T_PW, P_T_PW_RANDOM,
  P_DEJA_VU, P_LENGTH, P_T_DEJA_VU, P_X_DEJA_VU,
  P_X_SPREAD, P_X_BIAS, P_X_STEPS, P_X_MODE, P_X_RANGE, P_X_SCALE, P_X_CLOCK,
  P_Y_SPREAD, P_Y_BIAS, P_Y_STEPS, P_Y_DIVIDER, P_Y_RANGE,
  P_CLOCK, P_CLOCK_DIV,
  P_OUT_T1, P_OUT_T2, P_OUT_T3, P_OUT_X1, P_OUT_X2, P_OUT_X3, P_OUT_Y,
  P_MIDI_TRIGGER, P_MIDI_NOTE,
  P_OUTPUT,
  // Output = MIDI: 3 voices (gate, pitch source, channel, note) and 2 CC lanes (source, channel, CC number)
  P_VOICE_FIRST,
  P_CC_FIRST = P_VOICE_FIRST + 3 * 4,
  P_CC_LAST = P_CC_FIRST + 2 * 3 - 1,
  // MODULATION tab: per envelope attack, decay, sustain, release, trigger; per LFO shape, rate, sync, division
  // (unused: synced, the rate knob picks the note value); per matrix slot source, destination, amount
  P_MOD_FIRST,
  P_ENV_FIRST = P_MOD_FIRST,
  P_LFO_FIRST = P_ENV_FIRST + mod::kNumEnvs * 5,
  P_SLOT_FIRST = P_LFO_FIRST + mod::kNumLfos * 4,
  P_MANUAL = P_SLOT_FIRST + mod::kNumSlots * 3,   // the MANUAL tab's topic; skin only
  P_LAST
};

struct ParamInfo {
  const char* key;
  float def;
  bool option;  // an option index, reported as an integer
};

// Keys and defaults; params.json has the same list (names, ranges, option labels) in VST order. The modulation
// entries are filled in by InitModParams(); params.json gets them from skin/gen_params.py.
// Defaults follow the module's factory state, except the X/Y range (0-5 V: the jacks can't go
// negative) and the clock (internal, so it runs without the transport).
ParamInfo kParams[P_LAST] = {
  { "t_rate", 0.5f, false },
  { "t_bias", 0.5f, false },
  { "t_jitter", 0.0f, false },
  { "t_model", 0, true },
  { "t_range", 1, true },
  { "t_pw", 0.5f, false },
  { "t_pw_random", 0.0f, false },
  { "deja_vu", 0.5f, false },
  { "length", 7, true },
  { "t_deja_vu", 0, true },
  { "x_deja_vu", 0, true },
  { "x_spread", 0.5f, false },
  { "x_bias", 0.5f, false },
  { "x_steps", 0.5f, false },
  { "x_mode", 0, true },
  { "x_range", 1, true },
  { "x_scale", 0, true },
  { "x_clock", 0, true },
  { "y_spread", 0.5f, false },
  { "y_bias", 0.5f, false },
  { "y_steps", 0.0f, false },
  { "y_divider", 6, true },
  { "y_range", 1, true },
  { "clock", 0, true },
  { "clock_div", 2, true },
  { "out_t1", 2, true },  // CV 2
  { "out_t2", 0, true },
  { "out_t3", 0, true },
  { "out_x1", 1, true },  // CV 1
  { "out_x2", 0, true },
  { "out_x3", 0, true },
  { "out_y", 0, true },
  { "midi_trigger", 0, true },  // Any Note
  { "midi_note", 36, false },
  { "output", 0, true },          // CV
  // voice n: gate T1..T3, pitch X1..X3, channel n, root note C2 (the note at 0 V; X at 1 V/octave adds semitones)
  { "v1_gate", 1, true }, { "v1_pitch", 0, true }, { "v1_channel", 0, true }, { "v1_note", 48, false },
  { "v2_gate", 2, true }, { "v2_pitch", 1, true }, { "v2_channel", 1, true }, { "v2_note", 48, false },
  { "v3_gate", 3, true }, { "v3_pitch", 2, true }, { "v3_channel", 2, true }, { "v3_note", 48, false },
  // CC 1: Y on channel 1 as the mod wheel; CC 2: off (CC 74, cutoff, when switched on)
  { "cc1_source", 4, true }, { "cc1_channel", 0, true }, { "cc1_number", 1, false },
  { "cc2_source", 0, true }, { "cc2_channel", 0, true }, { "cc2_number", 74, false },
};

char mod_keys[P_MANUAL - P_MOD_FIRST][16];

void InitModParams() {
  static bool done = false;
  if (done) return;
  done = true;
  int k = 0;
  const char* env_keys[5] = { "attack", "decay", "sustain", "release", "trig" };
  const float env_defaults[5] = { 0.1f, 0.5f, 0.7f, 0.5f, mod::TRIG_MIDI };
  for (int e = 0; e < mod::kNumEnvs; ++e) {
    for (int j = 0; j < 5; ++j, ++k) {
      snprintf(mod_keys[k], sizeof mod_keys[k], "env%d_%s", e + 1, env_keys[j]);
      kParams[P_MOD_FIRST + k] = { mod_keys[k], env_defaults[j], j == 4 };
    }
  }
  const char* lfo_keys[4] = { "shape", "rate", "sync", "div" };
  const float lfo_defaults[4] = { mod::SHAPE_SINE, 0.5f, 0, 4 };
  for (int l = 0; l < mod::kNumLfos; ++l) {
    for (int j = 0; j < 4; ++j, ++k) {
      snprintf(mod_keys[k], sizeof mod_keys[k], "lfo%d_%s", l + 1, lfo_keys[j]);
      kParams[P_MOD_FIRST + k] = { mod_keys[k], lfo_defaults[j], j != 1 };
    }
  }
  const char* slot_keys[3] = { "src", "dst", "amt" };
  for (int m = 0; m < mod::kNumSlots; ++m) {
    for (int j = 0; j < 3; ++j, ++k) {
      snprintf(mod_keys[k], sizeof mod_keys[k], "mod%d_%s", m + 1, slot_keys[j]);
      kParams[P_MOD_FIRST + k] = { mod_keys[k], 0.0f, j != 2 };   // amount in %
    }
  }
  kParams[P_MANUAL] = { "manual_page", 0, true };
}

const char* const kDivisionNames[] = { "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32" };
int LfoDivisionOf(float rate_knob) { return rate_knob < 0 ? 0 : (rate_knob > 1 ? 7 : static_cast<int>(rate_knob * 7.0f + 0.5f)); }

enum OutputMode { OUTPUT_CV, OUTPUT_MIDI };
enum VoicePitch { PITCH_X1, PITCH_X2, PITCH_X3, PITCH_Y, PITCH_FIXED };   // voice gate: 0 off, 1..3 = T1..T3
enum CcSource { CC_OFF, CC_X1, CC_X2, CC_X3, CC_Y };                     // CC lane source
const int kVelocity = 100;

enum Output { OUT_T1, OUT_T2, OUT_T3, OUT_X1, OUT_X2, OUT_X3, OUT_Y, NUM_OUTPUTS };

const int kLoopLengths[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16 };
const int kNumLoopLengths = sizeof(kLoopLengths) / sizeof(kLoopLengths[0]);
const Ratio kYDividers[] = {
  { 1, 64 }, { 1, 48 }, { 1, 32 }, { 1, 24 }, { 1, 16 }, { 1, 12 },
  { 1, 8 }, { 1, 6 }, { 1, 4 }, { 1, 3 }, { 1, 2 }, { 1, 1 },
};
const int kNumYDividers = sizeof(kYDividers) / sizeof(kYDividers[0]);
const int kClockPulsesPerBeat[] = { 1, 2, 4, 8 };  // 1/4, 1/8, 1/16, 1/32 notes

template<typename T> T Clamp(T x, T lo, T hi) { return x < lo ? lo : (x > hi ? hi : x); }

struct Instance {
  float param[P_LAST];
  int jack[NUM_OUTPUTS];  // -1 = not connected

  RandomGenerator random_generator;
  RandomStream random_stream;
  TGenerator t_generator;
  XYGenerator xy_generator;

  // Host transport, updated before every block.
  bool playing;
  bool was_playing;
  double ppq;
  double bpm;
  bool clock_level;

  // Clock = MIDI: notes on the plugin's track clock the T section.
  int midi_held;           // matching notes currently down
  int midi_pulse;          // samples left of the minimum pulse after a note-on
  int notes_held;          // any notes down (envelope trigger MIDI)
  bool midi_retrigger;
  float velocity;

  mod::Modulation modulation;
  bool t_clock_level, x_clock_level;   // the matrix's clock gates
  float last_volts[4];                  // X1-X3, Y at the end of the previous block (modulation sources)
  GateFlags x_clock[kSubBlock];

  // Gates seen during the current block, so pulses shorter than a block still reach the jack.
  bool gate_level[3];
  bool gate_rose[3];
  bool gate_sent[3];

  // Output = MIDI
  midi_out::Port* midi;
  int note_on[3];          // the note each voice is holding, -1 = none
  int note_channel[3];
  int last_cc[2];          // the last value each CC lane sent, -1 = none

  float ramp_buffer[kSubBlock * 4];
  bool gates[kSubBlock * 2];
  float voltages[kSubBlock * 4];
  GateFlags clock[kSubBlock];
};

int OptionIndex(const Instance* s, Param p) { return static_cast<int>(s->param[p] + 0.5f); }

// The module's deja vu pot has a deadband at 12 o'clock (full lock).
float DejaVuAmount(float knob) {
  if (knob < 0.47f) return knob * 1.06382978723f;
  if (knob > 0.53f) return 0.5f + (knob - 0.53f) * 1.06382978723f;
  return 0.5f;
}

float DejaVuFor(const Instance* s, Param mode, float knob) {
  switch (OptionIndex(s, mode)) {
    case 1: return DejaVuAmount(knob);                 // on
    case 2: return 0.5f;                               // locked
    default: return 0.0f;                              // off
  }
}

// Marbles' voltages -> 0..5 V at the jack. The +/-5 V range is squeezed into 0..5 V (still smooth,
// but no longer 1 V/octave); the 0..2 V and 0..5 V ranges pass through unchanged.
float ToJackVolts(float v, int range) {
  if (range == VOLTAGE_RANGE_FULL) v = (v + 5.0f) * 0.5f;
  return Clamp(v, 0.0f, 5.0f);
}

bool MidiOutput(const Instance* s) { return OptionIndex(s, P_OUTPUT) == OUTPUT_MIDI; }

// The jack an output's option asks for; none while the output goes to MIDI.
int JackOption(const Instance* s, int output) {
  return MidiOutput(s) ? 0 : OptionIndex(s, static_cast<Param>(P_OUT_T1 + output));
}

void AllNotesOff(Instance* s) {
  for (int g = 0; g < 3; ++g) {
    if (s->note_on[g] >= 0) midi_out::NoteOff(s->midi, s->note_channel[g], s->note_on[g]);
    s->note_on[g] = -1;
  }
  s->last_cc[0] = s->last_cc[1] = -1;
}

void Connect(Instance* s, int output, int option) {
  int jack = option - 1;  // option 0 = off, 1..8 = CV 1..8
  if (s->jack[output] == jack) {
    if (jack >= 0 && !cv_out::Owns(s, jack)) cv_out::Claim(s, jack);  // re-assigned: take it back
    return;
  }
  int old = s->jack[output];
  s->jack[output] = jack;
  bool still_used = false;
  for (int o = 0; o < NUM_OUTPUTS; ++o) still_used |= s->jack[o] == old;
  if (old >= 0 && !still_used) cv_out::Release(s, old);
  if (jack >= 0) cv_out::Claim(s, jack);
  if (output <= OUT_T3) s->gate_sent[output] = !s->gate_sent[output];  // force a resend
}

// The firmware's objects are globals, zeroed before Init(); some members are never set by Init()
// (e.g. clouds' GranularProcessor::silence_), so instances live in zeroed memory too.
Instance* NewInstance() {
  void* mem = calloc(1, sizeof(Instance));
  return mem ? new (mem) Instance : NULL;
}

void DeleteInstance(Instance* s) {
  s->~Instance();
  free(s);
}

void* Create(const char* data_dir) {
  (void)data_dir;
  InitModParams();
  Instance* s = NewInstance();
  if (!s) return NULL;
  memset(s->gate_level, 0, sizeof s->gate_level);
  memset(s->gate_rose, 0, sizeof s->gate_rose);
  memset(s->gate_sent, 0, sizeof s->gate_sent);
  s->playing = s->was_playing = false;
  s->ppq = -1.0;
  s->bpm = 120.0;
  s->clock_level = false;

  uint32_t seed = static_cast<uint32_t>(time(NULL)) ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s));
  s->modulation.Init(seed ^ 0x5a5a5a5au);
  s->random_generator.Init(seed);
  s->random_stream.Init(&s->random_generator);
  s->t_generator.Init(&s->random_stream, kSampleRate);
  s->xy_generator.Init(&s->random_stream, kSampleRate);
  for (int i = 0; i < 6; ++i) s->xy_generator.LoadScale(i, preset_scales[i]);

  cv_out::AddInstance();
  for (int o = 0; o < NUM_OUTPUTS; ++o) s->jack[o] = -1;
  for (int p = 0; p < P_LAST; ++p) s->param[p] = kParams[p].def;
  for (int o = 0; o < NUM_OUTPUTS; ++o) Connect(s, o, JackOption(s, o));
  s->midi = midi_out::Open();   // always open, so a MIDI track can be set up before switching to MIDI
  for (int g = 0; g < 3; ++g) s->note_on[g] = -1;
  s->last_cc[0] = s->last_cc[1] = -1;
  return s;
}

void Destroy(void* inst) {
  Instance* s = static_cast<Instance*>(inst);
  for (int o = 0; o < NUM_OUTPUTS; ++o) {
    if (s->jack[o] >= 0) cv_out::Release(s, s->jack[o]);
  }
  cv_out::RemoveInstance();
  AllNotesOff(s);
  midi_out::Close(s->midi);
  DeleteInstance(s);
}

enum MidiTrigger { MIDI_ANY, MIDI_LEARN, MIDI_ONE_NOTE };
const int kMinPulse = 44;  // 1 ms, so a note shorter than a sample block still makes a clock edge

void Midi(void* inst, const uint8_t* msg, int len) {
  Instance* s = static_cast<Instance*>(inst);
  if (len < 3) return;
  uint8_t status = msg[0] & 0xf0;
  bool on = status == 0x90 && msg[2] > 0;
  bool off = status == 0x80 || (status == 0x90 && msg[2] == 0);
  if (!on && !off) return;
  if (on) {
    ++s->notes_held;
    s->midi_retrigger = true;
    s->velocity = msg[2] / 127.0f;
  } else if (s->notes_held > 0) {
    --s->notes_held;
  }
  int mode = OptionIndex(s, P_MIDI_TRIGGER);
  if (on && mode == MIDI_LEARN) {
    // The next note becomes the trigger note (press the pad you want).
    s->param[P_MIDI_NOTE] = msg[1];
    s->param[P_MIDI_TRIGGER] = MIDI_ONE_NOTE;
    mode = MIDI_ONE_NOTE;
  }
  if (mode == MIDI_ONE_NOTE && msg[1] != static_cast<int>(s->param[P_MIDI_NOTE] + 0.5f)) return;
  if (on) {
    ++s->midi_held;
    s->midi_pulse = kMinPulse;
  } else if (s->midi_held > 0) {
    --s->midi_held;
  }
}

void SetParam(void* inst, const char* key, const char* val);

// Project save/load: the wrapper stores whatever get_param("state") returns as the plugin chunk.
// Format: "key=value;key=value;..." over every param, so older saves load with new params at default.
int SaveState(const Instance* s, char* buf, int buf_len) {
  int len = 0;
  for (int p = 0; p < P_LAST && len < buf_len; ++p) {
    len += snprintf(buf + len, buf_len - len, "%s=%g;", kParams[p].key, s->param[p]);
  }
  return len < buf_len ? len : buf_len - 1;
}

void LoadState(Instance* s, const char* state) {
  char item[64];
  while (*state) {
    size_t n = strcspn(state, ";");
    if (n < sizeof item) {
      memcpy(item, state, n);
      item[n] = 0;
      char* eq = strchr(item, '=');
      if (eq) {
        *eq = 0;
        if (strcmp(item, "state")) SetParam(s, item, eq + 1);
      }
    }
    state += n;
    if (*state == ';') ++state;
  }
}

void SetParam(void* inst, const char* key, const char* val) {
  Instance* s = static_cast<Instance*>(inst);
  if (!strcmp(key, "state")) {
    LoadState(s, val);
    return;
  }
  if (!strcmp(key, "host_transport")) {
    int playing = 0;
    double ppq = -1.0, bpm = 0.0;
    if (sscanf(val, "%d %lf %lf", &playing, &ppq, &bpm) >= 2) {
      s->playing = playing != 0;
      s->ppq = ppq;
      if (bpm > 0.0) s->bpm = bpm;
    }
    return;
  }
  for (int p = 0; p < P_LAST; ++p) {
    if (strcmp(key, kParams[p].key)) continue;
    bool was_midi = MidiOutput(s);
    s->param[p] = static_cast<float>(atof(val));
    if (p >= P_OUT_T1 && p <= P_OUT_Y) Connect(s, p - P_OUT_T1, JackOption(s, p - P_OUT_T1));
    if (p == P_OUTPUT && MidiOutput(s) != was_midi) {
      AllNotesOff(s);
      for (int o = 0; o < NUM_OUTPUTS; ++o) Connect(s, o, JackOption(s, o));
    }
    return;
  }
}

int GetParam(void* inst, const char* key, char* buf, int buf_len) {
  const Instance* s = static_cast<const Instance*>(inst);
  if (!strcmp(key, "state")) return SaveState(s, buf, buf_len);
  size_t key_len = strlen(key);
  if (key_len > 8 && !strcmp(key + key_len - 8, "_display")) {
    // Readable values for the envelope times and LFO rates (dynamic_display in params.json).
    for (int p = P_MOD_FIRST; p < P_SLOT_FIRST; ++p) {
      if (strlen(kParams[p].key) != key_len - 8 || strncmp(key, kParams[p].key, key_len - 8)) continue;
      float v = Clamp(s->param[p], 0.0f, 1.0f);
      if (strstr(key, "_rate")) {
        int l = (p - P_LFO_FIRST) / 4;
        if (s->param[P_LFO_FIRST + l * 4 + 2] > 0.5f) return snprintf(buf, buf_len, "%s", kDivisionNames[LfoDivisionOf(v)]);
        return snprintf(buf, buf_len, "%.2f Hz", 0.01f * powf(3000.0f, v));
      }
      float sec = 0.001f * powf(10000.0f, v);
      return sec < 1.0f ? snprintf(buf, buf_len, "%d ms", static_cast<int>(sec * 1000.0f + 0.5f))
                        : snprintf(buf, buf_len, "%.1f s", sec);
    }
    return 0;
  }
  for (int p = 0; p < P_LAST; ++p) {
    if (strcmp(key, kParams[p].key)) continue;
    if (kParams[p].option) return snprintf(buf, buf_len, "%d", OptionIndex(s, static_cast<Param>(p)));
    return snprintf(buf, buf_len, "%g", s->param[p]);
  }
  return 0;
}

// Clock pulses from the MPC song position: a 50% duty square at the chosen note value, so the
// T section locks to the transport (and its rate knob then picks a multiplier/divider, as with an
// external clock on the module).
void MakeTransportClock(Instance* s, size_t offset, size_t size) {
  int per_beat = kClockPulsesPerBeat[Clamp(OptionIndex(s, P_CLOCK_DIV), 0, 3)];
  double beats_per_sample = s->bpm / 60.0 / kSampleRate;
  for (size_t i = 0; i < size; ++i) {
    bool level = false;
    if (s->playing && s->ppq >= 0.0) {
      double pulses = (s->ppq + (offset + i) * beats_per_sample) * per_beat;
      level = pulses - floor(pulses) < 0.5;
    }
    s->clock[i] = stmlib::ExtractGateFlags(s->clock_level ? stmlib::GATE_FLAG_HIGH : stmlib::GATE_FLAG_LOW, level);
    s->clock_level = level;
  }
}

// A matrix clock gate, constant over the block: an edge at its start.
void MakeGateClock(GateFlags* flags, bool* level, bool gate, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    flags[i] = stmlib::ExtractGateFlags(*level ? stmlib::GATE_FLAG_HIGH : stmlib::GATE_FLAG_LOW, gate);
    *level = gate;
  }
}

// Clock = MIDI: high while a trigger note is held (at least kMinPulse samples).
void MakeMidiClock(Instance* s, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    bool level = s->midi_held > 0 || s->midi_pulse > 0;
    if (s->midi_pulse > 0) --s->midi_pulse;
    s->clock[i] = stmlib::ExtractGateFlags(s->clock_level ? stmlib::GATE_FLAG_HIGH : stmlib::GATE_FLAG_LOW, level);
    s->clock_level = level;
  }
}

// Marbles' voltage -> -1..1 over the group's range (a modulation source).
float Bipolar(float v, int range) {
  switch (range) {
    case VOLTAGE_RANGE_NARROW: return Clamp(v - 1.0f, -1.0f, 1.0f);
    case VOLTAGE_RANGE_POSITIVE: return Clamp(v * 0.4f - 1.0f, -1.0f, 1.0f);
    default: return Clamp(v * 0.2f, -1.0f, 1.0f);
  }
}

// One step of the envelopes, LFOs and matrix per block.
const mod::Modulation& UpdateModulation(Instance* s) {
  mod::Settings ms;
  for (int e = 0; e < mod::kNumEnvs; ++e) {
    const float* v = &s->param[P_ENV_FIRST + e * 5];
    ms.env[e] = { v[0], v[1], v[2], v[3], static_cast<int>(v[4] + 0.5f) };
  }
  for (int l = 0; l < mod::kNumLfos; ++l) {
    const float* v = &s->param[P_LFO_FIRST + l * 4];
    ms.lfo[l] = { static_cast<int>(v[0] + 0.5f), v[1], v[2] > 0.5f, LfoDivisionOf(v[1]) };
  }
  for (int k = 0; k < mod::kNumSlots; ++k) {
    const float* v = &s->param[P_SLOT_FIRST + k * 3];
    ms.source[k] = static_cast<int>(v[0] + 0.5f);
    ms.dest[k] = static_cast<int>(v[1] + 0.5f);
    ms.amount[k] = v[2] / 100.0f;   // -100..+100 %
  }
  int x_range = Clamp(OptionIndex(s, P_X_RANGE), 0, 2), y_range = Clamp(OptionIndex(s, P_Y_RANGE), 0, 2);
  mod::Inputs in;
  in.midi_gate = s->notes_held > 0;
  in.midi_retrigger = s->midi_retrigger;
  in.velocity = s->velocity;
  in.playing = s->playing;
  in.ppq = s->ppq;
  in.bpm = s->bpm;
  for (int g = 0; g < 3; ++g) in.gate[g] = s->gate_sent[g];
  for (int c = 0; c < 3; ++c) in.x[c] = Bipolar(s->last_volts[c], x_range);
  in.y = Bipolar(s->last_volts[3], y_range);
  s->midi_retrigger = false;
  s->modulation.Process(ms, in, 128.0f / kSampleRate);
  return s->modulation;
}

void ConfigureGenerators(Instance* s, GroupSettings* x, GroupSettings* y, const mod::Modulation& m) {
  TGenerator& t = s->t_generator;
  t.set_model(TGeneratorModel(Clamp(OptionIndex(s, P_T_MODEL), 0, int(T_GENERATOR_MODEL_MARKOV))));
  t.set_range(TGeneratorRange(Clamp(OptionIndex(s, P_T_RANGE), 0, 2)));
  float rate = Clamp(s->param[P_T_RATE] + m.out(mod::DST_T_RATE), 0.0f, 1.0f);
  t.set_rate(rate * 120.0f - 60.0f);  // the module's rate pot spans +/-60 semitones
  t.set_bias(Clamp(s->param[P_T_BIAS] + m.out(mod::DST_T_BIAS), 0.0f, 1.0f));
  t.set_jitter(Clamp(s->param[P_T_JITTER] + m.out(mod::DST_JITTER), 0.0f, 1.0f));
  float deja_vu_knob = Clamp(s->param[P_DEJA_VU] + m.out(mod::DST_DEJA_VU), 0.0f, 1.0f);
  t.set_deja_vu(DejaVuFor(s, P_T_DEJA_VU, deja_vu_knob));
  int length = kLoopLengths[Clamp(OptionIndex(s, P_LENGTH), 0, kNumLoopLengths - 1)];
  t.set_length(length);
  t.set_pulse_width_mean(s->param[P_T_PW]);
  t.set_pulse_width_std(s->param[P_T_PW_RANDOM]);

  x->control_mode = ControlMode(Clamp(OptionIndex(s, P_X_MODE), 0, 2));
  x->voltage_range = VoltageRange(Clamp(OptionIndex(s, P_X_RANGE), 0, 2));
  x->register_mode = false;
  x->register_value = 0.0f;
  x->spread = Clamp(s->param[P_X_SPREAD] + m.out(mod::DST_SPREAD), 0.0f, 1.0f);
  x->bias = Clamp(s->param[P_X_BIAS] + m.out(mod::DST_X_BIAS), 0.0f, 1.0f);
  x->steps = Clamp(s->param[P_X_STEPS] + m.out(mod::DST_STEPS), 0.0f, 1.0f);
  x->deja_vu = DejaVuFor(s, P_X_DEJA_VU, deja_vu_knob);
  x->length = length;
  x->ratio.p = 1;
  x->ratio.q = 1;
  x->scale_index = Clamp(OptionIndex(s, P_X_SCALE), 0, 5);

  y->control_mode = CONTROL_MODE_IDENTICAL;
  y->voltage_range = VoltageRange(Clamp(OptionIndex(s, P_Y_RANGE), 0, 2));
  y->register_mode = false;
  y->register_value = 0.0f;
  y->spread = s->param[P_Y_SPREAD];
  y->bias = s->param[P_Y_BIAS];
  y->steps = s->param[P_Y_STEPS];
  y->deja_vu = 0.0f;
  y->length = 1;
  y->ratio = kYDividers[Clamp(OptionIndex(s, P_Y_DIVIDER), 0, kNumYDividers - 1)];
  y->scale_index = x->scale_index;
}

// Marbles' voltage -> 0..1 over the group's range (for the Y CC).
float Normalized(float v, int range) {
  switch (range) {
    case VOLTAGE_RANGE_NARROW: return Clamp(v * 0.5f, 0.0f, 1.0f);
    case VOLTAGE_RANGE_POSITIVE: return Clamp(v * 0.2f, 0.0f, 1.0f);
    default: return Clamp((v + 5.0f) * 0.1f, 0.0f, 1.0f);
  }
}

// Output = MIDI: each voice plays a note while its gate (T1..T3) is open, the pitch taken from its source when the
// gate opens: X1..X3 or Y at 1 V/octave as semitones above the voice's note (Marbles quantizes X to the scale, so
// notes stay in key), or the note itself (Fixed: drums). Each CC lane sends X1..X3 or Y over its range, 0..127.
void WriteMidi(Instance* s, const GroupSettings& x, const GroupSettings& y) {
  const float* last = &s->voltages[(kSubBlock - 1) * 4];
  bool rose[3], fell[3];
  for (int g = 0; g < 3; ++g) {
    bool out = s->gate_level[g] || (s->gate_rose[g] && !s->gate_sent[g]);
    s->gate_rose[g] = false;
    rose[g] = out && !s->gate_sent[g];
    fell[g] = !out && s->gate_sent[g];
    s->gate_sent[g] = out;
  }
  for (int v = 0; v < 3; ++v) {
    const float* p = &s->param[P_VOICE_FIRST + v * 4];
    int g = static_cast<int>(p[0] + 0.5f) - 1;
    if (g < 0 || g > 2) {
      if (s->note_on[v] >= 0) midi_out::NoteOff(s->midi, s->note_channel[v], s->note_on[v]);
      s->note_on[v] = -1;
      continue;
    }
    if (rose[g]) {
      if (s->note_on[v] >= 0) midi_out::NoteOff(s->midi, s->note_channel[v], s->note_on[v]);
      int pitch = Clamp(static_cast<int>(p[1] + 0.5f), 0, int(PITCH_FIXED));
      int note = static_cast<int>(p[3] + 0.5f);
      if (pitch != PITCH_FIXED) note += static_cast<int>(floorf(last[pitch] * 12.0f + 0.5f));
      note = Clamp(note, 0, 127);
      int channel = Clamp(static_cast<int>(p[2] + 0.5f), 0, 15);
      midi_out::NoteOn(s->midi, channel, note, kVelocity);
      s->note_on[v] = note;
      s->note_channel[v] = channel;
    } else if (fell[g] && s->note_on[v] >= 0) {
      midi_out::NoteOff(s->midi, s->note_channel[v], s->note_on[v]);
      s->note_on[v] = -1;
    }
  }
  for (int c = 0; c < 2; ++c) {
    const float* p = &s->param[P_CC_FIRST + c * 3];
    int src = Clamp(static_cast<int>(p[0] + 0.5f), 0, int(CC_Y));
    if (src == CC_OFF) {
      s->last_cc[c] = -1;
      continue;
    }
    int range = src == CC_Y ? y.voltage_range : x.voltage_range;
    int value = static_cast<int>(Normalized(last[src - CC_X1], range) * 127.0f + 0.5f);
    if (value != s->last_cc[c]) {
      midi_out::Control(s->midi, Clamp(static_cast<int>(p[1] + 0.5f), 0, 15), static_cast<int>(p[2] + 0.5f), value);
      s->last_cc[c] = value;
    }
  }
}

void WriteOutputs(Instance* s, const GroupSettings& x, const GroupSettings& y) {
  if (MidiOutput(s)) {
    WriteMidi(s, x, y);
    return;
  }
  const Output gate_outputs[3] = { OUT_T1, OUT_T2, OUT_T3 };
  for (int g = 0; g < 3; ++g) {
    // Hold a pulse that started and ended inside this block for one block, so it isn't lost.
    bool out = s->gate_level[g] || (s->gate_rose[g] && !s->gate_sent[g]);
    s->gate_rose[g] = false;
    int jack = s->jack[gate_outputs[g]];
    if (jack >= 0 && out != s->gate_sent[g]) cv_out::Set(s, jack, out ? cv_out::kMaxValue : 0);
    s->gate_sent[g] = out;
  }
  // X1-X3 and Y at the end of the block (voltages are interleaved X1, X2, X3, Y per sample).
  const float* last = &s->voltages[(kSubBlock - 1) * 4];
  for (int c = 0; c < 4; ++c) {
    int jack = s->jack[OUT_X1 + c];
    if (jack < 0) continue;
    int range = c < 3 ? x.voltage_range : y.voltage_range;
    cv_out::Set(s, jack, cv_out::VoltsToCode(ToJackVolts(last[c], range)));
  }
  cv_out::Flush();
}

void Render(void* inst, int16_t* out_lr, int frames) {
  Instance* s = static_cast<Instance*>(inst);
  memset(out_lr, 0, sizeof(int16_t) * 2 * frames);

  GroupSettings x, y;
  const mod::Modulation& m = UpdateModulation(s);
  ConfigureGenerators(s, &x, &y, m);

  enum { CLOCK_INTERNAL, CLOCK_MPC, CLOCK_MIDI };
  int clock = OptionIndex(s, P_CLOCK);
  // A matrix slot on T Clock / X Clock is a cable in the module's CLOCK jack: it takes over that section's clock.
  bool t_mod_clock = m.used(mod::DST_T_CLOCK), x_mod_clock = m.used(mod::DST_X_CLOCK);
  bool t_gate = m.out(mod::DST_T_CLOCK) > 0.5f, x_gate = m.out(mod::DST_X_CLOCK) > 0.5f;
  bool external = t_mod_clock || clock == CLOCK_MPC || clock == CLOCK_MIDI;
  // Restart the patterns when the transport starts, so deja vu loops line up with the song.
  bool reset = clock == CLOCK_MPC && s->playing && !s->was_playing;
  s->was_playing = s->playing;

  Ramps ramps;
  ramps.master = &s->ramp_buffer[0];
  ramps.external = &s->ramp_buffer[kSubBlock];
  ramps.slave[0] = &s->ramp_buffer[kSubBlock * 2];
  ramps.slave[1] = &s->ramp_buffer[kSubBlock * 3];
  ClockSource xy_source = x_mod_clock ? CLOCK_SOURCE_EXTERNAL : ClockSource(Clamp(OptionIndex(s, P_X_CLOCK), 0, 3));

  for (size_t offset = 0; offset + kSubBlock <= static_cast<size_t>(frames); offset += kSubBlock) {
    if (t_mod_clock) {
      MakeGateClock(s->clock, &s->t_clock_level, t_gate, kSubBlock);
    } else if (clock == CLOCK_MIDI) {
      MakeMidiClock(s, kSubBlock);
    } else {
      MakeTransportClock(s, offset, kSubBlock);
    }
    if (x_mod_clock) MakeGateClock(s->x_clock, &s->x_clock_level, x_gate, kSubBlock);
    bool t_reset = reset;
    bool x_reset = reset;
    reset = false;
    s->t_generator.Process(external, &t_reset, s->clock, ramps, s->gates, kSubBlock);
    s->xy_generator.Process(xy_source, x, y, &x_reset, x_mod_clock ? s->x_clock : s->clock, ramps, s->voltages,
                            kSubBlock);
    for (size_t i = 0; i < kSubBlock; ++i) {
      bool level[3] = { s->gates[i * 2], ramps.master[i] < 0.5f, s->gates[i * 2 + 1] };
      for (int g = 0; g < 3; ++g) {
        s->gate_rose[g] |= level[g] && !s->gate_level[g];
        s->gate_level[g] = level[g];
      }
    }
  }
  WriteOutputs(s, x, y);
  memcpy(s->last_volts, &s->voltages[(kSubBlock - 1) * 4], sizeof s->last_volts);
}

const mpc_engine_t kEngine = { Create, Destroy, Midi, SetParam, GetParam, Render, NULL };

}  // namespace

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }
