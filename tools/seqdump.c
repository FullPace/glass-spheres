/* seqdump: print the events of an ALSA sequencer port, for checking Glass Spheres' MIDI output on the device.
 *   seqdump <client>:<port> [seconds]      (BusyBox has no aseqdump; libasound is loaded at run time)
 * Build: zig cc -target arm-linux-gnueabihf.2.27 -O2 -o seqdump seqdump.c -ldl */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

typedef struct _snd_seq snd_seq_t;
typedef struct {
    unsigned char type, flags, tag, queue;
    unsigned int time[2];
    unsigned char sc, sp, dc, dp;
    union {
        struct { unsigned char channel, note, velocity, off_velocity; unsigned int duration; } note;
        struct { unsigned char channel, unused[3]; unsigned int param; int value; } control;
    } data;
} ev_t;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: seqdump client:port [seconds]\n"); return 1; }
    int client = atoi(argv[1]), port = atoi(argv[1] + strcspn(argv[1], ":") + 1), secs = argc > 2 ? atoi(argv[2]) : 10;
    void *l = dlopen("libasound.so.2", RTLD_NOW);
    if (!l) { fprintf(stderr, "no libasound\n"); return 1; }
    int (*open_)(snd_seq_t **, const char *, int, int) = dlsym(l, "snd_seq_open");
    int (*port_)(snd_seq_t *, const char *, unsigned, unsigned) = dlsym(l, "snd_seq_create_simple_port");
    int (*conn)(snd_seq_t *, int, int, int) = dlsym(l, "snd_seq_connect_from");
    int (*input)(snd_seq_t *, ev_t **) = dlsym(l, "snd_seq_event_input");
    snd_seq_t *seq;
    if (open_(&seq, "default", 2, 1) < 0) return 1;   /* input, non-blocking */
    int me = port_(seq, "dump", (1 << 1) | (1 << 6), 1 << 1);
    if (conn(seq, me, client, port) < 0) { fprintf(stderr, "connect failed\n"); return 1; }
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        ev_t *e = NULL;
        if (input(seq, &e) < 0 || !e) { usleep(1000); continue; }
        if (e->type == 6 || e->type == 7)
            printf("%s ch%d note %d vel %d\n", e->type == 6 ? "ON " : "OFF", e->data.note.channel + 1, e->data.note.note, e->data.note.velocity);
        else if (e->type == 10)
            printf("CC  ch%d cc %u val %d\n", e->data.control.channel + 1, e->data.control.param, e->data.control.value);
        else
            printf("type %d\n", e->type);
        fflush(stdout);
    }
    return 0;
}
