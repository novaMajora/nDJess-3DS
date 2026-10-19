#include "engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD // the 3DS's ARM11 has no NEON
#include "vendor/minimp3.h"

#define CHANNEL 0
#define NUM_BUFS 4
#define BUF_FRAMES 768      // ~17 ms per wave buffer: short, so scratching feels direct
#define READ_SIZE (16 * 1024)
#define RING_BITS 20        // 2^20 stereo frames (~24 s at 44.1 kHz) of decoded audio per deck
#define RING_CAP (1 << RING_BITS)
#define RING_MASK (RING_CAP - 1)
#define HISTORY_SEC 8       // decoded audio kept behind the playhead (scratching back, loops)
#define AHEAD_SEC 10        // decoded ahead of it
#define MAX_TRACK_SEC (40 * 60)
#define WAVE_CAP (MAX_TRACK_SEC * 48000 / WAVE_BLOCK)
#define ENV_CAP (100 * 48000 / WAVE_BLOCK) // the first ~100 s feed the BPM detector
#define BPM_AFTER_SEC 90
#define ECHO_LEN (OUT_HZ * 5 / 2)
#define FLANGE_LEN 1024
#define REC_CAP (OUT_HZ * 4) // stereo frames buffered between the mixer and the SD card
#define RPM 33.333f

static const float LOOP_BEATS[LOOP_SIZES] = {0.25f, 0.5f, 1, 2, 4, 8, 16, 32};

// ---------------------------------------------------------------------------------------------
// Filters (RBJ biquads, transposed direct form II)

typedef struct { float b0, b1, b2, a1, a2; } Coef;
typedef struct { float z1, z2; } BqState;

static inline float bq(const Coef *c, BqState *s, float x) {
    float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}

static Coef biquad(bool high, float hz, float q) {
    float w = 2 * (float)M_PI * hz / OUT_HZ, cs = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
    Coef c;
    if (high) {
        c.b0 = (1 + cs) / 2 / a0;
        c.b1 = -(1 + cs) / a0;
    } else {
        c.b0 = (1 - cs) / 2 / a0;
        c.b1 = (1 - cs) / a0;
    }
    c.b2 = c.b0;
    c.a1 = -2 * cs / a0;
    c.a2 = (1 - al) / a0;
    return c;
}

// ---------------------------------------------------------------------------------------------
// MP3 decoder (from nTapeDeck)

typedef struct {
    FILE *f;
    mp3dec_t dec;
    unsigned char *in;
    size_t in_len;
    long data_start; // after the ID3v2 tag
    long file_size;
    bool eof;
    s16 frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int frame_len;   // stereo frames in `frame`
    int hz;
} Decoder;

static void dec_close(Decoder *d) {
    if (d->f) fclose(d->f);
    free(d->in);
    memset(d, 0, sizeof *d);
}

static void dec_fill(Decoder *d) {
    if (d->eof) return;
    size_t space = READ_SIZE - d->in_len;
    if (!space) return;
    size_t n = fread(d->in + d->in_len, 1, space, d->f);
    d->in_len += n;
    if (n < space) d->eof = true;
}

// Decodes the next MP3 frame into d->frame (always stereo). Returns false at the end of the file.
static bool dec_next(Decoder *d) {
    mp3dec_frame_info_t info;
    static s16 raw[MINIMP3_MAX_SAMPLES_PER_FRAME];
    for (int tries = 0; tries < 64; tries++) {
        if (d->in_len < 2048 && !d->eof) dec_fill(d);
        if (!d->in_len) return false;
        int samples = mp3dec_decode_frame(&d->dec, d->in, (int)d->in_len, raw, &info);
        size_t used = (size_t)info.frame_bytes;
        if (used > d->in_len) used = d->in_len;
        memmove(d->in, d->in + used, d->in_len - used);
        d->in_len -= used;
        if (samples > 0) {
            if (!d->hz) d->hz = info.hz;
            if (info.channels == 1) {
                for (int i = 0; i < samples; i++) d->frame[2 * i] = d->frame[2 * i + 1] = raw[i];
            } else {
                memcpy(d->frame, raw, samples * 2 * sizeof(s16));
            }
            d->frame_len = samples;
            return true;
        }
        if (used == 0) {
            if (d->eof) return false;
            if (d->in_len >= READ_SIZE) d->in_len = 0; // no frame in a full buffer: skip it
            dec_fill(d);
        }
    }
    return false;
}

static unsigned syncsafe(const unsigned char *b) { return (b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F); }

// Opens a file and finds its sample rate and duration (Xing header, or a CBR estimate).
static bool dec_open(Decoder *d, const char *path, float *duration, char *err, int errlen) {
    memset(d, 0, sizeof *d);
    d->f = fopen(path, "rb");
    if (!d->f) { snprintf(err, errlen, "Can't open the file"); return false; }
    setvbuf(d->f, NULL, _IOFBF, 32 * 1024);
    d->in = malloc(READ_SIZE);
    if (!d->in) { snprintf(err, errlen, "Out of memory"); dec_close(d); return false; }
    fseek(d->f, 0, SEEK_END);
    d->file_size = ftell(d->f);
    fseek(d->f, 0, SEEK_SET);
    unsigned char h[10];
    if (fread(h, 1, 10, d->f) == 10 && memcmp(h, "ID3", 3) == 0) d->data_start = 10 + syncsafe(h + 6) + ((h[5] & 0x10) ? 10 : 0);
    fseek(d->f, d->data_start, SEEK_SET);
    mp3dec_init(&d->dec);

    *duration = 0;
    dec_fill(d);
    mp3dec_frame_info_t info;
    static s16 tmp[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_t probe;
    mp3dec_init(&probe);
    int s = mp3dec_decode_frame(&probe, d->in, (int)d->in_len, tmp, &info);
    for (size_t i = 0; i + 120 < d->in_len && i < 4096; i++) {
        if ((!memcmp(d->in + i, "Xing", 4) || !memcmp(d->in + i, "Info", 4)) && (d->in[i + 7] & 1)) {
            unsigned frames = d->in[i + 8] << 24 | d->in[i + 9] << 16 | d->in[i + 10] << 8 | d->in[i + 11];
            if (info.hz > 0) *duration = (float)frames * (s > 0 ? s : 1152) / info.hz;
            break;
        }
    }
    if (*duration <= 0 && info.bitrate_kbps > 0) *duration = (float)(d->file_size - d->data_start) * 8.0f / (info.bitrate_kbps * 1000.0f);
    if (!dec_next(d)) { snprintf(err, errlen, "Not a playable MP3"); dec_close(d); return false; }
    // Rewind so the first frame isn't lost.
    fseek(d->f, d->data_start, SEEK_SET);
    d->in_len = 0;
    d->eof = false;
    d->frame_len = 0;
    mp3dec_init(&d->dec);
    return true;
}

// Approximate seek: jump to a byte offset proportional to the fraction, then resync.
static void dec_seek(Decoder *d, float fraction) {
    if (fraction < 0) fraction = 0;
    if (fraction > 0.999f) fraction = 0.999f;
    fseek(d->f, d->data_start + (long)((d->file_size - d->data_start) * fraction), SEEK_SET);
    d->in_len = 0;
    d->eof = false;
    d->frame_len = 0;
    mp3dec_init(&d->dec);
}

// ---------------------------------------------------------------------------------------------
// State

typedef struct {
    DeckControls ctl;
    LightLock lock;
    // Load request (main thread -> worker)
    char path[512];
    int tag;
    volatile int load_gen;
    // Stream (worker writes under lock; the mixer reads)
    LoadState load;
    char error[64];
    int hz;
    float duration;
    s16 *ring;
    s64 ring_lo, ring_hi, eof; // decoded frames [lo, hi); eof = -1 until the end was reached
    s64 reload_at;
    int reload_gen, reload_ack;
    u8 *wave;
    volatile int wave_n;
    // Analysis results (under lock)
    float bpm, beat_off, rms, analyzed;
    bool analysis_new;
    // Transport (under lock)
    double pos, vel, target, slip_pos, move;
    bool playing, grabbed, speed_mode, slip, slipping, synced, loop_on;
    float scratch_speed, bend, tempo;
    double loop_in, loop_out, cue, hot[HOTCUES];
    bool hot_set[HOTCUES];
    int loop_size;
    // DSP state (mixer thread only)
    BqState eq_lo[2], eq_hi[2], flt[2];
    Coef flt_c;
    float flt_for;
    int last_fx;
    float *echo;
    int echo_w;
    float flange[FLANGE_LEN * 2];
    int flange_w;
    float lfo, crush_hold[2], crush_t, gate;
    float peak;
} Deck;

// Running waveform/analysis accumulation for one decoder.
typedef struct {
    s64 block;
    float peak, bass, lp;
    int n;
} Acc;

typedef struct {
    Decoder dec;
    bool open, eof;
    s64 cursor;     // track frame of the next decoded frame
    int gen, reload_gen;
    Acc acc;
    // Analysis pass (a second decoder that reads the whole file once)
    Decoder adec;
    bool aopen, adone, bpm_done;
    s64 acursor;
    Acc aacc;
    float *env;     // bass energy per block, for the BPM detector
    double sumsq;
    s64 sumn;
} Stream;

typedef struct {
    float *data;
    int len;
    volatile int pos; // -1 idle
    const char *name;
} Pad;

static struct {
    Deck deck[DECKS];
    Stream stream[DECKS];
    MixControls mix;
    Pad pads[PADS];
    // Output
    ndspWaveBuf wave[NUM_BUFS];
    s16 *pcm[NUM_BUFS];
    Thread mixer, worker;
    LightEvent mix_wake, worker_wake;
    volatile bool quit;
    Coef eq_lo_c, eq_hi_c;
    float meter[2];
    // Recorder
    LightLock rec_lock;
    FILE *rec_file;
    volatile bool recording;
    s16 *rec_ring;
    volatile u32 rec_w, rec_r;
    u64 rec_frames;
} E;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float engine_loop_beats(int size) { return LOOP_BEATS[size < 0 ? 0 : size >= LOOP_SIZES ? LOOP_SIZES - 1 : size]; }

// Seconds per beat of a deck's track (120 BPM when unknown).
static double beat_frames(const Deck *k) { return 60.0 / (k->bpm > 0 ? k->bpm : 120.0) * (k->hz ? k->hz : OUT_HZ); }

// ---------------------------------------------------------------------------------------------
// Worker thread: decoding ahead, analysis, recording

static void acc_flush(Deck *k, Acc *a, Stream *s, bool analysis) {
    if (a->n && a->block >= 0 && a->block < WAVE_CAP) {
        float p = a->peak;
        int pk = 1 + (int)(254 * powf(clampf(p, 0, 1), 0.7f));
        float bass = sqrtf(a->bass / a->n) / (p * 0.6f + 0.001f);
        k->wave[a->block * 2] = (u8)pk;
        k->wave[a->block * 2 + 1] = (u8)(255 * clampf(bass, 0, 1));
        if (a->block + 1 > k->wave_n) k->wave_n = (int)a->block + 1;
        if (analysis && s->env && a->block < ENV_CAP) s->env[a->block] = a->bass / a->n;
    }
    a->peak = a->bass = 0;
    a->n = 0;
}

static void acc_push(Deck *k, Acc *a, Stream *s, bool analysis, s64 frame, const s16 *pcm, int n) {
    float lpk = 1 - expf(-2 * (float)M_PI * 150 / (k->hz ? k->hz : 44100));
    for (int i = 0; i < n; i++) {
        s64 b = (frame + i) / WAVE_BLOCK;
        if (b != a->block) {
            acc_flush(k, a, s, analysis);
            a->block = b;
        }
        float l = pcm[2 * i] * (1 / 32768.0f), r = pcm[2 * i + 1] * (1 / 32768.0f), m = (l + r) * 0.5f;
        a->lp += (m - a->lp) * lpk;
        float p = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        if (p > a->peak) a->peak = p;
        a->bass += a->lp * a->lp;
        a->n++;
        if (analysis) {
            s->sumsq += m * m;
            s->sumn++;
        }
    }
}

// Comb filter over the onset curve: the best beat phase for period `P` (blocks) and its score.
static float comb(const float *on, int m, float P, float step, float *phase) {
    float best = -1;
    for (float ph = 0; ph < P; ph += step) {
        float s = 0;
        int c = 0;
        for (float t = ph; t < m; t += P) {
            s += on[(int)t];
            c++;
        }
        if (c && s / c > best) {
            best = s / c;
            *phase = ph;
        }
    }
    return best;
}

// Tempo and first beat from the bass energy per block.
static void detect_bpm(const float *e, int m, float rate, float *bpm_out, float *off_out) {
    *bpm_out = 0;
    *off_out = 0;
    if (m < rate * 8) return;
    float *on = malloc(m * sizeof(float));
    float *sm = malloc(m * sizeof(float));
    if (!on || !sm) { free(on); free(sm); return; }
    float prev = logf(e[0] + 1e-9f), mean = 0;
    on[0] = 0;
    for (int i = 1; i < m; i++) {
        float l = logf(e[i] + 1e-9f), d = l - prev;
        prev = l;
        on[i] = d > 0 ? d : 0;
        mean += on[i];
    }
    mean /= m;
    for (int i = 0; i < m; i++) {
        float a = i > 0 ? on[i - 1] : 0, c = i + 1 < m ? on[i + 1] : 0;
        float v = 0.5f * a + on[i] + 0.5f * c - 2 * mean;
        sm[i] = v > 0 ? v : 0;
    }
    float best = -1, bpm = 0, ph = 0;
    for (float b = 80; b <= 175; b += 0.5f) {
        float p = 0, s = comb(sm, m, 60 * rate / b, 1.0f, &p);
        s *= 1 - 0.15f * fabsf(log2f(b / 125.0f)); // lean towards common dance tempos
        if (s > best) { best = s; bpm = b; }
    }
    best = -1;
    float lo = bpm - 0.6f, hi = bpm + 0.6f;
    for (float b = lo; b <= hi; b += 0.02f) {
        float p = 0, s = comb(sm, m, 60 * rate / b, 0.25f, &p);
        if (s > best) { best = s; bpm = b; ph = p; }
    }
    if (fabsf(bpm - roundf(bpm)) < 0.12f) bpm = roundf(bpm); // most tracks sit on a whole BPM
    *bpm_out = bpm;
    *off_out = ph;
    free(on);
    free(sm);
}

static void finish_bpm(Deck *k, Stream *s) {
    if (s->bpm_done) return;
    s->bpm_done = true;
    int m = (int)(s->acursor / WAVE_BLOCK);
    if (m > ENV_CAP) m = ENV_CAP;
    float bpm, off;
    float rate = (float)k->hz / WAVE_BLOCK;
    detect_bpm(s->env, m, rate, &bpm, &off);
    LightLock_Lock(&k->lock);
    k->bpm = bpm;
    k->beat_off = off / rate;
    LightLock_Unlock(&k->lock);
}

static void stream_close(Stream *s) {
    if (s->open) dec_close(&s->dec);
    if (s->aopen) dec_close(&s->adec);
    s->open = s->aopen = false;
}

static void stream_open(Deck *k, Stream *s) {
    char path[512], err[64];
    LightLock_Lock(&k->lock);
    memcpy(path, k->path, sizeof path);
    LightLock_Unlock(&k->lock);
    stream_close(s);
    float duration = 0;
    bool ok = dec_open(&s->dec, path, &duration, err, sizeof err);
    memset(k->wave, 0, WAVE_CAP * 2);
    k->wave_n = 0;
    s->cursor = s->acursor = 0;
    s->eof = s->adone = s->bpm_done = false;
    s->acc = s->aacc = (Acc){.block = -1};
    s->sumsq = 0;
    s->sumn = 0;
    if (s->env) memset(s->env, 0, ENV_CAP * sizeof(float));
    LightLock_Lock(&k->lock);
    if (ok) {
        s->open = true;
        k->hz = s->dec.hz;
        k->duration = duration > 0 ? duration : 0;
        k->ring_lo = k->ring_hi = 0;
        k->eof = -1;
        k->load = LOAD_READY;
    } else {
        k->load = LOAD_ERROR;
        snprintf(k->error, sizeof k->error, "%s", err);
    }
    s->reload_gen = k->reload_ack = k->reload_gen;
    LightLock_Unlock(&k->lock);
}

static s64 total_frames(Deck *k) { return k->eof >= 0 ? k->eof : (s64)(k->duration * k->hz); }

// Keeps a deck's ring filled around its playhead. Returns true if it did any work.
static bool stream_step(Deck *k, Stream *s) {
    if (k->load_gen != s->gen) {
        s->gen = k->load_gen;
        stream_open(k, s);
        return true;
    }
    if (!s->open) return false;
    LightLock_Lock(&k->lock);
    bool reload = k->reload_gen != s->reload_gen;
    s64 at = k->reload_at;
    int rgen = k->reload_gen;
    double pos = k->pos;
    double protect = pos - HISTORY_SEC * (double)k->hz;
    if (k->loop_on && k->loop_in < protect) protect = k->loop_in;
    if (k->slipping && k->slip_pos - k->hz < protect) protect = k->slip_pos - k->hz;
    s64 total = total_frames(k);
    LightLock_Unlock(&k->lock);

    if (reload) {
        s->reload_gen = rgen;
        if (at < 0) at = 0;
        dec_seek(&s->dec, total > 0 ? (float)at / total : 0);
        s->cursor = at;
        s->eof = false;
        s->acc = (Acc){.block = -1};
        LightLock_Lock(&k->lock);
        k->ring_lo = k->ring_hi = at;
        k->eof = -1;
        k->reload_ack = rgen;
        LightLock_Unlock(&k->lock);
        return true;
    }
    if (s->eof) return false;
    if (s->cursor >= pos + AHEAD_SEC * (double)k->hz) return false;
    if (s->cursor + 4 * 1152 - RING_CAP > protect) return false; // would overwrite audio still needed

    for (int f = 0; f < 4; f++) {
        if (!dec_next(&s->dec)) {
            s->eof = true;
            LightLock_Lock(&k->lock);
            k->eof = s->cursor;
            LightLock_Unlock(&k->lock);
            break;
        }
        int n = s->dec.frame_len;
        s64 c = s->cursor;
        LightLock_Lock(&k->lock);
        if (c + n - RING_CAP > k->ring_lo) k->ring_lo = c + n - RING_CAP;
        LightLock_Unlock(&k->lock);
        int at0 = (int)(c & RING_MASK), first = n < RING_CAP - at0 ? n : RING_CAP - at0;
        memcpy(k->ring + at0 * 2, s->dec.frame, first * 2 * sizeof(s16));
        if (first < n) memcpy(k->ring, s->dec.frame + first * 2, (n - first) * 2 * sizeof(s16));
        acc_push(k, &s->acc, s, false, c, s->dec.frame, n);
        s->cursor = c + n;
        LightLock_Lock(&k->lock);
        k->ring_hi = s->cursor;
        LightLock_Unlock(&k->lock);
    }
    return true;
}

// Reads the whole track once in the background: waveform, BPM and loudness.
static bool analysis_step(Deck *k, Stream *s) {
    if (!s->open || s->adone) return false;
    if (!s->aopen) {
        char path[512], err[64];
        float d;
        LightLock_Lock(&k->lock);
        memcpy(path, k->path, sizeof path);
        LightLock_Unlock(&k->lock);
        if (!dec_open(&s->adec, path, &d, err, sizeof err)) { s->adone = true; return false; }
        s->aopen = true;
    }
    for (int f = 0; f < 6; f++) {
        if (!dec_next(&s->adec)) {
            acc_flush(k, &s->aacc, s, true);
            s->adone = true;
            dec_close(&s->adec);
            s->aopen = false;
            finish_bpm(k, s);
            LightLock_Lock(&k->lock);
            if (s->acursor > 0) k->duration = (float)s->acursor / k->hz; // exact now
            k->analyzed = 1;
            if (s->sumn) k->rms = sqrtf((float)(s->sumsq / s->sumn));
            k->analysis_new = true;
            LightLock_Unlock(&k->lock);
            return true;
        }
        acc_push(k, &s->aacc, s, true, s->acursor, s->adec.frame, s->adec.frame_len);
        s->acursor += s->adec.frame_len;
    }
    if (!s->bpm_done && s->acursor >= (s64)BPM_AFTER_SEC * k->hz) finish_bpm(k, s);
    LightLock_Lock(&k->lock);
    float total = k->duration * k->hz;
    k->analyzed = total > 0 ? clampf(s->acursor / total, 0, 0.99f) : 0;
    if (s->sumn > (s64)k->hz * 20) k->rms = sqrtf((float)(s->sumsq / s->sumn));
    LightLock_Unlock(&k->lock);
    return true;
}

static void rec_flush(void) {
    LightLock_Lock(&E.rec_lock);
    if (E.rec_file) {
        u32 w = E.rec_w, r = E.rec_r;
        while (r != w) {
            u32 at = r % REC_CAP, n = w - r;
            if (n > REC_CAP - at) n = REC_CAP - at;
            fwrite(E.rec_ring + at * 2, sizeof(s16) * 2, n, E.rec_file);
            r += n;
            E.rec_frames += n;
        }
        E.rec_r = r;
    }
    LightLock_Unlock(&E.rec_lock);
}

static void worker_main(void *arg) {
    (void)arg;
    while (!E.quit) {
        bool busy = false;
        for (int d = 0; d < DECKS; d++) busy |= stream_step(&E.deck[d], &E.stream[d]);
        if (E.recording && E.rec_w - E.rec_r >= OUT_HZ / 4) rec_flush();
        if (!busy)
            for (int d = 0; d < DECKS && !busy; d++) busy = analysis_step(&E.deck[d], &E.stream[d]);
        if (!busy) LightEvent_WaitTimeout(&E.worker_wake, 10 * 1000000ULL);
    }
    for (int d = 0; d < DECKS; d++) stream_close(&E.stream[d]);
}

// ---------------------------------------------------------------------------------------------
// Mixer thread

static float eq_gain(float knob, bool kill) {
    if (kill) return 0;
    if (knob < 0.5f) return (knob * 2) * (knob * 2);
    return 1 + (knob - 0.5f) * 2; // up to +6 dB
}

static float soft_clip(float x) {
    float a = fabsf(x);
    if (a <= 0.9f) return x;
    float y = 0.9f + 0.1f * tanhf((a - 0.9f) * 10);
    return x < 0 ? -y : y;
}

// Renders one deck (post EQ/filter/FX, pre-fader) into `out`.
static void deck_render(Deck *k, float *out, int n, const MixControls *m) {
    LightLock_Lock(&k->lock);
    if (k->load != LOAD_READY || !k->hz) {
        LightLock_Unlock(&k->lock);
        memset(out, 0, n * 2 * sizeof(float));
        return;
    }
    double ratio = (double)k->hz / OUT_HZ;
    if (!k->synced) k->tempo = 1 + k->ctl.pitch * m->pitch_range;
    double motor = k->playing ? k->tempo * (1 + k->bend * 0.1f) : 0;
    double v0 = k->vel, v1;
    if (k->grabbed) {
        if (k->speed_mode) k->target += k->scratch_speed * n * ratio;
        else { k->target += k->move; k->move = 0; }
        double want = (k->target - k->pos) / (n * ratio);
        if (want > 12) want = 12;
        if (want < -12) want = -12;
        v1 = v0 + (want - v0) * 0.55;
    } else {
        // Platter start (~0.15 s) and brake (~0.35 s) like a turntable motor.
        double d = motor - v0, max = n / (OUT_HZ * (fabs(motor) > fabs(v0) ? 0.15 : 0.35));
        if (d > max) d = max;
        if (d < -max) d = -max;
        v1 = v0 + d;
    }
    s64 lo = k->ring_lo, hi = k->ring_hi, eof = k->eof;
    const s16 *ring = k->ring;
    double pos = k->pos, slip_step = k->playing ? k->tempo * ratio : 0;
    bool loop = k->loop_on && k->loop_out > k->loop_in;
    float gate_phase = 0, gate_step = 0;
    if (k->ctl.fx == FX_GATE) {
        float bpm = k->bpm > 0 ? k->bpm : 120;
        gate_phase = (float)((pos / k->hz - k->beat_off) * bpm / 60 * 4);
        gate_step = (float)(v1 * ratio / k->hz * bpm / 60 * 4);
    }
    for (int i = 0; i < n; i++) {
        double v = v0 + (v1 - v0) * (i + 1) / n;
        pos += v * ratio;
        if (loop && v > 0 && pos >= k->loop_out) {
            double len = k->loop_out - k->loop_in;
            pos -= len;
            if (k->grabbed) k->target -= len;
        }
        if (k->slipping) k->slip_pos += slip_step;
        if (pos < 0) pos = 0;
        if (eof >= 0 && pos >= eof - 1) {
            pos = eof > 0 ? eof - 1 : 0;
            if (!k->grabbed && v > 0) { // end of the track: the record stops
                k->playing = false;
                v0 = v1 = 0;
            }
        }
        s64 i0 = (s64)pos;
        float l = 0, r = 0;
        if (i0 >= lo && i0 + 1 < hi) {
            float f = (float)(pos - i0);
            const s16 *a = ring + (i0 & RING_MASK) * 2, *b = ring + ((i0 + 1) & RING_MASK) * 2;
            l = (a[0] + (b[0] - a[0]) * f) * (1 / 32768.0f);
            r = (a[1] + (b[1] - a[1]) * f) * (1 / 32768.0f);
            float g = (float)fabs(v) * 4; // a barely moving record is quiet
            if (g < 1) { l *= g; r *= g; }
        }
        out[2 * i] = l;
        out[2 * i + 1] = r;
    }
    k->pos = pos;
    k->vel = v1;
    // Outside the decoded window (a jump, or scratched back past the history): decode from here.
    if ((pos < lo || pos > hi + k->hz) && k->reload_gen == k->reload_ack) {
        k->reload_at = (s64)pos;
        k->reload_gen++;
        LightEvent_Signal(&E.worker_wake);
    }
    float bpm = k->bpm > 0 ? k->bpm : 120, tempo = k->tempo, rms = k->rms;
    LightLock_Unlock(&k->lock);

    // Gain, 3-band isolator EQ, filter, FX.
    const DeckControls *c = &k->ctl;
    float gain = powf(10, (c->trim - 0.5f) * 24 / 20);
    if (m->auto_gain && rms > 0.01f) gain *= clampf(0.18f / rms, 0.4f, 2.5f);
    float g0 = eq_gain(c->eq[0], c->kill[0]), g1 = eq_gain(c->eq[1], c->kill[1]), g2 = eq_gain(c->eq[2], c->kill[2]);
    float fl = c->filter;
    bool filt = fabsf(fl) > 0.03f;
    if (filt && fl != k->flt_for) {
        if ((fl < 0) != (k->flt_for < 0) || k->flt_for == 0) memset(k->flt, 0, sizeof k->flt);
        k->flt_c = fl < 0 ? biquad(false, 20000 * powf(100 / 20000.0f, -fl), 1.2f) : biquad(true, 30 * powf(8000 / 30.0f, fl), 1.2f);
        k->flt_for = fl;
    } else if (!filt) {
        k->flt_for = 0;
    }
    int fx = c->fx;
    if (fx != k->last_fx) {
        if (k->echo) memset(k->echo, 0, ECHO_LEN * 2 * sizeof(float));
        memset(k->flange, 0, sizeof k->flange);
        k->last_fx = fx;
    }
    float amt = c->fx_amount;
    bool on = c->fx_on;
    int delay = (int)(0.5f * 60 / (bpm * tempo) * OUT_HZ); // echo: half a beat
    if (delay < 1) delay = 1;
    if (delay >= ECHO_LEN) delay = ECHO_LEN - 1;
    float fb = 0.35f + 0.4f * amt;
    int crush_every = 1 + (int)(amt * 18);
    float crush_q = powf(2, 15 - amt * 11);
    float lfo_step = 1.0f / (OUT_HZ * 4 * 60 / bpm); // flanger sweep: 4 beats

    for (int i = 0; i < n; i++) {
        for (int ch = 0; ch < 2; ch++) {
            float x = out[2 * i + ch] * gain;
            float lo_ = bq(&E.eq_lo_c, &k->eq_lo[ch], x), hi_ = bq(&E.eq_hi_c, &k->eq_hi[ch], x);
            x = lo_ * g0 + (x - lo_ - hi_) * g1 + hi_ * g2;
            if (filt) x = bq(&k->flt_c, &k->flt[ch], x);
            switch (fx) {
            case FX_ECHO:
                if (k->echo) {
                    int ri = (k->echo_w - delay + ECHO_LEN) % ECHO_LEN;
                    float d = k->echo[ri * 2 + ch];
                    k->echo[k->echo_w * 2 + ch] = (on ? x : 0) + d * fb;
                    x += d * amt; // the tail rings out after FX is switched off
                }
                break;
            case FX_FLANGER:
                if (on) {
                    float dl = (0.0008f + 0.004f * (0.5f + 0.5f * sinf(2 * (float)M_PI * k->lfo))) * OUT_HZ;
                    float rp = k->flange_w - dl;
                    while (rp < 0) rp += FLANGE_LEN;
                    int a = (int)rp, b = (a + 1) % FLANGE_LEN;
                    float f = rp - a, d = k->flange[a * 2 + ch] + (k->flange[b * 2 + ch] - k->flange[a * 2 + ch]) * f;
                    k->flange[k->flange_w * 2 + ch] = x + d * 0.6f;
                    x = x * (1 - 0.4f * amt) + d * 0.8f * amt;
                }
                break;
            case FX_CRUSH:
                if (on) {
                    if (k->crush_t <= 0) k->crush_hold[ch] = roundf(x * crush_q) / crush_q;
                    x = x + (k->crush_hold[ch] - x) * amt;
                }
                break;
            case FX_GATE:
                if (on) {
                    float p = gate_phase + gate_step * i;
                    float open = p - floorf(p) < 0.5f ? 1 : 0;
                    if (ch == 0) k->gate += (open - k->gate) * 0.02f;
                    x *= 1 - amt * (1 - k->gate);
                }
                break;
            }
            out[2 * i + ch] = x;
        }
        if (fx == FX_ECHO) k->echo_w = (k->echo_w + 1) % ECHO_LEN;
        if (fx == FX_FLANGER) {
            k->flange_w = (k->flange_w + 1) % FLANGE_LEN;
            k->lfo += lfo_step;
            if (k->lfo > 1) k->lfo -= 1;
        }
        if (fx == FX_CRUSH && on && --k->crush_t < 0) k->crush_t = crush_every - 1;
    }
}

static void render(s16 *dst, int n) {
    static float mix[BUF_FRAMES * 2], cue[BUF_FRAMES * 2], buf[BUF_FRAMES * 2];
    MixControls m = E.mix;
    memset(mix, 0, n * 2 * sizeof(float));
    memset(cue, 0, n * 2 * sizeof(float));
    float x = clampf(m.crossfader, 0, 1), c = clampf(m.curve, 0, 1);
    float ga = (1 - c) * cosf(x * (float)M_PI / 2) + c * clampf((1 - x) * 12, 0, 1);
    float gb = (1 - c) * sinf(x * (float)M_PI / 2) + c * clampf(x * 12, 0, 1);
    bool any_pfl = false;
    for (int d = 0; d < DECKS; d++) {
        Deck *k = &E.deck[d];
        deck_render(k, buf, n, &m);
        float vol = k->ctl.volume * k->ctl.volume, g = vol * (d == 0 ? ga : gb), pk = 0;
        bool pfl = k->ctl.pfl;
        any_pfl |= pfl;
        for (int i = 0; i < n * 2; i++) {
            float s = buf[i];
            mix[i] += s * g;
            if (pfl) cue[i] += s;
            float a = fabsf(s * vol);
            if (a > pk) pk = a;
        }
        k->peak = pk > k->peak * 0.9f ? pk : k->peak * 0.9f;
    }
    for (int p = 0; p < PADS; p++) {
        Pad *pad = &E.pads[p];
        int at = pad->pos;
        if (at < 0 || !pad->data) continue;
        float v = m.sampler_vol * m.sampler_vol;
        for (int i = 0; i < n && at < pad->len; i++, at++) {
            mix[2 * i] += pad->data[at] * v;
            mix[2 * i + 1] += pad->data[at] * v;
        }
        pad->pos = at >= pad->len ? -1 : at;
    }
    float master = m.master * m.master, pl = 0, pr = 0;
    bool split = m.split_cue && any_pfl;
    for (int i = 0; i < n; i++) {
        float l = soft_clip(mix[2 * i] * master), r = soft_clip(mix[2 * i + 1] * master);
        if (fabsf(l) > pl) pl = fabsf(l);
        if (fabsf(r) > pr) pr = fabsf(r);
        if (E.recording) {
            u32 w = E.rec_w;
            if (w - E.rec_r < REC_CAP) {
                s16 *o = E.rec_ring + (w % REC_CAP) * 2;
                o[0] = (s16)(l * 32767);
                o[1] = (s16)(r * 32767);
                E.rec_w = w + 1;
            }
        }
        if (split) { // left ear: what's cued; right ear: the master
            r = (l + r) * 0.5f;
            l = soft_clip((cue[2 * i] + cue[2 * i + 1]) * 0.5f);
        }
        dst[2 * i] = (s16)(l * 32767);
        dst[2 * i + 1] = (s16)(r * 32767);
    }
    E.meter[0] = pl > E.meter[0] * 0.9f ? pl : E.meter[0] * 0.9f;
    E.meter[1] = pr > E.meter[1] * 0.9f ? pr : E.meter[1] * 0.9f;
}

static void dsp_callback(void *arg) {
    (void)arg;
    LightEvent_Signal(&E.mix_wake);
}

static void mixer_main(void *arg) {
    (void)arg;
    while (!E.quit) {
        for (int i = 0; i < NUM_BUFS; i++) {
            ndspWaveBuf *w = &E.wave[i];
            if (w->status != NDSP_WBUF_DONE && w->status != NDSP_WBUF_FREE) continue;
            render(E.pcm[i], BUF_FRAMES);
            DSP_FlushDataCache(E.pcm[i], BUF_FRAMES * 2 * sizeof(s16));
            w->data_vaddr = E.pcm[i];
            w->nsamples = BUF_FRAMES;
            ndspChnWaveBufAdd(CHANNEL, w);
        }
        LightEvent_Wait(&E.mix_wake);
    }
}

// ---------------------------------------------------------------------------------------------
// Sampler pads: built-in sounds, or sdmc:/3ds/nDJess/samples/A.wav .. D.wav

static float *pad_alloc(Pad *p, float seconds) {
    p->len = (int)(seconds * OUT_HZ);
    p->data = calloc(p->len, sizeof(float));
    if (!p->data) p->len = 0;
    return p->data;
}

static float saw(float ph) { return 2 * (ph - floorf(ph)) - 1; }

static void synth_pads(void) {
    float *d;
    // A: air horn, three short blasts and a long one
    if ((d = pad_alloc(&E.pads[0], 1.7f))) {
        static const float starts[] = {0, 0.2f, 0.4f, 0.62f}, lens[] = {0.14f, 0.14f, 0.14f, 1.0f};
        static const float freqs[] = {466.2f, 554.4f, 698.5f};
        for (int b = 0; b < 4; b++) {
            int s0 = (int)(starts[b] * OUT_HZ), len = (int)(lens[b] * OUT_HZ);
            float ph[3] = {0};
            for (int i = 0; i < len && s0 + i < E.pads[0].len; i++) {
                float t = (float)i / OUT_HZ, env = fminf(1, t * 60) * fminf(1, (lens[b] - t) * 30);
                float bendup = 1 - 0.04f * expf(-t * 25), vib = 1 + 0.006f * sinf(2 * (float)M_PI * 5.5f * t), v = 0;
                for (int k = 0; k < 3; k++) {
                    ph[k] += freqs[k] * bendup * vib / OUT_HZ;
                    v += saw(ph[k]);
                }
                d[s0 + i] = tanhf(v * 0.9f) * 0.55f * env;
            }
        }
    }
    // B: siren
    if ((d = pad_alloc(&E.pads[1], 2.0f))) {
        float ph = 0;
        for (int i = 0; i < E.pads[1].len; i++) {
            float t = (float)i / OUT_HZ, f = 900 + 450 * sinf(2 * (float)M_PI * 1.5f * t);
            ph += f / OUT_HZ;
            float env = fminf(1, t * 20) * fminf(1, (2.0f - t) * 4);
            d[i] = (sinf(2 * (float)M_PI * ph) * 0.7f + 0.3f * saw(ph)) * 0.45f * env;
        }
    }
    // C: 808 boom
    if ((d = pad_alloc(&E.pads[2], 1.4f))) {
        float ph = 0;
        for (int i = 0; i < E.pads[2].len; i++) {
            float t = (float)i / OUT_HZ, f = 48 + 110 * expf(-t * 22);
            ph += f / OUT_HZ;
            d[i] = tanhf(sinf(2 * (float)M_PI * ph) * 1.8f) * 0.8f * expf(-t * 2.6f) * fminf(1, t * 400);
        }
    }
    // D: laser zap, twice
    if ((d = pad_alloc(&E.pads[3], 0.7f))) {
        for (int z = 0; z < 2; z++) {
            float ph = 0;
            int s0 = (int)(z * 0.3f * OUT_HZ), len = (int)(0.28f * OUT_HZ);
            for (int i = 0; i < len; i++) {
                float t = (float)i / OUT_HZ, f = 2600 * expf(-t * 11) + 120;
                ph += f / OUT_HZ;
                d[s0 + i] += (ph - floorf(ph) < 0.5f ? 0.3f : -0.3f) * expf(-t * 6) * fminf(1, t * 500);
            }
        }
    }
    E.pads[0].name = "Air Horn";
    E.pads[1].name = "Siren";
    E.pads[2].name = "Boom";
    E.pads[3].name = "Laser";
}

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

// 16-bit PCM WAV, any rate, mono or stereo, up to 10 s -> mono float at OUT_HZ.
static bool load_wav(Pad *p, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    u8 h[12];
    int ch = 0, rate = 0, bits = 0;
    bool ok = false;
    if (fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        u8 c[8];
        while (fread(c, 1, 8, f) == 8) {
            u32 size = rd32(c + 4);
            if (!memcmp(c, "fmt ", 4)) {
                u8 fmt[16];
                if (size < 16 || fread(fmt, 1, 16, f) != 16) break;
                ch = fmt[2] | fmt[3] << 8;
                rate = (int)rd32(fmt + 4);
                bits = fmt[14] | fmt[15] << 8;
                fseek(f, size - 16 + (size & 1), SEEK_CUR);
            } else if (!memcmp(c, "data", 4)) {
                if (bits != 16 || ch < 1 || ch > 2 || rate < 4000) break;
                int frames = (int)(size / (2 * ch));
                if (frames > rate * 10) frames = rate * 10;
                s16 *raw = malloc(frames * ch * sizeof(s16));
                if (!raw) break;
                frames = (int)fread(raw, 2 * ch, frames, f);
                float *out = calloc((size_t)((double)frames * OUT_HZ / rate) + 1, sizeof(float));
                if (out) {
                    int len = (int)((double)frames * OUT_HZ / rate);
                    for (int i = 0; i < len; i++) {
                        int s = (int)((double)i * rate / OUT_HZ);
                        if (s >= frames) s = frames - 1;
                        out[i] = (ch == 2 ? (raw[2 * s] + raw[2 * s + 1]) * 0.5f : raw[s]) / 32768.0f;
                    }
                    free(p->data);
                    p->data = out;
                    p->len = len;
                    p->name = "Custom";
                    ok = true;
                }
                free(raw);
                break;
            } else {
                fseek(f, size + (size & 1), SEEK_CUR);
            }
        }
    }
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// API

bool engine_init(char *err, int len) {
    memset(&E, 0, sizeof E);
    E.mix = (MixControls){.crossfader = 0.5f, .curve = 0.3f, .master = 0.85f, .sampler_vol = 0.8f, .pitch_range = 0.08f, .auto_gain = true};
    E.eq_lo_c = biquad(false, 300, 0.707f);
    E.eq_hi_c = biquad(true, 3000, 0.707f);
    for (int d = 0; d < DECKS; d++) {
        Deck *k = &E.deck[d];
        LightLock_Init(&k->lock);
        k->ctl = (DeckControls){.volume = 0.85f, .trim = 0.5f, .eq = {0.5f, 0.5f, 0.5f}, .fx_amount = 0.5f};
        k->ring = malloc(RING_CAP * 2 * sizeof(s16));
        k->wave = calloc(WAVE_CAP, 2);
        k->echo = calloc(ECHO_LEN * 2, sizeof(float));
        k->eof = -1;
        k->tempo = 1;
        k->loop_size = 4;
        E.stream[d].env = calloc(ENV_CAP, sizeof(float));
        if (!k->ring || !k->wave || !E.stream[d].env) { snprintf(err, len, "Not enough memory for the decks"); return false; }
    }
    for (int p = 0; p < PADS; p++) E.pads[p].pos = -1;
    synth_pads();
    static const char *names[PADS] = {"A", "B", "C", "D"};
    for (int p = 0; p < PADS; p++) {
        char path[96];
        snprintf(path, sizeof path, "sdmc:/3ds/nDJess/samples/%s.wav", names[p]);
        load_wav(&E.pads[p], path);
    }
    E.rec_ring = malloc(REC_CAP * 2 * sizeof(s16));
    LightLock_Init(&E.rec_lock);

    if (R_FAILED(ndspInit())) {
        snprintf(err, len, "Audio failed to start. Dump your DSP firmware to sdmc:/3ds/dspfirm.cdc (run the DSP1 homebrew once).");
        return false;
    }
    for (int i = 0; i < NUM_BUFS; i++) {
        E.pcm[i] = linearAlloc(BUF_FRAMES * 2 * sizeof(s16));
        if (!E.pcm[i]) { snprintf(err, len, "Out of audio memory"); return false; }
        memset(E.pcm[i], 0, BUF_FRAMES * 2 * sizeof(s16));
        E.wave[i].status = NDSP_WBUF_DONE;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(CHANNEL);
    ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CHANNEL, OUT_HZ);
    ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {0};
    mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(CHANNEL, mix);
    LightEvent_Init(&E.mix_wake, RESET_ONESHOT);
    LightEvent_Init(&E.worker_wake, RESET_ONESHOT);
    ndspSetCallback(dsp_callback, NULL);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    E.mixer = threadCreate(mixer_main, NULL, 32 * 1024, prio - 3, -2, false);
    // The decoders get the second CPU core when the system allows it.
    APT_SetAppCpuTimeLimit(80);
    E.worker = threadCreate(worker_main, NULL, 160 * 1024, prio + 1, 1, false);
    if (!E.worker) E.worker = threadCreate(worker_main, NULL, 160 * 1024, prio + 1, -2, false);
    if (!E.mixer || !E.worker) { snprintf(err, len, "Couldn't start the audio threads"); return false; }
    return true;
}

void engine_exit(void) {
    engine_record_stop();
    E.quit = true;
    LightEvent_Signal(&E.mix_wake);
    LightEvent_Signal(&E.worker_wake);
    if (E.mixer) { threadJoin(E.mixer, U64_MAX); threadFree(E.mixer); }
    if (E.worker) { threadJoin(E.worker, U64_MAX); threadFree(E.worker); }
    ndspSetCallback(NULL, NULL);
    ndspChnWaveBufClear(CHANNEL);
    for (int i = 0; i < NUM_BUFS; i++)
        if (E.pcm[i]) linearFree(E.pcm[i]);
    if (E.pcm[0]) ndspExit();
    for (int d = 0; d < DECKS; d++) {
        free(E.deck[d].ring);
        free(E.deck[d].wave);
        free(E.deck[d].echo);
        free(E.stream[d].env);
    }
    for (int p = 0; p < PADS; p++) free(E.pads[p].data);
    free(E.rec_ring);
}

DeckControls *engine_controls(int deck) { return &E.deck[deck].ctl; }
MixControls *engine_mix(void) { return &E.mix; }

#define LOCKED(d) for (Deck *k = &E.deck[d], *once_ = (LightLock_Lock(&k->lock), k); once_; LightLock_Unlock(&k->lock), once_ = NULL)

void engine_load(int deck, const char *path, int tag) {
    LOCKED(deck) {
        snprintf(k->path, sizeof k->path, "%s", path);
        k->tag = tag;
        k->load = LOAD_LOADING;
        k->error[0] = 0;
        k->playing = k->grabbed = k->slipping = k->loop_on = k->synced = false;
        k->pos = k->vel = k->target = k->move = k->cue = 0;
        k->bend = 0;
        k->bpm = k->beat_off = k->rms = k->analyzed = 0;
        k->analysis_new = false;
        for (int i = 0; i < HOTCUES; i++) k->hot_set[i] = false;
        k->load_gen++;
    }
    LightEvent_Signal(&E.worker_wake);
}

void engine_state(int deck, DeckState *o) {
    LOCKED(deck) {
        double hz = k->hz ? k->hz : OUT_HZ;
        o->load = k->load;
        snprintf(o->error, sizeof o->error, "%s", k->error);
        o->tag = k->tag;
        o->pos = (float)(k->pos / hz);
        o->duration = k->eof >= 0 && k->eof / hz > k->duration ? (float)(k->eof / hz) : k->duration;
        o->playing = k->playing;
        o->grabbed = k->grabbed;
        o->loop_on = k->loop_on;
        o->slip = k->slip;
        o->synced = k->synced;
        o->loop_in = (float)(k->loop_in / hz);
        o->loop_out = (float)(k->loop_out / hz);
        o->cue = (float)(k->cue / hz);
        for (int i = 0; i < HOTCUES; i++) {
            o->hotcue[i] = (float)(k->hot[i] / hz);
            o->hotcue_set[i] = k->hot_set[i];
        }
        o->loop_size = k->loop_size;
        o->bpm = k->bpm;
        o->beat_offset = k->beat_off;
        o->tempo = k->tempo;
        o->speed = (float)k->vel;
        o->peak = k->peak;
        o->analyzed = k->analyzed;
    }
}

static void jump(Deck *k, double frames) {
    if (frames < 0) frames = 0;
    k->pos = frames;
    k->target = frames;
    if (k->slipping) k->slip_pos = frames;
}

void engine_play(int deck, bool on) {
    LOCKED(deck) if (k->load == LOAD_READY) k->playing = on;
}

void engine_toggle_play(int deck) {
    LOCKED(deck) if (k->load == LOAD_READY) k->playing = !k->playing;
}

void engine_cue(int deck) {
    LOCKED(deck) {
        if (k->load != LOAD_READY) continue;
        if (k->playing) {
            k->playing = false;
            k->vel = 0;
            jump(k, k->cue);
        } else if (fabs(k->pos - k->cue) < k->hz * 0.05) {
            jump(k, k->cue);
        } else {
            k->cue = k->pos;
        }
    }
}

void engine_seek(int deck, float seconds) {
    LOCKED(deck) if (k->load == LOAD_READY) jump(k, seconds * (double)k->hz);
}

void engine_hotcue(int deck, int i) {
    LOCKED(deck) {
        if (k->load != LOAD_READY || i < 0 || i >= HOTCUES) continue;
        if (k->hot_set[i]) jump(k, k->hot[i]);
        else { k->hot[i] = k->pos; k->hot_set[i] = true; }
    }
}

void engine_hotcue_clear(int deck, int i) {
    LOCKED(deck) if (i >= 0 && i < HOTCUES) k->hot_set[i] = false;
}

void engine_loop_toggle(int deck) {
    LOCKED(deck) {
        if (k->load != LOAD_READY) continue;
        if (k->loop_on) {
            k->loop_on = false;
            if (k->slipping && !k->grabbed) { jump(k, k->slip_pos); k->slipping = false; }
            continue;
        }
        double beat = beat_frames(k), in = k->pos;
        if (k->bpm > 0) { // quantize the loop start to the nearest beat
            double off = k->beat_off * k->hz, b = round((in - off) / beat);
            double q = off + b * beat;
            if (q >= 0 && fabs(q - in) < beat / 2) in = q;
        }
        k->loop_in = in;
        k->loop_out = in + engine_loop_beats(k->loop_size) * beat;
        k->loop_on = true;
        if (k->slip && !k->slipping) { k->slipping = true; k->slip_pos = k->pos; }
    }
}

void engine_loop_resize(int deck, int dir) {
    LOCKED(deck) {
        int s = k->loop_size + dir;
        if (s < 0 || s >= LOOP_SIZES) continue;
        k->loop_size = s;
        if (k->loop_on) {
            double len = engine_loop_beats(s) * beat_frames(k);
            k->loop_out = k->loop_in + len;
            if (k->pos >= k->loop_out) k->pos = k->loop_in + fmod(k->pos - k->loop_in, len);
        }
    }
}

void engine_slip_toggle(int deck) {
    LOCKED(deck) {
        k->slip = !k->slip;
        if (!k->slip) k->slipping = false;
    }
}

// Tempo of `d` that matches deck `o`, folded to within half/double time.
static float matched_tempo(const Deck *d, const Deck *o) {
    float r = o->bpm * o->tempo / d->bpm;
    while (r > 1.8f) r /= 2; // only fold real half/double time
    while (r < 0.55f) r *= 2;
    return r;
}

void engine_sync(int deck, bool on) {
    Deck *k = &E.deck[deck], *o = &E.deck[1 - deck];
    LightLock_Lock(&E.deck[0].lock);
    LightLock_Lock(&E.deck[1].lock);
    if (!on) {
        k->synced = false;
        k->ctl.pitch = clampf((k->tempo - 1) / (E.mix.pitch_range > 0 ? E.mix.pitch_range : 0.08f), -1, 1);
    } else if (k->bpm > 0 && o->bpm > 0 && k->load == LOAD_READY && o->load == LOAD_READY) {
        k->tempo = matched_tempo(k, o);
        k->synced = true;
        if (o->playing) { // line the beats up with the other deck
            double bk = 60.0 / k->bpm, bo = 60.0 / o->bpm;
            double pk = (k->pos / k->hz - k->beat_off) / bk, po = (o->pos / o->hz - o->beat_off) / bo;
            double diff = (po - floor(po)) - (pk - floor(pk));
            if (diff > 0.5) diff -= 1;
            if (diff < -0.5) diff += 1;
            jump(k, k->pos + diff * bk * k->hz);
        }
    }
    LightLock_Unlock(&E.deck[1].lock);
    LightLock_Unlock(&E.deck[0].lock);
}

void engine_set_pitch(int deck, float pitch) {
    Deck *k = &E.deck[deck];
    k->ctl.pitch = clampf(pitch, -1, 1);
    if (k->synced) LOCKED(deck) k->synced = false;
}

void engine_update(float dt) {
    LightLock_Lock(&E.deck[0].lock);
    LightLock_Lock(&E.deck[1].lock);
    for (int d = 0; d < DECKS; d++) {
        Deck *k = &E.deck[d], *o = &E.deck[1 - d];
        if (k->synced && k->bpm > 0 && o->bpm > 0 && !o->synced) k->tempo = matched_tempo(k, o);
        k->bend *= expf(-dt * 10);
    }
    LightLock_Unlock(&E.deck[1].lock);
    LightLock_Unlock(&E.deck[0].lock);
}

void engine_grab(int deck, bool on) {
    LOCKED(deck) {
        if (on == k->grabbed) continue;
        k->grabbed = on;
        if (on) {
            k->target = k->pos;
            k->move = 0;
            k->scratch_speed = 0;
            k->speed_mode = false;
            if (k->slip && !k->slipping) { k->slipping = true; k->slip_pos = k->pos; }
        } else if (k->slipping && !k->loop_on) {
            jump(k, k->slip_pos);
            k->slipping = false;
        }
    }
}

void engine_scratch_move(int deck, float revolutions) {
    LOCKED(deck) {
        k->move += revolutions * (60.0 / RPM) * (k->hz ? k->hz : OUT_HZ);
        k->speed_mode = false;
    }
}

void engine_scratch_speed(int deck, float speed) {
    LOCKED(deck) {
        k->scratch_speed = speed;
        k->speed_mode = true;
    }
}

void engine_bend(int deck, float amount) {
    LOCKED(deck) k->bend = clampf(amount, -1, 1);
}

void engine_pad(int pad) {
    if (pad >= 0 && pad < PADS && E.pads[pad].data) E.pads[pad].pos = 0;
}

bool engine_pad_active(int pad) { return pad >= 0 && pad < PADS && E.pads[pad].pos >= 0; }
const char *engine_pad_name(int pad) { return pad >= 0 && pad < PADS && E.pads[pad].name ? E.pads[pad].name : ""; }

static void wav_header(FILE *f, u64 frames) {
    u32 data = (u32)(frames * 4), riff = data + 36, rate = OUT_HZ, bps = OUT_HZ * 4;
    u8 h[44] = {'R', 'I', 'F', 'F'};
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16;
    h[20] = 1;
    h[22] = 2;
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &bps, 4);
    h[32] = 4;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

bool engine_record_start(const char *path) {
    if (E.recording || !E.rec_ring) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
    wav_header(f, 0);
    LightLock_Lock(&E.rec_lock);
    E.rec_file = f;
    E.rec_frames = 0;
    E.rec_r = E.rec_w = 0;
    LightLock_Unlock(&E.rec_lock);
    E.recording = true;
    return true;
}

void engine_record_stop(void) {
    if (!E.recording) return;
    E.recording = false;
    rec_flush();
    LightLock_Lock(&E.rec_lock);
    if (E.rec_file) {
        wav_header(E.rec_file, E.rec_frames);
        fclose(E.rec_file);
        E.rec_file = NULL;
    }
    LightLock_Unlock(&E.rec_lock);
}

bool engine_recording(float *seconds) {
    if (seconds) *seconds = (float)(E.rec_frames + (E.rec_w - E.rec_r)) / OUT_HZ;
    return E.recording;
}

void engine_meters(float *left, float *right) {
    *left = E.meter[0];
    *right = E.meter[1];
}

const u8 *engine_wave(int deck, int *blocks, float *block_sec) {
    Deck *k = &E.deck[deck];
    *blocks = k->load == LOAD_READY ? k->wave_n : 0;
    *block_sec = (float)WAVE_BLOCK / (k->hz ? k->hz : OUT_HZ);
    return k->wave;
}

bool engine_take_analysis(int deck, int *tag, float *bpm, float *duration) {
    bool got = false;
    LOCKED(deck) {
        if (!k->analysis_new) continue;
        k->analysis_new = false;
        *tag = k->tag;
        *bpm = k->bpm;
        *duration = k->duration;
        got = true;
    }
    return got;
}
