/* sound.c - synthesised effects over ALSA. Nothing is held open between
 * sounds; if the card is busy the sound is simply skipped. */
#include "sound.h"

#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define RATE 44100
#define MAXLEN (RATE * 2) /* longest effect: 2 s */

static pthread_t thread;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned pending;          /* bit per Sfx */
static bool running, quitting;
static atomic_bool enabled = true;
static char device[64] = "default";

/* ------------------------------------------------------------ synthesis */

/* a wooden tap: a very short filtered noise burst on top of a low thump */
static void tap(float *b, int at, float amp, float bright, float pitch, unsigned seed) {
    float lp = 0;
    for (int i = 0; i < RATE / 12 && at + i < MAXLEN; i++) {
        float t = (float)i / RATE;
        seed = seed * 1103515245u + 12345u;
        float noise = (float)((seed >> 16) & 0x7fff) / 16384.0f - 1.0f;
        lp += bright * (noise - lp);
        float click = lp * expf(-t / 0.004f);
        float thump = sinf(2 * (float)M_PI * pitch * t) * expf(-t / 0.02f);
        b[at + i] += amp * (0.9f * click + 0.55f * thump);
    }
}

/* a soft bell: fundamental plus a quiet octave */
static void bell(float *b, int at, float freq, float amp, float decay) {
    for (int i = 0; at + i < MAXLEN; i++) {
        float t = (float)i / RATE;
        float env = (1 - expf(-t * 300)) * expf(-t * decay);
        if (env < 0.0005f && t > 0.05f) break;
        float s = sinf(2 * (float)M_PI * freq * t) + 0.25f * sinf(2 * (float)M_PI * 2 * freq * t);
        b[at + i] += amp * env * s;
    }
}

/* a muted low "bonk" for a mistyped move */
static void bonk(float *b, int at, float amp) {
    for (int i = 0; i < RATE / 6 && at + i < MAXLEN; i++) {
        float t = (float)i / RATE;
        float f = 170 - 60 * t;
        float env = (1 - expf(-t * 400)) * expf(-t * 22);
        b[at + i] += amp * env * (sinf(2 * (float)M_PI * f * t) + 0.3f * sinf(4 * (float)M_PI * f * t));
    }
}

#define MS(x) ((int)(RATE * (x) / 1000))

/* writes the effect into b (mono) and returns its length in frames */
static int synth(Sfx s, float *b) {
    switch (s) {
    case SFX_MOVE: tap(b, 0, 0.55f, 0.30f, 190, 7); return MS(120);
    case SFX_CAPTURE: tap(b, 0, 0.8f, 0.55f, 150, 11); tap(b, MS(24), 0.5f, 0.45f, 230, 13); return MS(160);
    case SFX_CASTLE: tap(b, 0, 0.5f, 0.30f, 190, 7); tap(b, MS(95), 0.5f, 0.30f, 200, 9); return MS(220);
    case SFX_CHECK: tap(b, 0, 0.55f, 0.35f, 190, 7); bell(b, MS(35), 1318.5f, 0.16f, 14); return MS(400);
    case SFX_PROMOTE: tap(b, 0, 0.55f, 0.3f, 190, 7); bell(b, MS(40), 880, 0.14f, 10); bell(b, MS(130), 1318.5f, 0.14f, 10); return MS(600);
    case SFX_ERROR: bonk(b, 0, 0.30f); return MS(170);
    case SFX_CORRECT: bell(b, 0, 784, 0.17f, 7); bell(b, MS(110), 1174.7f, 0.17f, 6); return MS(800);
    case SFX_WRONG: bell(b, 0, 440, 0.15f, 8); bell(b, MS(140), 349.2f, 0.15f, 7); return MS(800);
    case SFX_TICK: bell(b, 0, 1975.5f, 0.07f, 40); return MS(90);
    case SFX_START: bell(b, 0, 523.3f, 0.16f, 6); bell(b, MS(120), 659.3f, 0.16f, 6); bell(b, MS(240), 784, 0.18f, 5); return MS(1100);
    case SFX_WIN: bell(b, 0, 523.3f, 0.15f, 5); bell(b, MS(110), 659.3f, 0.15f, 5); bell(b, MS(220), 784, 0.15f, 5);
                  bell(b, MS(330), 1046.5f, 0.17f, 4); return MS(1500);
    case SFX_END: bell(b, 0, 659.3f, 0.14f, 6); bell(b, MS(160), 523.3f, 0.14f, 5); return MS(1000);
    default: return 0;
    }
}

/* ------------------------------------------------------------- output */

#ifdef __linux__
/* Headphones in? Then play softer, as notification chimes on this machine
 * do. The codec's jack switch shows up as an input device. */
static bool headphones_in(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return false;
    bool in = false;
    for (struct dirent *e; (e = readdir(dir));) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300], name[64] = "";
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long sw[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof name), name) > 0 && strstr(name, "Headphones") &&
            ioctl(fd, EVIOCGSW(sizeof sw), sw) >= 0 &&
            ((sw[SW_HEADPHONE_INSERT / (8 * sizeof(long))] >> (SW_HEADPHONE_INSERT % (8 * sizeof(long)))) & 1))
            in = true;
        close(fd);
    }
    closedir(dir);
    return in;
}
#else
static bool headphones_in(void) { return false; }
#endif

/* CHESS_SOUND_DEVICE, else the first sound card that isn't HDMI (boards
 * like this one default to HDMI, which usually has no speaker), else default */
static void pick_device(void) {
    const char *env = getenv("CHESS_SOUND_DEVICE");
    if (env && *env) { snprintf(device, sizeof device, "%s", env); return; }
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char *name = NULL;
        if (snd_card_get_name(card, &name) != 0) continue;
        bool hdmi = strcasestr(name, "hdmi") != NULL;
        free(name);
        if (hdmi) continue;
        char hw[16];
        snprintf(hw, sizeof hw, "hw:%d", card);
        snd_ctl_t *ctl;
        if (snd_ctl_open(&ctl, hw, 0) == 0) {
            snd_ctl_card_info_t *info;
            snd_ctl_card_info_alloca(&info);
            if (snd_ctl_card_info(ctl, info) == 0)
                snprintf(device, sizeof device, "plughw:CARD=%s", snd_ctl_card_info_get_id(info));
            snd_ctl_close(ctl);
            if (strncmp(device, "default", 7)) return;
        }
    }
}

static void play_buffer(const float *b, int n) {
    static short pcm16[MAXLEN * 2];
    float g = headphones_in() ? 0.2f : 1.0f;
    for (int i = 0; i < n; i++) {
        float v = b[i] * g * 32767;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm16[2 * i] = pcm16[2 * i + 1] = (short)v;
    }
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0) < 0) return; /* busy: skip */
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, RATE, 1, 60000) < 0) {
        snd_pcm_close(pcm);
        return;
    }
    for (int done = 0; done < n;) {
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, pcm16 + 2 * done, (snd_pcm_uframes_t)(n - done));
        if (w == -EPIPE) { snd_pcm_prepare(pcm); continue; }
        if (w < 0) break;
        done += (int)w;
    }
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
}

static void *worker(void *arg) {
    (void)arg;
    static float mix[MAXLEN];
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!pending && !quitting) pthread_cond_wait(&cv, &mu);
        if (quitting) { pthread_mutex_unlock(&mu); return NULL; }
        unsigned todo = pending;
        pending = 0;
        pthread_mutex_unlock(&mu);
        /* everything requested together is mixed into one sound */
        memset(mix, 0, sizeof mix);
        int len = 0;
        for (int s = 0; s < SFX_COUNT; s++) {
            if (!(todo & (1u << s))) continue;
            static float one[MAXLEN];
            memset(one, 0, sizeof one);
            int n = synth((Sfx)s, one);
            for (int i = 0; i < n; i++) mix[i] += one[i];
            if (n > len) len = n;
        }
        if (len) play_buffer(mix, len);
    }
}

/* ALSA prints to stderr when a card is busy; skipping is the plan, so hush */
static void quiet(const char *file, int line, const char *fn, int err, const char *fmt, ...) {
    (void)file; (void)line; (void)fn; (void)err; (void)fmt;
}

void sound_init(void) {
    if (running) return;
    const char *off = getenv("CHESS_SOUND");
    if (off && !strcmp(off, "0")) atomic_store(&enabled, false);
    pick_device();
    snd_lib_error_set_handler(quiet);
    running = pthread_create(&thread, NULL, worker, NULL) == 0;
}

void sound_shutdown(void) {
    if (!running) return;
    pthread_mutex_lock(&mu);
    quitting = true;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    pthread_join(thread, NULL);
    running = false;
}

void sound_play(Sfx s) {
    if (!running || !atomic_load(&enabled) || s >= SFX_COUNT) return;
    pthread_mutex_lock(&mu);
    pending |= 1u << s;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

void sound_set_enabled(bool on) { atomic_store(&enabled, on); }
bool sound_enabled(void) { return atomic_load(&enabled); }
const char *sound_device(void) { return device; }
