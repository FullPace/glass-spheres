#include "cv_out.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

namespace cv_out {

namespace {

// Kernel ABI (sound/asound.h), declared here so the build needs no ALSA headers.
struct RawmidiInfo {
  unsigned device, subdevice;
  int stream, card;
  unsigned flags;
  unsigned char id[64], name[80], subname[32];
  unsigned subdevices_count, subdevices_avail;
  unsigned char reserved[64];
};
const unsigned long kRawmidiIoctlInfo =
    (2ul << 30) | (static_cast<unsigned long>(sizeof(RawmidiInfo)) << 16) | ('W' << 8) | 0x01;
const int kStreamOutput = 0;

// Shortest gap between two writes to the controller. Values that change faster are coalesced, so
// this is also the highest update rate per jack.
const long kMinWriteIntervalNs = 2000000;  // 2 ms
const long kFdRetryNs = 1000000000;        // look for the port again at most once a second

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int instances = 0;
const void* owner[kNumJacks];

// Written by the audio threads, read by the sender.
volatile uint16_t wanted[kNumJacks];
volatile uint32_t dirty;  // bit per jack, accessed with __atomic builtins

pthread_t sender;
bool sender_running = false;
volatile bool sender_quit = false;
// The audio thread signals without taking wake_mutex (it must not block), so a wakeup can slip past
// the sender just before it waits; the timed wait bounds that case.
pthread_mutex_t wake_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
const long kMaxSleepNs = 10000000;  // 10 ms

int64_t NowNs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<int64_t>(t.tv_sec) * 1000000000 + t.tv_nsec;
}

// The app opens the controller's rawmidi device once per substream; ours is the output whose
// subdevice is called "MPC Private".
int FindPrivatePort() {
  DIR* d = opendir("/proc/self/fd");
  if (!d) return -1;
  int found = -1;
  while (dirent* e = readdir(d)) {
    int fd = atoi(e->d_name);
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    char path[64], target[128];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, target, sizeof target - 1);
    if (n <= 0) continue;
    target[n] = 0;
    if (strncmp(target, "/dev/snd/midiC", 14)) continue;
    RawmidiInfo info;
    memset(&info, 0, sizeof info);
    if (ioctl(fd, kRawmidiIoctlInfo, &info) < 0) continue;
    if (info.stream == kStreamOutput && strstr(reinterpret_cast<char*>(info.subname), "Private")) {
      found = fd;
      break;
    }
  }
  closedir(d);
  return found;
}

void WaitForWork() {
  pthread_mutex_lock(&wake_mutex);
  if (!sender_quit && !__atomic_load_n(&dirty, __ATOMIC_ACQUIRE)) {
    timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += kMaxSleepNs;
    if (until.tv_nsec >= 1000000000) {
      until.tv_sec += 1;
      until.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&wake, &wake_mutex, &until);
  }
  pthread_mutex_unlock(&wake_mutex);
}

void* SenderLoop(void*) {
  int fd = -1;
  int64_t next_lookup = 0;
  while (true) {
    WaitForWork();
    if (sender_quit) break;
    if (!__atomic_load_n(&dirty, __ATOMIC_ACQUIRE)) continue;
    if (fd < 0) {
      int64_t now = NowNs();
      if (now < next_lookup) continue;  // values stay queued until the port turns up
      fd = FindPrivatePort();
      if (fd < 0) {
        next_lookup = now + kFdRetryNs;
        continue;
      }
    }
    uint32_t mask = __atomic_exchange_n(&dirty, 0u, __ATOMIC_ACQ_REL);
    unsigned char msg[kNumJacks * 6];
    int len = 0;
    for (int j = 0; j < kNumJacks; ++j) {
      if (!(mask & (1u << j))) continue;
      uint16_t v = wanted[j];
      msg[len++] = 0xbf; msg[len++] = static_cast<unsigned char>(2 * j);     msg[len++] = v >> 7;
      msg[len++] = 0xbf; msg[len++] = static_cast<unsigned char>(2 * j + 1); msg[len++] = v & 0x7f;
    }
    if (write(fd, msg, len) < 0 && errno == EBADF) {
      fd = -1;  // the app reopened the device (e.g. after a USB reset)
    }
    timespec gap = { 0, kMinWriteIntervalNs };
    nanosleep(&gap, NULL);
  }
  return NULL;
}

void Wake() {
  pthread_cond_signal(&wake);
}

}  // namespace

void AddInstance() {
  pthread_mutex_lock(&mutex);
  if (instances++ == 0) {
    sender_quit = false;
    sender_running = pthread_create(&sender, NULL, SenderLoop, NULL) == 0;
  }
  pthread_mutex_unlock(&mutex);
}

void RemoveInstance() {
  pthread_mutex_lock(&mutex);
  if (--instances == 0 && sender_running) {
    // Let the sender flush the zeroed jacks of the last instance, then stop it.
    Wake();
    timespec settle = { 0, 3 * kMinWriteIntervalNs };
    nanosleep(&settle, NULL);
    pthread_mutex_lock(&wake_mutex);
    sender_quit = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&wake_mutex);
    pthread_join(sender, NULL);
    sender_running = false;
  }
  pthread_mutex_unlock(&mutex);
}

void Claim(const void* who, int jack) {
  if (jack < 0 || jack >= kNumJacks) return;
  pthread_mutex_lock(&mutex);
  owner[jack] = who;
  wanted[jack] = 0xffff;  // not a valid code: the new owner's first Set() always goes out
  pthread_mutex_unlock(&mutex);
}

void Release(const void* who, int jack) {
  if (jack < 0 || jack >= kNumJacks) return;
  pthread_mutex_lock(&mutex);
  if (owner[jack] == who) {
    owner[jack] = NULL;
    wanted[jack] = 0;
    __atomic_or_fetch(&dirty, 1u << jack, __ATOMIC_RELEASE);
    if (sender_running) Wake();
  }
  pthread_mutex_unlock(&mutex);
}

bool Owns(const void* who, int jack) {
  return jack >= 0 && jack < kNumJacks && owner[jack] == who;
}

void Set(const void* who, int jack, uint16_t value) {
  if (!Owns(who, jack) || wanted[jack] == value) return;
  wanted[jack] = value;
  __atomic_or_fetch(&dirty, 1u << jack, __ATOMIC_RELEASE);
}

void Flush() {
  if (sender_running && __atomic_load_n(&dirty, __ATOMIC_ACQUIRE)) Wake();
}

}  // namespace cv_out
