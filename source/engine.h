// The DJ audio engine: two MP3 decks with their own playhead, EQ, filter and FX, a crossfader
// mix, a four-pad sampler and a recorder, all mixed in software into one ndsp channel.
//
// Threads: a mixer thread (high priority, fed by the DSP callback) renders the output; a worker
// thread decodes each deck ahead of its playhead into a ring buffer, analyses whole tracks
// (waveform, BPM, loudness) and writes recordings to the SD card.
#pragma once

#include <3ds.h>
#include <stdbool.h>

#define DECKS 2
#define HOTCUES 4
#define PADS 4
#define OUT_HZ 44100
#define WAVE_BLOCK 256 // decoded frames per waveform block
#define LOOP_SIZES 8

typedef enum { FX_ECHO, FX_FLANGER, FX_CRUSH, FX_GATE, FX_COUNT } FxType;
typedef enum { LOAD_EMPTY, LOAD_LOADING, LOAD_READY, LOAD_ERROR } LoadState;

// Knobs and faders of one channel. The UI writes these directly; the mixer reads them.
typedef struct {
    float volume;    // channel fader 0..1
    float trim;      // gain knob 0..1 (0.5 = unity, ±12 dB)
    float eq[3];     // low, mid, high knobs 0..1 (0.5 = flat, 0 = cut)
    bool kill[3];
    float filter;    // -1 low-pass .. 0 off .. 1 high-pass
    bool pfl;        // headphone cue
    int fx;          // FxType
    bool fx_on;
    float fx_amount; // 0..1
    float pitch;     // tempo fader -1..1, scaled by MixControls.pitch_range
} DeckControls;

typedef struct {
    float crossfader;  // 0 = deck A only, 1 = deck B only
    float curve;       // crossfader curve: 0 smooth .. 1 sharp cut
    float master;      // 0..1
    float sampler_vol; // 0..1
    float pitch_range; // e.g. 0.08 for ±8%
    bool split_cue;    // headphone preview: cue in the left ear, master in the right
    bool auto_gain;    // level tracks to the same loudness
} MixControls;

typedef struct {
    LoadState load;
    char error[64];
    int tag;              // whatever was passed to engine_load (the library index)
    float pos, duration;  // seconds
    bool playing, grabbed, loop_on, slip, synced;
    float loop_in, loop_out, cue;
    float hotcue[HOTCUES];
    bool hotcue_set[HOTCUES];
    int loop_size;        // index into engine_loop_beats()
    float bpm;            // detected tempo of the track (0 = unknown yet)
    float beat_offset;    // seconds to the first beat
    float tempo;          // playback speed set by the pitch fader / sync (1 = normal)
    float speed;          // actual platter speed now (negative while scratching back)
    float peak;           // channel meter 0..1
    float analyzed;       // 0..1 progress of the waveform/BPM analysis
} DeckState;

bool engine_init(char *err, int len);
void engine_exit(void);
// Main thread, once per frame: tempo sync and pitch-bend decay.
void engine_update(float dt);

DeckControls *engine_controls(int deck);
MixControls *engine_mix(void);

void engine_load(int deck, const char *path, int tag);
void engine_state(int deck, DeckState *out);
void engine_play(int deck, bool on);
void engine_toggle_play(int deck);
void engine_cue(int deck);               // CDJ cue: back to the cue point (playing) or set it (paused)
void engine_seek(int deck, float seconds);
void engine_hotcue(int deck, int i);     // set when empty, jump when set
void engine_hotcue_clear(int deck, int i);
void engine_loop_toggle(int deck);
void engine_loop_resize(int deck, int dir);
float engine_loop_beats(int size);       // beats for a loop size index
void engine_slip_toggle(int deck);
void engine_sync(int deck, bool on);
void engine_set_pitch(int deck, float pitch); // fader move: also ends sync

// Scratching. While grabbed, the record follows the hand instead of the motor.
void engine_grab(int deck, bool on);
void engine_scratch_move(int deck, float revolutions); // platter turned by this much (touch, stick spin)
void engine_scratch_speed(int deck, float speed);      // stick velocity mode: 1 = normal forward speed
void engine_bend(int deck, float amount);              // nudge -1..1 (jog wheel rim)

void engine_pad(int pad);
bool engine_pad_active(int pad);
const char *engine_pad_name(int pad);

bool engine_record_start(const char *path);
void engine_record_stop(void);
bool engine_recording(float *seconds);

void engine_meters(float *left, float *right);

// The waveform of a deck: 2 bytes per WAVE_BLOCK frames (peak, bass share), 0 = not decoded yet.
// `blocks` receives how many blocks are known, `block_sec` the length of one.
const u8 *engine_wave(int deck, int *blocks, float *block_sec);

// True once when a deck's analysis finished; gives the results for the BPM cache.
bool engine_take_analysis(int deck, int *tag, float *bpm, float *duration);
