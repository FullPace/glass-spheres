#include "midi_out.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace midi_out {

namespace {

// The parts of the ALSA sequencer ABI we use (alsa/seq_event.h, alsa/seq.h).
typedef struct _snd_seq snd_seq_t;
struct SeqEvent {
  unsigned char type, flags, tag, queue;
  unsigned int time[2];
  unsigned char source_client, source_port, dest_client, dest_port;
  union {
    struct { unsigned char channel, note, velocity, off_velocity; unsigned int duration; } note;
    struct { unsigned char channel, unused[3]; unsigned int param; int value; } control;
    unsigned char raw[12];
  } data;
};
const int kSeqOpenOutput = 1;
const int kSeqNonblock = 1;
const unsigned kCapRead = 1 << 0, kCapSubsRead = 1 << 5;   // SND_SEQ_PORT_CAP_READ, _SUBS_READ
const unsigned kTypeMidiGeneric = 1 << 1, kTypeApplication = 1 << 20;
const unsigned char kEventNoteOn = 6, kEventNoteOff = 7, kEventController = 10;
const unsigned char kQueueDirect = 253, kAddressSubscribers = 254, kAddressUnknown = 253;

struct Api {
  bool loaded;
  int (*open)(snd_seq_t**, const char*, int, int);
  int (*close)(snd_seq_t*);
  int (*set_client_name)(snd_seq_t*, const char*);
  int (*create_simple_port)(snd_seq_t*, const char*, unsigned, unsigned);
  int (*event_output_direct)(snd_seq_t*, SeqEvent*);
};

Api api;
int next_number = 1;

bool LoadApi() {
  if (api.loaded) return true;
  void* lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return false;
  api.open = reinterpret_cast<int (*)(snd_seq_t**, const char*, int, int)>(dlsym(lib, "snd_seq_open"));
  api.close = reinterpret_cast<int (*)(snd_seq_t*)>(dlsym(lib, "snd_seq_close"));
  api.set_client_name = reinterpret_cast<int (*)(snd_seq_t*, const char*)>(dlsym(lib, "snd_seq_set_client_name"));
  api.create_simple_port =
      reinterpret_cast<int (*)(snd_seq_t*, const char*, unsigned, unsigned)>(dlsym(lib, "snd_seq_create_simple_port"));
  api.event_output_direct =
      reinterpret_cast<int (*)(snd_seq_t*, SeqEvent*)>(dlsym(lib, "snd_seq_event_output_direct"));
  api.loaded = api.open && api.close && api.set_client_name && api.create_simple_port && api.event_output_direct;
  return api.loaded;
}

}  // namespace

struct Port {
  snd_seq_t* seq;
  int port;
};

Port* Open() {
  if (!LoadApi()) return NULL;
  snd_seq_t* seq = NULL;
  if (api.open(&seq, "default", kSeqOpenOutput, kSeqNonblock) < 0) return NULL;
  char name[32];
  snprintf(name, sizeof name, "Glass Spheres %d", next_number++);
  api.set_client_name(seq, name);
  int port_id = api.create_simple_port(seq, "MIDI Out", kCapRead | kCapSubsRead, kTypeMidiGeneric | kTypeApplication);
  if (port_id < 0) {
    api.close(seq);
    return NULL;
  }
  Port* port = static_cast<Port*>(calloc(1, sizeof(Port)));
  if (!port) {
    api.close(seq);
    return NULL;
  }
  port->seq = seq;
  port->port = port_id;
  return port;
}

void Close(Port* port) {
  if (!port) return;
  api.close(port->seq);
  free(port);
}

namespace {

void Send(Port* port, SeqEvent* ev) {
  ev->flags = 0;   // tick time stamp, fixed length; direct, so no time is used
  ev->queue = kQueueDirect;
  ev->source_port = static_cast<unsigned char>(port->port);
  ev->dest_client = kAddressSubscribers;
  ev->dest_port = kAddressUnknown;
  api.event_output_direct(port->seq, ev);   // non-blocking: a full output pool drops the event
}

}  // namespace

void NoteOn(Port* port, int channel, int note, int velocity) {
  if (!port) return;
  SeqEvent ev;
  memset(&ev, 0, sizeof ev);
  ev.type = kEventNoteOn;
  ev.data.note.channel = static_cast<unsigned char>(channel & 0x0f);
  ev.data.note.note = static_cast<unsigned char>(note & 0x7f);
  ev.data.note.velocity = static_cast<unsigned char>(velocity & 0x7f);
  Send(port, &ev);
}

void NoteOff(Port* port, int channel, int note) {
  if (!port) return;
  SeqEvent ev;
  memset(&ev, 0, sizeof ev);
  ev.type = kEventNoteOff;
  ev.data.note.channel = static_cast<unsigned char>(channel & 0x0f);
  ev.data.note.note = static_cast<unsigned char>(note & 0x7f);
  Send(port, &ev);
}

void Control(Port* port, int channel, int cc, int value) {
  if (!port) return;
  SeqEvent ev;
  memset(&ev, 0, sizeof ev);
  ev.type = kEventController;
  ev.data.control.channel = static_cast<unsigned char>(channel & 0x0f);
  ev.data.control.param = static_cast<unsigned int>(cc & 0x7f);
  ev.data.control.value = value & 0x7f;
  Send(port, &ev);
}

}  // namespace midi_out
