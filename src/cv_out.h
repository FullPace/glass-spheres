// CV/Gate jack output for the MPC X, from inside the MPC process.
//
// The app drives the jacks with CC pairs on MIDI channel 16 to the controller's "MPC Private" rawmidi
// port: jack n (0-based) = CC 2n (MSB) + CC 2n+1 (LSB), 14 bit over 0..5 V, 1 V/octave
// (see ../cv/README.md). This module finds the app's already-open fd for that port and writes the
// same messages from a sender thread, so the audio thread never blocks on the device.
//
// Jacks are shared by every plugin instance in the process: an instance claims a jack before setting
// it, and the latest claim wins (the previous owner's writes are then ignored).
#pragma once

#include <stdint.h>

namespace cv_out {

const int kNumJacks = 8;
const uint16_t kMaxValue = 16383;
const float kCodesPerVolt = 3276.8f;

// Call from create()/destroy(): the sender thread runs while at least one instance exists, and is
// joined when the last one goes, so MPC can unload the .so safely.
void AddInstance();
void RemoveInstance();

void Claim(const void* owner, int jack);      // jack 0..7
void Release(const void* owner, int jack);    // sets the jack to 0 V if owner still holds it
bool Owns(const void* owner, int jack);

// Audio thread: queue a new value (sent only if it changed). Flush() wakes the sender once per block.
void Set(const void* owner, int jack, uint16_t value);
void Flush();

inline uint16_t VoltsToCode(float volts) {
  float code = volts * kCodesPerVolt + 0.5f;
  if (code < 0.0f) return 0;
  if (code > kMaxValue) return kMaxValue;
  return static_cast<uint16_t>(code);
}

}  // namespace cv_out
