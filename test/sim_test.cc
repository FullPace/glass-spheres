// Offline check of what the engine sends to the jacks: runs the real engine with a recording
// stand-in for cv_out and prints gate/voltage statistics per jack.
//   make -C test   (see test/Makefile)

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/cv_out.h"

extern "C" {
#include "engine.h"
}

// ---- recording cv_out ----
namespace {
struct JackLog {
  const void* owner;
  int writes, rises;
  uint16_t last, min, max;
};
JackLog jacks[cv_out::kNumJacks];
}

namespace cv_out {
void AddInstance() { }
void RemoveInstance() { }
void Claim(const void* who, int jack) { jacks[jack].owner = who; }
void Release(const void* who, int jack) { if (jacks[jack].owner == who) jacks[jack].owner = NULL; }
bool Owns(const void* who, int jack) { return jack >= 0 && jack < kNumJacks && jacks[jack].owner == who; }
void Set(const void* who, int jack, uint16_t value) {
  if (!Owns(who, jack)) return;
  JackLog& j = jacks[jack];
  if (j.writes && value == j.last) return;
  if (!j.writes) j.min = j.max = value;
  if (value > j.max) j.max = value;
  if (value < j.min) j.min = value;
  if (value == kMaxValue && j.last == 0) j.rises++;
  j.last = value;
  j.writes++;
}
void Flush() { }
}  // namespace cv_out

namespace {

const int kBlock = 128;
const float kSampleRate = 44100.0f;

void Reset() { for (auto& j : jacks) { const void* o = j.owner; memset(&j, 0, sizeof j); j.owner = o; } }

void Report(const char* title, float seconds) {
  printf("%s\n", title);
  for (int i = 0; i < cv_out::kNumJacks; ++i) {
    const JackLog& j = jacks[i];
    if (!j.owner) continue;
    printf("  CV %d: %5d writes, %4d gate rises (%.2f/s), range %.3f..%.3f V\n", i + 1, j.writes, j.rises,
           j.rises / seconds, j.min / cv_out::kCodesPerVolt, j.max / cv_out::kCodesPerVolt);
  }
}

void Run(const mpc_engine_t* e, void* inst, float seconds, bool playing, double bpm) {
  int16_t out[kBlock * 2];
  double ppq = 0.0;
  int blocks = static_cast<int>(seconds * kSampleRate / kBlock);
  for (int b = 0; b < blocks; ++b) {
    char transport[64];
    snprintf(transport, sizeof transport, "%d %.6f %.4f", playing ? 1 : 0, ppq, bpm);
    e->set_param(inst, "host_transport", transport);
    e->render(inst, out, kBlock);
    for (int i = 0; i < kBlock * 2; ++i) {
      if (out[i]) { printf("FAIL: audio output not silent\n"); return; }
    }
    if (playing) ppq += bpm / 60.0 * kBlock / kSampleRate;
  }
}

}  // namespace

int main() {
  const mpc_engine_t* e = mpc_engine();
  void* a = e->create(NULL);

  // Defaults: internal clock, t1 -> CV 2, X1 -> CV 1.
  Run(e, a, 10.0f, false, 120.0);
  Report("internal clock, defaults (10 s)", 10.0f);

  // All outputs on, clocked from the MPC at 120 BPM in 1/16 notes (8 pulses/s).
  const char* outs[][2] = { { "out_t1", "1" }, { "out_t2", "2" }, { "out_t3", "3" }, { "out_x1", "4" },
                            { "out_x2", "5" }, { "out_x3", "6" }, { "out_y", "7" } };
  for (auto& o : outs) e->set_param(a, o[0], o[1]);
  e->set_param(a, "clock", "1");
  Reset();
  Run(e, a, 10.0f, true, 120.0);
  Report("MPC clock 120 BPM 1/16, all outputs (CV1 t1, CV2 t2, CV3 t3, CV4-6 X1-3, CV7 Y)", 10.0f);

  Reset();
  Run(e, a, 2.0f, false, 120.0);
  Report("MPC clock, transport stopped (2 s): gates should stay quiet", 2.0f);

  // A second instance takes over CV 1; the first one's writes must stop landing there.
  void* b = e->create(NULL);
  e->set_param(b, "out_x1", "0");
  e->set_param(b, "out_t1", "1");
  printf("instance b took CV 1: owner is b = %s\n", cv_out::Owns(b, 0) ? "yes" : "NO");
  e->destroy(b);
  e->destroy(a);
  printf("done\n");
  return 0;
}
