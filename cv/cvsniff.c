/* cvsniff.so - LD_PRELOAD logger to find out how the MPC app drives the CV/Gate outputs.
 *
 * Logs, from inside the MPC process only:
 *   W  raw write() to device nodes (/dev/snd/midi*, /dev/snd/control*, /dev/tty*, /dev/bus/usb, /dev/hidraw*, ...)
 *   C  ALSA control element writes (SNDRV_CTL_IOCTL_ELEM_WRITE / TLV_WRITE) via ioctl()
 *   U  USB control/bulk transfers (USBDEVFS ioctls) on /dev/bus/usb
 *   S  ALSA sequencer events sent with snd_seq_event_output*()
 * Also: a FIFO /tmp/cvinject whose hex bytes are written to the controller (see injector below).
 * Output: $CVSNIFF_LOG (default /tmp/cvsniff.log), one line per event, ms timestamps.
 *
 * Build: zig cc -target arm-linux-gnueabihf.2.31 -shared -fPIC -O2 -o cvsniff.so cvsniff.c
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

typedef struct _snd_seq snd_seq_t;
typedef struct snd_seq_addr { unsigned char client, port; } snd_seq_addr_t;
typedef struct snd_seq_event {
    unsigned char type, flags, tag, queue;
    unsigned int time[2];
    snd_seq_addr_t source, dest;
    unsigned char data[12];
} snd_seq_event_t;

static int active = 0;
static FILE *logf = NULL;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int in_hook = 0;
static struct timespec t0;

#define MAXFD 1024
static unsigned char fdclass[MAXFD];   /* 0 unknown, 1 log, 2 ignore */
static char fdname[MAXFD][40];

static ssize_t (*o_write)(int, const void *, size_t);
static int (*o_close)(int);
static int (*o_ioctl)(int, unsigned long, void *);
static int (*o_ioctl64)(int, unsigned long, void *);
static int (*o_out)(snd_seq_t *, snd_seq_event_t *);
static int (*o_outd)(snd_seq_t *, snd_seq_event_t *);
static int (*o_outb)(snd_seq_t *, snd_seq_event_t *);

/* Resolve lazily: other preloaded libs' constructors (customBufferSizeMPC, shim_remap6) may call write()/ioctl()
 * before our constructor has run. */
#define REAL(var, name) (var ? var : (var = dlsym(RTLD_NEXT, name)))

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) * 1000.0 + (t.tv_nsec - t0.tv_nsec) / 1e6;
}

static void logline(const char *fmt, ...) {
    if (!logf) return;
    pthread_mutex_lock(&mu);
    fprintf(logf, "%10.1f ", now_ms());
    va_list ap; va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap);
    fputc('\n', logf); fflush(logf);
    pthread_mutex_unlock(&mu);
}

static void hex(char *dst, int cap, const unsigned char *b, size_t n) {
    int o = 0;
    for (size_t i = 0; i < n && o + 4 < cap; i++) o += snprintf(dst + o, cap - o, "%02x ", b[i]);
    if (n && o + 4 >= cap) snprintf(dst + o - 1, cap - o + 1, "..");
    dst[o] = 0;
}

static int classify(int fd) {
    if (fd < 0 || fd >= MAXFD) return 2;
    if (fdclass[fd]) return fdclass[fd];
    char p[32], t[128];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, t, sizeof t - 1);
    int c = 2;
    if (n > 0) {
        t[n] = 0;
        if (!strncmp(t, "/dev/", 5) && strcmp(t, "/dev/null") && strncmp(t, "/dev/shm", 8) &&
            strncmp(t, "/dev/snd/pcm", 12) && strcmp(t, "/dev/snd/seq") && strncmp(t, "/dev/pts", 8) &&
            strncmp(t, "/dev/dri", 8) && strcmp(t, "/dev/tty0") && strncmp(t, "/dev/input", 10)) {
            c = 1;
            snprintf(fdname[fd], sizeof fdname[fd], "%s", t + 5);
            logline("O fd=%d %s", fd, t);
        }
    }
    fdclass[fd] = c;
    return c;
}

/* ---- injector: hex bytes written to the FIFO /tmp/cvinject go out on the controller's rawmidi fd ----
 *   echo "bf 00 33 bf 01 19" > /tmp/cvinject      send bytes (to the fd the app sends CV on)
 *   echo "fd 46" > /tmp/cvinject                  force the target fd
 */
static volatile int cv_fd = -1;

struct rawmidi_info {
    unsigned device, subdevice; int stream, card; unsigned flags;
    char id[64], name[80], subname[32];
    unsigned subdevices_count, subdevices_avail;
    unsigned char reserved[64];
};
#define RAWMIDI_IOCTL_INFO ((2u << 30) | ((unsigned)sizeof(struct rawmidi_info) << 16) | ('W' << 8) | 0x01)

static void list_midi_fds(void) {
    for (int fd = 0; fd < MAXFD; fd++) {
        char p[32], t[64];
        snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(p, t, sizeof t - 1);
        if (n <= 0) continue;
        t[n] = 0;
        if (strncmp(t, "/dev/snd/midiC", 14)) continue;
        struct rawmidi_info ri; memset(&ri, 0, sizeof ri);
        int r = REAL(o_ioctl, "ioctl")(fd, RAWMIDI_IOCTL_INFO, &ri);
        logline("# midi fd=%d %s info=%d stream=%d sub=%u '%s' '%s'", fd, t, r, ri.stream, ri.subdevice, ri.name, ri.subname);
    }
}

static void *injector(void *arg) {
    (void)arg;
    sleep(10);                                  /* let the app open its devices first */
    list_midi_fds();
    unlink("/tmp/cvinject");
    if (mkfifo("/tmp/cvinject", 0600)) { logline("# mkfifo failed"); return NULL; }
    FILE *f = fopen("/tmp/cvinject", "r+");     /* r+ keeps the FIFO open between writers */
    if (!f) { logline("# fifo open failed"); return NULL; }
    logline("# injector ready");
    char line[512];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "fd %d", &v) == 1) { cv_fd = v; logline("# target fd %d", v); continue; }
        unsigned char b[128]; int n = 0; char *s = line;
        while (n < (int)sizeof b) {
            char *e; long x = strtol(s, &e, 16);
            if (e == s) break;
            b[n++] = (unsigned char)x; s = e;
        }
        if (!n) continue;
        if (cv_fd < 0) { logline("# inject skipped: CV fd unknown (play a CV track once or send 'fd N')"); continue; }
        ssize_t w = write(cv_fd, b, n);
        logline("# injected %d bytes to fd %d -> %d", n, cv_fd, (int)w);
    }
    return NULL;
}

__attribute__((constructor)) static void init(void) {
    char comm[32] = {0};
    FILE *f = fopen("/proc/self/comm", "r");
    if (f) { if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
    o_write = dlsym(RTLD_NEXT, "write");
    o_close = dlsym(RTLD_NEXT, "close");
    o_ioctl = dlsym(RTLD_NEXT, "ioctl");
    o_ioctl64 = dlsym(RTLD_NEXT, "__ioctl_time64");
    o_out = dlsym(RTLD_NEXT, "snd_seq_event_output");
    o_outd = dlsym(RTLD_NEXT, "snd_seq_event_output_direct");
    o_outb = dlsym(RTLD_NEXT, "snd_seq_event_output_buffer");
    if (strcmp(comm, "MPC")) return;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const char *path = getenv("CVSNIFF_LOG");
    logf = fopen(path ? path : "/tmp/cvsniff.log", "w");
    active = logf != NULL;
    logline("# cvsniff active pid=%d", getpid());
    pthread_t th; pthread_create(&th, NULL, injector, NULL); pthread_detach(th);
}

ssize_t write(int fd, const void *buf, size_t n) {
    if (active && !in_hook) {
        in_hook = 1;
        if (classify(fd) == 1) {
            char h[400]; hex(h, sizeof h, buf, n);
            logline("W %s fd=%d len=%u %s", fdname[fd], fd, (unsigned)n, h);
            if (n && ((const unsigned char *)buf)[0] == 0xbf && cv_fd != fd) {
                cv_fd = fd;
                logline("# CV fd is %d", fd);
            }
        }
        in_hook = 0;
    }
    return REAL(o_write, "write")(fd, buf, n);
}

int close(int fd) {
    if (fd >= 0 && fd < MAXFD) fdclass[fd] = 0;
    return REAL(o_close, "close")(fd);
}

static void log_ioctl(int fd, unsigned long req, void *arg) {
    unsigned type = (req >> 8) & 0xff, nr = req & 0xff;
    if (type == 'U' && (nr == 0x13 || nr == 0x1b)) {           /* CTL ELEM_WRITE / TLV_WRITE */
        if (classify(fd) != 1 || !arg) return;
        const unsigned char *v = arg;
        unsigned numid; memcpy(&numid, v, 4);
        const char *name = (const char *)v + 16;
        int vals[8]; memcpy(vals, v + 68, sizeof vals);
        logline("C %s nr=%02x numid=%u name='%.44s' v=%d %d %d %d %d %d %d %d", fdname[fd], nr, numid, name,
                vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6], vals[7]);
    } else if (type == 'U' && fd >= 0 && fd < MAXFD && classify(fd) == 1 && !strncmp(fdname[fd], "bus/usb", 7)) {
        char h[200] = ""; if (arg) hex(h, sizeof h, arg, 24);  /* usbdevfs ctrl/bulk header */
        logline("U %s req=%08lx %s", fdname[fd], req, h);
    }
}

int ioctl(int fd, unsigned long req, void *arg) {
    if (active && !in_hook) { in_hook = 1; log_ioctl(fd, req, arg); in_hook = 0; }
    return REAL(o_ioctl, "ioctl")(fd, req, arg);
}

int __ioctl_time64(int fd, unsigned long req, void *arg) {
    if (active && !in_hook) { in_hook = 1; log_ioctl(fd, req, arg); in_hook = 0; }
    return REAL(o_ioctl64, "__ioctl_time64") ? o_ioctl64(fd, req, arg) : REAL(o_ioctl, "ioctl")(fd, req, arg);
}

static void log_seq(const char *fn, snd_seq_event_t *ev) {
    if (!ev || ev->type == 36 || ev->type == 42) return;          /* skip clock / sensing */
    char h[64]; hex(h, sizeof h, ev->data, 12);
    logline("S %s type=%u %u:%u->%u:%u q=%u %s", fn, ev->type, ev->source.client, ev->source.port,
            ev->dest.client, ev->dest.port, ev->queue, h);
}

int snd_seq_event_output(snd_seq_t *s, snd_seq_event_t *ev) {
    if (active && !in_hook) { in_hook = 1; log_seq("out", ev); in_hook = 0; }
    return REAL(o_out, "snd_seq_event_output")(s, ev);
}
int snd_seq_event_output_direct(snd_seq_t *s, snd_seq_event_t *ev) {
    if (active && !in_hook) { in_hook = 1; log_seq("dir", ev); in_hook = 0; }
    return REAL(o_outd, "snd_seq_event_output_direct")(s, ev);
}
int snd_seq_event_output_buffer(snd_seq_t *s, snd_seq_event_t *ev) {
    if (active && !in_hook) { in_hook = 1; log_seq("buf", ev); in_hook = 0; }
    return REAL(o_outb, "snd_seq_event_output_buffer")(s, ev);
}
