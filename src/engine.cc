// Marbles for MPC OS: Mutable Instruments Marbles' random generators as an MPC plugin whose outputs
// drive the MPC X's CV/Gate jacks.
//
// Replaces the firmware main loop (marbles/marbles.cc): parameters come from plugin params instead of
// pots/CV, the T section clocks from its own rate or from the MPC transport, and the seven outputs
// (t1-t3, X1-X3, Y) go to any of the eight jacks through cv_out. The plugin renders silence.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <new>

#include "cv_out.h"

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
  P_LAST
};

struct ParamInfo {
  const char* key;
  float def;
  bool option;  // an option index, reported as an integer
};

// Keys and defaults; params.json has the same list (names, ranges, option labels) in VST order.
// Defaults follow the module's factory state, except the X/Y range (0-5 V: the jacks can't go
// negative) and the clock (internal, so it runs without the transport).
const ParamInfo kParams[P_LAST] = {
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
};

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

  // Gates seen during the current block, so pulses shorter than a block still reach the jack.
  bool gate_level[3];
  bool gate_rose[3];
  bool gate_sent[3];

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

float DejaVuFor(const Instance* s, Param mode) {
  switch (OptionIndex(s, mode)) {
    case 1: return DejaVuAmount(s->param[P_DEJA_VU]);  // on
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
  s->random_generator.Init(seed);
  s->random_stream.Init(&s->random_generator);
  s->t_generator.Init(&s->random_stream, kSampleRate);
  s->xy_generator.Init(&s->random_stream, kSampleRate);
  for (int i = 0; i < 6; ++i) s->xy_generator.LoadScale(i, preset_scales[i]);

  cv_out::AddInstance();
  for (int o = 0; o < NUM_OUTPUTS; ++o) s->jack[o] = -1;
  for (int p = 0; p < P_LAST; ++p) {
    s->param[p] = kParams[p].def;
    if (p >= P_OUT_T1) Connect(s, p - P_OUT_T1, static_cast<int>(kParams[p].def));
  }
  return s;
}

void Destroy(void* inst) {
  Instance* s = static_cast<Instance*>(inst);
  for (int o = 0; o < NUM_OUTPUTS; ++o) {
    if (s->jack[o] >= 0) cv_out::Release(s, s->jack[o]);
  }
  cv_out::RemoveInstance();
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
    s->param[p] = static_cast<float>(atof(val));
    if (p >= P_OUT_T1) Connect(s, p - P_OUT_T1, OptionIndex(s, static_cast<Param>(p)));
    return;
  }
}

int GetParam(void* inst, const char* key, char* buf, int buf_len) {
  const Instance* s = static_cast<const Instance*>(inst);
  if (!strcmp(key, "state")) return SaveState(s, buf, buf_len);
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

// Clock = MIDI: high while a trigger note is held (at least kMinPulse samples).
void MakeMidiClock(Instance* s, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    bool level = s->midi_held > 0 || s->midi_pulse > 0;
    if (s->midi_pulse > 0) --s->midi_pulse;
    s->clock[i] = stmlib::ExtractGateFlags(s->clock_level ? stmlib::GATE_FLAG_HIGH : stmlib::GATE_FLAG_LOW, level);
    s->clock_level = level;
  }
}

void ConfigureGenerators(Instance* s, GroupSettings* x, GroupSettings* y) {
  TGenerator& t = s->t_generator;
  t.set_model(TGeneratorModel(Clamp(OptionIndex(s, P_T_MODEL), 0, int(T_GENERATOR_MODEL_MARKOV))));
  t.set_range(TGeneratorRange(Clamp(OptionIndex(s, P_T_RANGE), 0, 2)));
  t.set_rate(s->param[P_T_RATE] * 120.0f - 60.0f);  // the module's rate pot spans +/-60 semitones
  t.set_bias(s->param[P_T_BIAS]);
  t.set_jitter(s->param[P_T_JITTER]);
  t.set_deja_vu(DejaVuFor(s, P_T_DEJA_VU));
  int length = kLoopLengths[Clamp(OptionIndex(s, P_LENGTH), 0, kNumLoopLengths - 1)];
  t.set_length(length);
  t.set_pulse_width_mean(s->param[P_T_PW]);
  t.set_pulse_width_std(s->param[P_T_PW_RANDOM]);

  x->control_mode = ControlMode(Clamp(OptionIndex(s, P_X_MODE), 0, 2));
  x->voltage_range = VoltageRange(Clamp(OptionIndex(s, P_X_RANGE), 0, 2));
  x->register_mode = false;
  x->register_value = 0.0f;
  x->spread = s->param[P_X_SPREAD];
  x->bias = s->param[P_X_BIAS];
  x->steps = s->param[P_X_STEPS];
  x->deja_vu = DejaVuFor(s, P_X_DEJA_VU);
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

void WriteOutputs(Instance* s, const GroupSettings& x, const GroupSettings& y) {
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
  ConfigureGenerators(s, &x, &y);

  enum { CLOCK_INTERNAL, CLOCK_MPC, CLOCK_MIDI };
  int clock = OptionIndex(s, P_CLOCK);
  bool external = clock == CLOCK_MPC || clock == CLOCK_MIDI;
  // Restart the patterns when the transport starts, so deja vu loops line up with the song.
  bool reset = clock == CLOCK_MPC && s->playing && !s->was_playing;
  s->was_playing = s->playing;

  Ramps ramps;
  ramps.master = &s->ramp_buffer[0];
  ramps.external = &s->ramp_buffer[kSubBlock];
  ramps.slave[0] = &s->ramp_buffer[kSubBlock * 2];
  ramps.slave[1] = &s->ramp_buffer[kSubBlock * 3];
  ClockSource xy_source = ClockSource(Clamp(OptionIndex(s, P_X_CLOCK), 0, 3));

  for (size_t offset = 0; offset + kSubBlock <= static_cast<size_t>(frames); offset += kSubBlock) {
    if (clock == CLOCK_MIDI) {
      MakeMidiClock(s, kSubBlock);
    } else {
      MakeTransportClock(s, offset, kSubBlock);
    }
    bool t_reset = reset;
    bool x_reset = reset;
    reset = false;
    s->t_generator.Process(external, &t_reset, s->clock, ramps, s->gates, kSubBlock);
    s->xy_generator.Process(xy_source, x, y, &x_reset, s->clock, ramps, s->voltages, kSubBlock);
    for (size_t i = 0; i < kSubBlock; ++i) {
      bool level[3] = { s->gates[i * 2], ramps.master[i] < 0.5f, s->gates[i * 2 + 1] };
      for (int g = 0; g < 3; ++g) {
        s->gate_rose[g] |= level[g] && !s->gate_level[g];
        s->gate_level[g] = level[g];
      }
    }
  }
  WriteOutputs(s, x, y);
}

const mpc_engine_t kEngine = { Create, Destroy, Midi, SetParam, GetParam, Render, NULL };

}  // namespace

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }
