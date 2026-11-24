/* SPDX-License-Identifier: GPL-3.0-only */
/* BUOY: an FM-1 firmware on Dinghy's platform, for exploration and drones.
 *
 * The effects are the synth. The SEA (an oscillator that never stops, on the scale's root) runs into three BELLS
 * (RESONATOR, SHIFTER or PRISM): the notes only retune the bells, the ENV only opens and closes how much sea each
 * bell gets.
 *
 *   KEYS / ARP -> SEQ (3 tracks) -> SCALE -> BELL 1 2 3  <- SEA      ->  REVERB, ECHO
 *
 * The files: bell.h (the sea, the bells, the ENV and the LFO), scale.h (the scales), effects.h (reverb and echo,
 * in parallel), this file (the ARP, the SEQ, the presets, the pages and the panel). platform.c is Dinghy's
 * plumbing. */
#define APP_NAME "BUOY"
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: the clock and MIDI keep going */
#define CV_MAX (240u * 150u)                    /* the pictures (224 x 60) and the chain (210 x 18) */
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif

#include "bell.h"
#include "scale.h"
#include "effects.h"

/* ------------------------------------------------------------------------------------------------ the sound */
static int32_t output_level(int32_t sample)
{
    /* the master VOLUME, then a soft ceiling (as LOOPHEADS) */
    int32_t magnitude = (sample < 0 ? -sample : sample) * (int32_t)volume / 40;
    if (magnitude > 20000) {
        int32_t excess = magnitude - 20000;
        magnitude = 20000 + 12000 * excess / (12000 + excess);
    }
    return sample < 0 ? -magnitude : magnitude;
}

static void app_audio(int32_t *out, uint32_t frames)
{
    bell_render(out, frames);
    effects_process(out, frames);
    for (uint32_t i = 0; i < 2u * frames; i++) out[i] = output_level(out[i]);
}

/* ------------------------------------------------------------------------------------------------ settings */
#define LONG_MS 700u                            /* a press this long is a "hold" */
#define GATE_PERCENT 60u                        /* how long an ARP or SEQ note is held, of its step */
#define KEY0 53u                                /* key 0 = F3 */

static const uint16_t div_ticks[] = {384, 192, 96, 64, 48, 32, 24, 16, 12};    /* 96 ticks a beat */
static const char *const div_names[] = {"1 BAR", "1/2", "1/4", "1/4 T", "1/8", "1/8 T", "1/16", "1/16 T", "1/32"};
#define DIVS (sizeof div_ticks / sizeof div_ticks[0])
#define DIV_EIGHTH 4u

#define ECHO_CLOCKS 18u                         /* the echo: a dotted eighth (24 MIDI clocks a beat) */
#define ECHO_FEEDBACK 55u
#define HALL_DECAY 82u
#define HALL_DAMPING 40u

static struct { uint8_t bpm; } glo = {80};

/* ---------------------------------------------------------------------------------------- the ARP
 * A normal arpeggiator: hold keys and it plays them, low to high, over RANGE octaves; let go and it stops.
 * PATTERN says how it walks through them, CHANCE whether a step plays. */
#define REST 0xFFu
#define TIE 0xFEu                               /* (in the SEQ: the note before goes on) */
enum { PAT_UP, PAT_DOWN, PAT_BOUNCE, PAT_RANDOM, PAT_WALK, PATTERNS };
static const char *const pattern_names[PATTERNS] = {"UP", "DOWN", "BOUNCE", "RANDOM", "WALK"};
static struct {
    uint8_t on, speed, pattern, range, chance;
    int16_t idx;
    int8_t dir;
    uint32_t rnd;
    uint8_t history[16], hist_n;                /* what it played last, for the screen (0..255 high; REST) */
} arp = {0, DIV_EIGHTH, PAT_UP, 1, 100, -1, 1, 0x1234567u, {0}, 0};

/* A note in the ARP and the SEQ is a scale step (0 = the root), + STEP_BIAS so it fits a byte. */
#define STEP_BIAS 64

static uint8_t held_notes[8], held_n;           /* the keys (and MIDI notes) held, in the order pressed */

static uint8_t arp_random(void)
{
    arp.rnd = arp.rnd * 1664525u + 1013904223u;
    return (uint8_t)(arp.rnd >> 24);
}

static int32_t key_step(uint8_t note);

static uint32_t arp_notes(int32_t list[32])     /* the held notes as steps, low to high, RANGE octaves of them */
{
    int32_t base[8];
    uint32_t n = 0, total = 0;
    for (uint32_t i = 0; i < held_n; i++) {
        int32_t s = key_step(held_notes[i]), j = (int32_t)n;
        uint8_t dup = 0;
        for (uint32_t k = 0; k < n; k++) dup |= base[k] == s;
        if (dup) continue;
        while (j > 0 && base[j - 1] > s) { base[j] = base[j - 1]; j--; }
        base[j] = s;
        n++;
    }
    for (uint32_t o = 0; o < arp.range; o++)
        for (uint32_t i = 0; i < n && total < 32u; i++) list[total++] = base[i] + (int32_t)(o * scale_now.size);
    return total;
}

static uint8_t arp_step(void)                   /* the next note (step + STEP_BIAS) or REST */
{
    int32_t list[32];
    int32_t n = (int32_t)arp_notes(list);
    if (!n) return REST;
    switch (arp.pattern) {
    case PAT_UP: arp.idx = (int16_t)((arp.idx + 1) % n); break;
    case PAT_DOWN: arp.idx = (int16_t)(arp.idx <= 0 || arp.idx > n ? n - 1 : arp.idx - 1); break;
    case PAT_BOUNCE:
        if (n < 2) { arp.idx = 0; break; }
        if (arp.idx < 0) { arp.idx = 0; arp.dir = 1; break; }
        if (arp.idx + arp.dir >= n || arp.idx + arp.dir < 0) arp.dir = (int8_t)-arp.dir;
        arp.idx = (int16_t)(arp.idx + arp.dir);
        break;
    case PAT_RANDOM: arp.idx = (int16_t)(arp_random() * (uint32_t)n >> 8); break;
    default: {                                  /* WALK: one step up or down, now and then two */
        uint8_t r = arp_random();
        int32_t d = arp.idx < 0 ? 1 : r < 100 ? -1 : r < 200 ? 1 : r < 228 ? -2 : 2;
        int32_t i = arp.idx + d;
        if (i < 0) i = -i;
        if (i >= n) i = 2 * (n - 1) - i;
        arp.idx = (int16_t)(i < 0 ? 0 : i >= n ? n - 1 : i);
    }
    }
    if (arp.idx >= n) arp.idx = (int16_t)(n - 1);
    uint8_t plays = arp_random() * 100u / 256u < arp.chance;
    for (uint32_t i = 15; i > 0; i--) arp.history[i] = arp.history[i - 1];
    arp.history[0] = plays ? (uint8_t)(n > 1 ? arp.idx * 255 / (n - 1) : 128) : REST;
    if (arp.hist_n < 16) arp.hist_n++;
    return plays ? (uint8_t)(list[arp.idx] + STEP_BIAS) : REST;
}

/* ------------------------------------------------------------------------------------- the SEQ: one track
 * Up to 32 steps: notes, RESTs, and TIEs (the note before goes on). Three ways to record (MODE):
 *   LIVE  what you play (keys or ARP), on the RATE grid, from the first note
 *   STEP  a key a step; F#3 is a REST, G#3 a TIE
 *   ARP   what the ARP plays, step for step, from its first note (its rests too)
 * Nothing is saved. */
#define SEQ_STEPS 32u
enum { MODE_LIVE, MODE_STEP, MODE_ARP, MODES };
static const char *const mode_names[MODES] = {"LIVE", "STEP", "ARP"};
#define KEY_REST 1u                             /* F#3 */
#define KEY_TIE 3u                              /* G#3 */
static struct {
    uint8_t steps[SEQ_STEPS];
    uint8_t len, speed, mode, bell, chance, playing;
    uint8_t rec;                                /* 0, 1 = waiting for the first note, 2 = recording */
    uint8_t pending, next_pending;              /* LIVE: what goes into the step being recorded, and the next one */
} seq = {{0}, 0, DIV_EIGHTH, MODE_LIVE, 2, 100, 0, 0, REST, REST};   /* the SEQ starts on bell 3, the keys on bell 1 */
static uint32_t seq_version, chance_rnd = 0x6c8e9cf5u;

static void seq_clear(void)
{
    memset(seq.steps, REST, sizeof seq.steps);
    seq.len = 0;
    seq_version++;
}

/* ---------------------------------------------------------------------------------- the bells, from the notes
 * Each bell remembers the step it plays, so a new scale or key retunes it at once, held bells too. */
static int32_t bell_step[VOICES];
static uint8_t bell_has_step[VOICES], bell_midi[VOICES];
static uint8_t selected;
static void seq_heard(int32_t step);

static int32_t partner_of(int32_t step)         /* SPREAD: the partner's distance, 1/256 semitones */
{
    if (snd.spread == SPREAD_OFF) return 0;
    return scale_step_pitch(step + scale_spread_steps(snd.spread)) - scale_step_pitch(step);
}

static void bell_on(uint32_t v, int32_t step)
{
    if (voices[v].frozen) return;
    bell_step[v] = step;
    bell_has_step[v] = 1;
    uint8_t midi = scale_step_note(step);
    if (bell_midi[v]) midi_send(0x80u | v, bell_midi[v], 0);
    midi_send(0x90u | v, midi, 100);              /* MIDI out: bell 1..3 on channels 1..3 */
    bell_midi[v] = midi;
    bell_play(v, scale_step_pitch(step), partner_of(step));
}

static void bell_off(uint32_t v)
{
    if (voices[v].frozen) return;
    if (bell_midi[v]) { midi_send(0x80u | v, bell_midi[v], 0); bell_midi[v] = 0; }
    bell_release(v);
}

static void bells_retune(void)                  /* the scale, the key or SPREAD changed */
{
    for (uint32_t v = 0; v < VOICES; v++) {
        if (!bell_has_step[v]) continue;
        voices[v].partner = partner_of(bell_step[v]);
        voices[v].target = scale_step_pitch(bell_step[v]);
    }
    int32_t root = SCALE_BASE + scale_now.transpose;   /* the sea: the root, kept between C2 and B4 */
    while (root < 36) root += 12;
    while (root >= 72) root -= 12;
    sea_pitch = root * 256;
}

/* the bell the keys and the ARP play: the selected one, or the next one not held */
static int32_t live_bell(void)
{
    for (uint32_t i = 0; i < VOICES; i++) {
        uint32_t v = (selected + i) % VOICES;
        if (!voices[v].frozen) return (int32_t)v;
    }
    return -1;
}

/* ------------------------------------------------------------------------------------------- the clock
 * 96 ticks a beat, from the tempo or from a MIDI clock (4 ticks a MIDI clock). It always runs: the ARP plays
 * whenever keys are held; the SEQ when PLAY is on. */
enum { PLAYER_SEQ, PLAYER_ARP, PLAYERS };
static struct {
    uint32_t start, next, off, idx;
    int8_t bell;                                /* the bell it plays now, -1 = none */
} players[PLAYERS];
static struct { uint32_t tick, acc, last_ms, ext_ticks, last_midi_clock; } clk;

static uint32_t player_div(uint32_t p) { return div_ticks[p == PLAYER_ARP ? arp.speed : seq.speed]; }

static void player_restart(uint32_t p, uint32_t at)
{
    players[p].start = at;
    players[p].idx = 0;
    players[p].next = at;
}

static void note_off(uint32_t p)
{
    if (players[p].bell < 0) return;
    bell_off((uint32_t)players[p].bell);
    players[p].bell = -1;
}

static void note_on(uint32_t p, uint32_t bell, int32_t step, uint32_t gate_ticks)
{
    if (voices[bell].frozen) return;
    bell_on(bell, step);
    players[p].bell = (int8_t)bell;
    players[p].off = clk.tick + gate_ticks;
}

static int32_t octave_steps(void) { return scale_now.octave * (int32_t)scale_now.size; }

static void rec_end(void)
{
    if (!seq.rec) return;
    uint8_t was_live = seq.mode == MODE_LIVE && seq.rec == 2;
    if (was_live && seq.len < SEQ_STEPS) {      /* the step being recorded counts */
        seq.steps[seq.len++] = seq.pending;
        seq_version++;
    }
    seq.rec = 0;
    if (seq.len && seq.mode != MODE_STEP) {     /* LIVE and ARP: it plays at once, from the next step */
        seq.playing = 1;
        player_restart(PLAYER_SEQ, seq.mode == MODE_ARP ? players[PLAYER_ARP].next : players[PLAYER_SEQ].next);
    }
}

static void rec_begin(void)
{
    note_off(PLAYER_SEQ);
    seq.playing = 0;
    seq_clear();
    seq.rec = seq.mode == MODE_STEP ? 2 : 1;
    if (seq.mode == MODE_ARP) seq.speed = arp.speed;
    seq.pending = seq.next_pending = REST;
}

static void seq_write(uint8_t value)
{
    seq.steps[seq.len++] = value;
    seq_version++;
    if (seq.len >= SEQ_STEPS) { seq.rec = 0; if (seq.mode != MODE_STEP) { seq.playing = 1; player_restart(PLAYER_SEQ, players[PLAYER_SEQ].next); } }
}

/* LIVE: a note was just heard: into the step nearest to it */
static void seq_heard(int32_t step)
{
    if (seq.mode != MODE_LIVE || !seq.rec) return;
    uint8_t v = (uint8_t)(step + STEP_BIAS);
    if (seq.rec == 1) {                         /* the first note starts the recording, on its own step */
        seq.rec = 2;
        player_restart(PLAYER_SEQ, clk.tick);
        players[PLAYER_SEQ].idx = 1;
        players[PLAYER_SEQ].next = clk.tick + player_div(PLAYER_SEQ);
        seq.pending = v;
        seq.next_pending = REST;
        return;
    }
    uint32_t div = player_div(PLAYER_SEQ), into = div - (players[PLAYER_SEQ].next - clk.tick);
    if (into < div / 2u) seq.pending = v; else seq.next_pending = v;
}

/* the number of TIEs after step s (the note goes on through them) */
static uint32_t ties_after(uint32_t s)
{
    uint32_t n = 0;
    while (n + 1u < seq.len && seq.steps[(s + 1u + n) % seq.len] == TIE) n++;
    return n;
}

static void player_fire(uint32_t p)
{
    if (p == PLAYER_ARP) {
        uint8_t recording = seq.rec && seq.mode == MODE_ARP;
        if (!arp.on || (!held_n && !(recording && seq.rec == 2))) return;
        uint8_t n = arp_step();
        note_off(p);
        int32_t bell = live_bell();
        if (n != REST && bell >= 0) {
            note_on(p, (uint32_t)bell, (int32_t)n - STEP_BIAS + octave_steps(), player_div(p) * GATE_PERCENT / 100u);
            seq_heard((int32_t)n - STEP_BIAS + octave_steps());
        }
        if (recording) {
            if (seq.rec == 1 && n != REST) seq.rec = 2;   /* from its first note */
            if (seq.rec == 2) seq_write(n == REST ? REST : (uint8_t)(n + octave_steps()));
        }
        return;
    }
    /* the SEQ */
    if (seq.rec == 2 && seq.mode == MODE_LIVE) {  /* LIVE recording: the step just finished goes in */
        seq_write(seq.pending);
        uint8_t holding = held_n || (players[PLAYER_ARP].bell >= 0);
        seq.pending = seq.next_pending != REST ? seq.next_pending : held_n && !arp.on ? TIE : REST;
        (void)holding;
        seq.next_pending = REST;
        return;
    }
    if (!seq.playing || !seq.len || seq.rec) return;
    uint32_t s = players[p].idx % seq.len;
    uint8_t v = seq.steps[s];
    if (v == TIE) return;                       /* (the note before is still on) */
    note_off(p);
    if (v == REST) return;
    if (seq.chance < 100) {
        chance_rnd = chance_rnd * 1664525u + 1013904223u;
        if ((chance_rnd >> 24) * 100u / 256u >= seq.chance) return;
    }
    uint32_t div = player_div(p);
    int32_t bell = -1;                          /* its bell, or the next one not held */
    for (uint32_t i = 0; i < VOICES && bell < 0; i++)
        if (!voices[(seq.bell + i) % VOICES].frozen) bell = (int32_t)((seq.bell + i) % VOICES);
    if (bell >= 0) note_on(p, (uint32_t)bell, (int32_t)v - STEP_BIAS, div * ties_after(s) + div * GATE_PERCENT / 100u);
}

static void clock_tick(void)
{
    clk.tick++;
    for (uint32_t p = 0; p < PLAYERS; p++) {
        if (players[p].bell >= 0 && (int32_t)(clk.tick - players[p].off) >= 0) note_off(p);
        if ((int32_t)(clk.tick - players[p].next) >= 0) {
            player_fire(p);
            players[p].idx++;
            players[p].next = players[p].start + players[p].idx * player_div(p);
        }
    }
}

static uint32_t external_bpm_value;
static int external_clock(void) { return fm1_ms - clk.last_midi_clock < 500u; }

static void seq_start(void)
{
    if (!seq.len) return;
    seq.playing = 1;
    note_off(PLAYER_SEQ);
    player_restart(PLAYER_SEQ, clk.tick + 1u);
}

static void seq_stop(void)
{
    if (seq.rec) rec_end();
    seq.playing = 0;
    note_off(PLAYER_SEQ);
}

static void clock_service(void)
{
    uint32_t now = fm1_ms, elapsed = now - clk.last_ms;
    clk.last_ms = now;
    if (external_clock()) {
        while (clk.ext_ticks) { clk.ext_ticks--; clock_tick(); }
        return;
    }
    clk.ext_ticks = 0;
    if (elapsed > 100u) elapsed = 100u;         /* after a long pause (a save), do not rush */
    clk.acc += elapsed * glo.bpm * 96u;         /* in 1/60000 ticks */
    while (clk.acc >= 60000u) { clk.acc -= 60000u; clock_tick(); }
}

/* ------------------------------------------------------------------------------------ the keys and MIDI in
 * With the ARP off, a bell plays one note: the last key held wins; letting it go goes back to the one still held.
 * With the ARP on, the keys held are what it plays. */
static int32_t held_bell = -1;

static int32_t key_step(uint8_t note) { return scale_note_step(note + 12 * scale_now.octave); }

static void live_press(uint8_t note)
{
    uint8_t first = held_n == 0;
    if (held_n < 8) held_notes[held_n++] = note;
    if (arp.on) {
        if (first) {                            /* the ARP starts with the first key, on the beat it is pressed */
            arp.idx = -1;
            arp.dir = 1;
            player_restart(PLAYER_ARP, clk.tick + 1u);
        }
        return;
    }
    int32_t bell = held_bell >= 0 && !voices[held_bell].frozen ? held_bell : live_bell();
    if (bell < 0) return;
    held_bell = bell;
    bell_on((uint32_t)bell, key_step(note));
    seq_heard(key_step(note));
}

static void live_lift(uint8_t note)
{
    uint32_t j = 0;
    for (uint32_t i = 0; i < held_n; i++) if (held_notes[i] != note) held_notes[j++] = held_notes[i];
    held_n = (uint8_t)j;
    if (arp.on) {
        if (!held_n && !(seq.rec == 2 && seq.mode == MODE_ARP)) note_off(PLAYER_ARP);
        return;
    }
    if (held_bell < 0) return;
    if (held_n) bell_on((uint32_t)held_bell, key_step(held_notes[held_n - 1]));
    else { bell_off((uint32_t)held_bell); held_bell = -1; }
}

static void live_all_up(void)                   /* (the ARP turned on or off with keys held) */
{
    if (held_bell >= 0) { bell_off((uint32_t)held_bell); held_bell = -1; }
    note_off(PLAYER_ARP);
}

static uint32_t idle_ok, clk_n, clk_t0;

static void midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2) != 0) {
        uint32_t ty = st & 0xF0u;
        if (st == 0xF8u) {                      /* clock: 24 a beat */
            clk.last_midi_clock = fm1_ms;
            clk.ext_ticks += 4u;
            if (clk_n % 24u == 0u) {
                if (clk_n) {
                    uint32_t ms = fm1_ms - clk_t0;
                    external_bpm_value = ms ? (60000u + ms / 2u) / ms : 0;
                }
                clk_t0 = fm1_ms;
            }
            clk_n++;
            continue;
        }
        if (st == 0xFAu) { clk_n = 0; seq_start(); continue; }
        if (st == 0xFCu) { seq_stop(); continue; }
        if (st == 0xFBu) { if (seq.len) seq.playing = 1; continue; }
        if (ty == 0x90u && d2) live_press(d1 & 127u);
        else if (ty == 0x80u || ty == 0x90u) live_lift(d1 & 127u);
    }
}

static void app_idle(void)
{
    if (!idle_ok) return;
    midi_in();
    clock_service();
}

/* small text helpers (no printf on the FM-1) */
static char *put_num(char *p, int32_t v, int sign)
{
    char tmp[12];
    uint32_t n = 0, u = (uint32_t)(v < 0 ? -v : v);
    if (v < 0) *p++ = '-';
    else if (sign && v > 0) *p++ = '+';
    do { tmp[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

/* ---------------------------------------------------------------------------------------------- presets
 * 8 sea states to start from, and 8 USER slots that SAVE fills (kept in flash). */
#define FACTORY 8u
#define USERS 8u
static const char *const factory_names[FACTORY] = {
    "BELL BUOY", "FOGHORN", "GLASS HARBOR", "TIDE CHOIR", "DEEP WATER", "SHIMMER", "REED STORM", "STILL"
};
/*   engine         wave        det tone push ring col spread           grit  A  D  S  R    lfo target, wave, rate, depth   sea bell rev echo */
static const sound_t factory[FACTORY] = {
    {ENG_RESONATOR, WAVE_SAW,     7,  70, 60, 70, 55, SPREAD_OFF,       25,  6, 20, 60, 26, LFO_COLOR, LFO_SINE, 14, 30,   0, 80, 45, 20},
    {ENG_SHIFTER,   WAVE_SAW,    12,  45, 80, 30, 40, SPREAD_OCT_DOWN,  10, 17, 21, 80, 25, LFO_PITCH, LFO_DRIFT, 16, 8,  10, 80, 50,  0},
    {ENG_PRISM,     WAVE_GLASS,   4, 100, 70, 80, 70, SPREAD_5TH,       15, 11, 24, 50, 28, LFO_RING, LFO_TRIANGLE, 20, 25, 0, 80, 60, 30},
    {ENG_SHIFTER,   WAVE_VOICE,   9,  80, 75, 55, 60, SPREAD_3RD,       20, 19, 22, 85, 27, LFO_VOLUME, LFO_SINE, 12, 20,  0, 80, 65, 15},
    {ENG_RESONATOR, WAVE_NOISE,   0,  40, 70, 90, 30, SPREAD_OCT_DOWN,   0, 16, 25, 70, RELEASE_FOREVER, LFO_COLOR, LFO_DRIFT, 8, 40, 5, 85, 55, 0},
    {ENG_SHIFTER,   WAVE_ORGAN,   6,  90, 60, 75, 75, SPREAD_OCT_UP,     5, 14, 24, 70, 28, LFO_PITCH, LFO_SINE, 26, 5,    0, 75, 70, 25},
    {ENG_PRISM,     WAVE_REED,   15,  85, 85, 45, 85, SPREAD_OFF,       45,  5, 18, 40, 21, LFO_GRIT, LFO_RANDOM, 30, 40,  0, 80, 40, 35},
    {ENG_RESONATOR, WAVE_SINE,    3, 100, 80, 60, 50, SPREAD_5TH,       35, 20, 25, 90, 30, LFO_PUSH, LFO_SINE, 10, 30,  15, 80, 55, 10},
};
static struct { uint32_t magic; sound_t user[USERS]; } store;
#define PRESET_MAGIC 0x42554F31u                /* "BUO1" */
#define PRESET_BLOB 0u
static uint8_t preset, last_user = FACTORY;

static void preset_name(uint8_t p, char *b)
{
    if (p < FACTORY) { put_str(b, factory_names[p]); return; }
    put_str(b, "USER ");
    b[5] = (char)('1' + p - FACTORY);
    b[6] = 0;
}

static void preset_load(uint8_t p)
{
    preset = p;
    snd = p < FACTORY ? factory[p] : store.user[p - FACTORY];
    if (snd.engine >= ENGINES) snd = factory[0];
    if (p >= FACTORY) last_user = p;
    bells_retune();
}

static void presets_init(void)
{
    if (blob_load(PRESET_BLOB, &store, sizeof store) < 0 || store.magic != PRESET_MAGIC) {
        store.magic = PRESET_MAGIC;
        for (uint32_t i = 0; i < USERS; i++) store.user[i] = factory[i];
    }
}

/* -------------------------------------------------------------------------------------------- the screen
 * As LOOPHEADS and Endless Memory: IBM Plex Mono, the title and the battery on top, then the CHAIN (the signal
 * path, on every page: SEA -> BELL -> SPACE, the part this page changes lit), a picture, the four knobs numbered
 * underneath, a hint at the bottom. Two colours carry the idea: teal is the sea, amber is the bell (the pitch). */
#include "ui_font.h"
#include "buoy_boot_logo.h"

#define C_BG RGB(10, 16, 22)
#define C_BELL RGB(239, 159, 39)
#define C_BELL_DIM RGB(112, 78, 30)
#define C_SEA RGB(93, 202, 165)
#define C_SEA_DIM RGB(42, 96, 80)
#define C_KEY RGB(236, 232, 222)
#define C_OFF RGB(140, 152, 160)
#define C_RULE RGB(44, 58, 68)
#define C_GRAPH RGB(22, 33, 42)
#define C_REC RGB(255, 96, 96)
#define UI_LEFT 15u
#define UI_WIDTH 210u
#define UI_FOOTER_Y 218u
#define UI_BATTERY_WIDTH 27u
#define CHAIN_Y 46u                             /* the chain: 210 x 16 */
#define PIC_Y 68u                               /* the picture: 210 x 60 */
#define PIC_H 60u
#define ROW_Y 134u                              /* the four knob rows */
#define ROW_H 19u
#define BOOT_LOGO_MS 1600u
#define BOX_X 8u
#define BOX_W 224u

static const int16_t sine64[64] = {
    0, 3212, 6393, 9512, 12539, 15446, 18204, 20787, 23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
    32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329, 23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
    0, -3212, -6393, -9512, -12539, -15446, -18204, -20787, -23170, -25329, -27245, -28898, -30273, -31356, -32137,
    -32609, -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329, -23170, -20787, -18204, -15446, -12539,
    -9512, -6393, -3212
};

enum { P_HOME, P_ARP, P_SEQ, P_EDIT, P_ENV, P_LFO, P_FX, P_GLO };
enum { FOCUS_NONE, FOCUS_SEA, FOCUS_BELL, FOCUS_NOTES };

static uint8_t page_focus(uint8_t page)
{
    switch (page) {
    case P_EDIT: return FOCUS_SEA;
    case P_FX: case P_ENV: case P_LFO: return FOCUS_BELL;
    case P_ARP: case P_SEQ: case P_GLO: return FOCUS_NOTES;
    default: return FOCUS_NONE;
    }
}

static void ui_text(uint32_t x, uint32_t y, uint32_t width, const felucca_font_t *font,
                    const char *text, uint16_t color, int align)
{
    draw_text_box_bg(x, y, width, font, text, color, align, C_BG);
}

static void ui_divider(uint32_t y, uint16_t color) { lcd_fill(UI_LEFT, y, UI_WIDTH, 1, color); }

static void ui_footer(const char *hint, uint16_t color)
{
    ui_text(UI_LEFT, UI_FOOTER_Y, UI_WIDTH, &UI_SMALL, hint, color, 1);
}

/* a knob row: "1 RING ........ 2.4 S" */
static void ui_row(uint32_t knob, const char *label, const char *value, uint16_t value_colour)
{
    char n[3] = {(char)('1' + knob), ' ', 0};
    cv_begin(UI_WIDTH, UI_BODY.h, C_BG);
    if (*label) {
        int32_t x = cv_text(0, 0, &UI_BODY, n, C_OFF, C_BG);
        cv_text(x, 0, &UI_BODY, label, C_KEY, C_BG);
        cv_text((int32_t)UI_WIDTH - text_w(&UI_BODY, value), 0, &UI_BODY, value, value_colour, C_BG);
    }
    cv_blit(UI_LEFT, ROW_Y + knob * ROW_H);
}

#if !BOARD_SMK37
static uint8_t read_battery_level(uint32_t now)
{
    static uint32_t last_read;
    static int32_t average;
    static uint8_t level = UINT8_MAX;
    if (now - last_read >= 250u) {
        last_read = now;
        int32_t raw = fm1_adc_read(FM1_ADC_BATT);
        if (raw > 0) {
            average = average ? average + (raw * 16 - average) / 4 : raw * 16;
            level = average >= 591 * 16 ? 3 : average >= 561 * 16 ? 2 : average >= 531 * 16 ? 1 : 0;
        }
    }
    return level;
}

static void draw_battery(uint8_t level)
{
    uint16_t outline = level == UINT8_MAX ? C_RULE : C_OFF;
    cv_begin(UI_BATTERY_WIDTH, 12, C_BG);
    cv_rect(0, 0, 24, 12, outline);
    cv_rect(1, 1, 22, 10, C_BG);
    cv_rect(24, 4, 3, 4, outline);
    if (level != UINT8_MAX)
        for (uint32_t i = 0; i < level; i++) cv_rect(3 + i * 6, 3, 5, 6, C_SEA);
    cv_blit(UI_LEFT + UI_WIDTH - UI_BATTERY_WIDTH, 17);
}
#endif

static void draw_boot_logo(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
    uint32_t run = 0, remaining = 0;
    uint16_t color = 0;
    for (uint32_t y = 0; y < BOOT_LOGO_HEIGHT; y += cv_h) {
        cv_begin(BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT - y, C_BG);
        uint16_t *pixel = cv_px;
        uint32_t pixels = cv_w * cv_h;
        while (pixels) {
            if (!remaining) {
                remaining = boot_logo_runs[run++];
                color = swap16(boot_logo_runs[run++]);
            }
            uint32_t count = remaining < pixels ? remaining : pixels;
            remaining -= count;
            pixels -= count;
            while (count--) *pixel++ = color;
        }
        cv_blit(BOOT_LOGO_X, BOOT_LOGO_Y + y);
    }
    ui_text(UI_LEFT, 206, UI_WIDTH, &UI_SMALL, "v" APP_VER, C_OFF, 1);
}

static void fmt(char *b, int32_t v, const char *unit) { put_str(put_num(b, v, 0), unit); }

static void fmt_ms(char *b, uint32_t ms)        /* "0 MS", "250 MS", "1.5 S", "12 S" */
{
    if (ms < 1000u) { fmt(b, (int32_t)ms, " MS"); return; }
    char *p = put_num(b, (int32_t)(ms / 1000u), 0);
    if (ms < 10000u && ms % 1000u) { *p++ = '.'; *p++ = (char)('0' + ms % 1000u / 100u); }
    put_str(p, " S");
}

static const char *const note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static void pitch_name(char *b, int32_t pitch)  /* 1/256 semitones -> "G4" */
{
    int32_t n = (pitch + 128) >> 8;
    char *p = put_str(b, note_names[n % 12]);
    put_num(p, n / 12 - 1, 0);
}

static int32_t bell_sounds(uint32_t v) { return voices[v].awake && voices[v].peak > 40; }

/* --- the CHAIN: SEA C3 --> BELL G4 --> SPACE, on every page. The sea's note never moves; the bell's does. */
static void draw_chain(uint8_t focus)
{
    char sea[12], bell[16];
    char *p = put_str(sea, "SEA ");
    pitch_name(p, sea_pitch);
    p = put_str(bell, "BELL ");
    *p++ = (char)('1' + selected);
    *p++ = ' ';
    if (voices[selected].awake || voices[selected].frozen) pitch_name(p, voices[selected].shown);
    else put_str(p, "--");
    const char *space = "SPACE";
    int32_t ws = text_w(&UI_SMALL, sea), wb = text_w(&UI_SMALL, bell), wp = text_w(&UI_SMALL, space);
    int32_t gap = ((int32_t)UI_WIDTH - ws - wb - wp) / 2;
    int32_t xb = ws + gap, xp = xb + wb + gap;
    uint16_t cs = focus == FOCUS_SEA || focus == FOCUS_NONE ? C_SEA : C_SEA_DIM;
    uint16_t cb = focus == FOCUS_BELL || focus == FOCUS_NOTES || focus == FOCUS_NONE ? C_BELL : C_BELL_DIM;
    uint16_t cp = focus == FOCUS_NONE ? C_KEY : C_OFF;
    cv_begin(UI_WIDTH, 16, C_BG);
    cv_text(0, 0, &UI_SMALL, sea, cs, C_BG);
    cv_text(xb, 0, &UI_SMALL, bell, cb, C_BG);
    cv_text(xp, 0, &UI_SMALL, space, cp, C_BG);
    for (int32_t i = 0; i < 2; i++) {           /* the arrows */
        int32_t a = (i ? xb + wb : ws) + 4, z = (i ? xp : xb) - 5;
        uint16_t c = i ? C_OFF : cs;
        if (!i) for (int32_t x = a; x < z; x++) cv_pset(x, 6 + sine64[(uint32_t)(x * 8) & 63u] / 16384, c);   /* the sea: a wave */
        else cv_rect(a, 6, z - a, 1, c);
        cv_line(z - 3, 3, z, 6, c);
        cv_line(z - 3, 9, z, 6, c);
    }
    if (focus != FOCUS_NONE) {                  /* this page changes this part: underlined */
        int32_t x = focus == FOCUS_SEA ? 0 : xb, w = focus == FOCUS_SEA ? ws : wb;
        cv_rect(x, 14, w, 2, focus == FOCUS_SEA ? C_SEA : C_BELL);
    }
    cv_blit(UI_LEFT, CHAIN_Y);
}

/* a 2-pixel frame, as LOOPHEADS draws around the selected tape */
static void cv_box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    cv_rect(x, y, w, 2, c);
    cv_rect(x, y + h - 2, w, 2, c);
    cv_rect(x, y + 2, 2, h - 4, c);
    cv_rect(x + w - 2, y + 2, 2, h - 4, c);
}

static void cv_ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    uint32_t n = r < 8 ? 32u : 64u, step = 64u / n;
    for (uint32_t a = 0; a < 64u; a += step)
        cv_pset(cx + sine64[(a + 16u) & 63u] * r / 32768, cy + sine64[a] * r / 32768, c);
}

/* --- HOME: the harbour. Three bells ringing over the sea; their rings grow with how loud they are. */
static void draw_harbour(uint32_t now)
{
    cv_begin(BOX_W, PIC_H, C_BG);
    uint32_t t = now / 60u;
    for (int32_t x = 0; x < (int32_t)BOX_W; x++) {   /* the sea, moving */
        cv_pset(x, 52 + sine64[(uint32_t)(x * 3 + (int32_t)t * 2) & 63u] * 3 / 32768, C_SEA);
        if (x & 1) cv_pset(x, 57 + sine64[(uint32_t)(x * 3 + (int32_t)t + 20) & 63u] * 2 / 32768, C_SEA_DIM);
    }
    for (uint32_t v = 0; v < VOICES; v++) {
        int32_t cx = 37 + (int32_t)v * 75, cy = 13;
        const voice_t *x = &voices[v];
        if (v == selected) cv_box(cx - 36, 0, 72, 48, C_BELL_DIM);
        int32_t level = x->awake ? x->peak : 0;
        uint16_t c = x->frozen ? C_SEA : C_BELL;
        int32_t rings = level > 9000 ? 3 : level > 2500 ? 2 : level > 300 ? 1 : 0;
        uint32_t pulse = (t / 2u + v * 3u) % 6u;
        for (int32_t r = 1; r <= rings; r++)
            cv_ring(cx, cy, 3 + r * 4 + (int32_t)(pulse > 2u && r == rings), r == rings ? C_BELL_DIM : c);
        cv_rect(cx - 2, cy - 2, 5, 5, level > 300 || x->frozen ? c : C_OFF);       /* the bell */
        cv_line(cx - 3, 29, cx - 1, cy + 3, C_OFF);                                  /* the cage and the hull */
        cv_line(cx + 3, 29, cx + 1, cy + 3, C_OFF);
        cv_rect(cx - 6, 29, 13, 4, C_KEY);
        char label[12], *p = label;
        *p++ = (char)('1' + v); *p++ = ' ';
        if (x->awake || x->frozen) pitch_name(p, x->shown);
        else put_str(p, "--");
        int32_t lw = text_w(&UI_SMALL, label);
        cv_text(cx - lw / 2, 34, &UI_SMALL, label, x->frozen ? C_SEA : v == selected ? C_BELL : C_OFF, C_BG);
        if (x->frozen) cv_rect(cx - lw / 2, 46, lw, 2, C_SEA);   /* a held bell: teal, underlined */
    }
    cv_blit(BOX_X, PIC_Y);
}

static void draw_home_rows(void)
{
    char v[8];
    fmt(v, snd.sea, "%");
    ui_row(0, "SEA (DRY)", v, C_SEA);
    fmt(v, snd.bell, "%");
    ui_row(1, "BELLS", v, C_BELL);
    fmt(v, snd.reverb, "%");
    ui_row(2, "REVERB", v, C_KEY);
    fmt(v, snd.echo, "%");
    ui_row(3, "ECHO", v, C_KEY);
}

/* --- EDIT: the sea's wave, two cycles */
static void draw_sea_pic(void)
{
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_rect(0, 30, UI_WIDTH, 1, C_GRAPH);
    uint32_t ns = 0x1234567u;
    int32_t py = 30, sm = 0;
    int32_t amp = 20 + snd.push / 10;
    for (uint32_t x = 0; x < UI_WIDTH; x++) {
        int32_t s;
        if (snd.wave == WAVE_NOISE) { s = white(&ns); sm += (s - sm) / 3; s = sm * 2; }
        else {
            uint32_t i = x * 512u / UI_WIDTH;
            s = sea_tables[snd.wave][i & 255u];
            if (snd.detune) s = (s + sea_tables[snd.wave][(i + i * snd.detune / 200u) & 255u]) / 2;
        }
        int32_t y = 30 - s * amp / 32768;
        if (x) cv_line((int32_t)x - 1, py, (int32_t)x, y, C_SEA);
        py = y;
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_edit_rows(void)
{
    char v[10];
    ui_row(0, "WAVE", wave_names[snd.wave], C_SEA);
    if (snd.detune) fmt(v, snd.detune, " CENTS");
    ui_row(1, "DETUNE", snd.detune ? v : "ONE OSC", C_SEA);
    fmt(v, snd.tone, "%");
    ui_row(2, "TONE", snd.tone == 100 ? "OPEN" : v, C_SEA);
    fmt(v, snd.push, "%");
    ui_row(3, "PUSH", v, C_SEA);
}

/* --- FX: the pitch strip. Five octaves, low to high. Teal lines: the sea's overtones, always on its root.
 * Amber: what the bells make of it, at their notes. The effect, not the oscillator, puts the notes there. */
#define STRIP_LO (36 * 256)                     /* C2 .. C7 */
static int32_t strip_x(int32_t pitch) { return (pitch - STRIP_LO) * 7 / 512; }   /* 3.5 px a semitone */

static void draw_strip(void)
{
    static const int16_t harmonic_semis[8] = {0, 3072, 4869, 6144, 7133, 7941, 8624, 9216};   /* 12 log2 k, x256 */
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_rect(0, 48, UI_WIDTH, 1, C_RULE);
    for (int32_t o = 0; o <= 5; o++) cv_rect(strip_x(STRIP_LO + o * 3072), 49, 1, 3, C_RULE);
    /* the sea */
    if (snd.wave == WAVE_NOISE) {
        for (int32_t x = 0; x < (int32_t)UI_WIDTH; x += 3) cv_pset(x, 45, C_SEA_DIM);
    } else {
        for (uint32_t k = 0; k < 8u; k++) {
            int32_t x = strip_x(sea_pitch + harmonic_semis[k]);
            if (x < 0 || x >= (int32_t)UI_WIDTH) continue;
            int32_t h = 20 / (int32_t)(k + 1u) + 2;
            cv_rect(x, 48 - h, 1, h, k ? C_SEA_DIM : C_SEA);
        }
    }
    char name[8];
    pitch_name(name, sea_pitch);
    int32_t sx = strip_x(sea_pitch);
    cv_text(sx - 3, 50, &UI_SMALL, "SEA", C_SEA, C_BG);
    /* the bells */
    uint8_t any = 0;
    for (uint32_t v = 0; v < VOICES; v++) {
        const voice_t *x = &voices[v];
        if (!x->awake && !x->frozen) continue;
        int32_t level = x->peak, h0 = 8 + level * 30 / 32768;
        uint16_t c = v == selected ? C_BELL : C_BELL_DIM;
        int32_t base = x->shown;
        any = 1;
        uint32_t n = snd.engine == ENG_PRISM ? 4u : 6u;
        for (uint32_t k = 0; k < n; k++) {
            int32_t px = strip_x(base + (snd.engine == ENG_SHIFTER ? harmonic_semis[k] : harmonic_semis[k]));
            if (px < 0 || px >= (int32_t)UI_WIDTH) continue;
            int32_t h = h0 / (int32_t)(k + 1u);
            if (snd.engine == ENG_PRISM && k) h = h * snd.color / 100;
            if (h < 1) continue;
            if (snd.engine == ENG_RESONATOR) { cv_line(px - 2, 48, px, 48 - h, c); cv_line(px, 48 - h, px + 2, 48, c); }
            else cv_rect(px, 48 - h, snd.engine == ENG_PRISM ? 1 : 2, h, c);
        }
        if (snd.spread != SPREAD_OFF) {
            int32_t px = strip_x(base + x->partner);
            if (px >= 0 && px < (int32_t)UI_WIDTH) cv_rect(px, 48 - h0 * 2 / 3, 1, h0 * 2 / 3, C_BELL_DIM);
        }
        if (v == selected) {                    /* the sea -> the note: how far the effect moved it */
            int32_t bx = strip_x(base);
            if (bx > 0 && bx < (int32_t)UI_WIDTH && (bx - sx > 6 || sx - bx > 6)) {
                int32_t y = 4;
                cv_rect(sx < bx ? sx : bx, y, sx < bx ? bx - sx : sx - bx, 1, C_BELL);
                int32_t d = bx > sx ? -1 : 1;
                cv_line(bx, y, bx + 3 * d, y - 3, C_BELL);
                cv_line(bx, y, bx + 3 * d, y + 3, C_BELL);
            }
            pitch_name(name, base);
            if (bx >= 0 && bx < (int32_t)UI_WIDTH) cv_text(bx + 3 > 186 ? 186 : bx + 3, 8, &UI_SMALL, name, C_BELL, C_BG);
        }
    }
    if (!any) cv_text(((int32_t)UI_WIDTH - text_w(&UI_SMALL, "PLAY A KEY")) / 2, 18, &UI_SMALL, "PLAY A KEY", C_OFF, C_BG);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_fx_rows(void)
{
    char v[12];
    fmt_ms(v, ring_ms[snd.ring]);
    ui_row(0, "RING", v, C_BELL);
    fmt(v, snd.color, "%");
    ui_row(1, "COLOR", v, C_BELL);
    ui_row(2, "SPREAD", spread_names[snd.spread], C_BELL);
    fmt(v, snd.grit, "%");
    ui_row(3, "GRIT", v, C_BELL);
}

/* --- ENV: how much sea reaches the bells */
static void draw_env_pic(void)
{
    uint8_t forever = snd.release >= RELEASE_FOREVER;
    int32_t times[] = {env_ms[snd.attack] + 60, env_ms[snd.decay] + 60, 900, forever ? 1500 : env_ms[snd.release] + 60};
    int32_t sq[4];
    for (uint32_t i = 0; i < 4; i++) sq[i] = (int32_t)isqrt32((uint32_t)times[i] * 64u);   /* (a gentler time axis) */
    int32_t total = sq[0] + sq[1] + sq[2] + sq[3];
    int32_t x[5] = {0}, elapsed = 0;
    for (uint32_t i = 0; i < 4; i++) { elapsed += sq[i]; x[i + 1] = elapsed * (int32_t)(UI_WIDTH - 1u) / total; }
    int32_t sustain = 56 - (int32_t)snd.sustain * 50 / 100;
    int32_t y[] = {56, 6, sustain, sustain, forever ? sustain : 56};
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    for (uint32_t i = 0; i < 4; i++) {
        int32_t width = x[i + 1] - x[i];
        for (int32_t col = 0; col < width; col++) {
            int32_t top = y[i] + (y[i + 1] - y[i]) * col / width;
            cv_rect(x[i] + col, top, 1, 57 - top, C_GRAPH);
        }
    }
    for (uint32_t i = 1; i < 4; i++)
        for (int32_t row = y[i] + 3; row < 56; row += 4) cv_pset(x[i], row, C_RULE);
    for (uint32_t i = 0; i < 4; i++) cv_line(x[i], y[i], x[i + 1], y[i + 1], C_BELL);
    if (forever) cv_text((int32_t)UI_WIDTH - text_w(&UI_SMALL, "FOREVER"), sustain - 13 < 0 ? 0 : sustain - 13, &UI_SMALL, "FOREVER", C_BELL, C_BG);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_env_rows(void)
{
    char v[12];
    fmt_ms(v, env_ms[snd.attack]);
    ui_row(0, "ATTACK", v, C_BELL);
    fmt_ms(v, env_ms[snd.decay]);
    ui_row(1, "DECAY", v, C_BELL);
    fmt(v, snd.sustain, "%");
    ui_row(2, "SUSTAIN", v, C_BELL);
    if (snd.release < RELEASE_FOREVER) fmt_ms(v, env_ms[snd.release]);
    ui_row(3, "RELEASE", snd.release < RELEASE_FOREVER ? v : "FOREVER", C_BELL);
}

/* --- LFO: the wave, and where each bell is on it (a third of a cycle apart) */
static void draw_lfo_pic(void)
{
    static const int16_t held[] = {18000, -11000, 29000, 7000, -28000, 13000, -3000, 24000};
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_rect(0, 30, UI_WIDTH, 1, C_RULE);
    int32_t depth = snd.lfo_depth ? snd.lfo_depth : 0;
    int32_t prev = 30;
    for (uint32_t x = 0; x < UI_WIDTH; x++) {
        uint32_t ph = x * (65536u * 2u / UI_WIDTH) % 65536u;
        int32_t s, m = (int32_t)ph - 32768, a = m < 0 ? -m : m;
        if (snd.lfo_wave == LFO_SINE) s = m * (32768 - a) >> 13;
        else if (snd.lfo_wave == LFO_TRIANGLE) s = 32767 - 2 * a;
        else if (snd.lfo_wave == LFO_RANDOM) s = held[x * 8u / UI_WIDTH];
        else { int32_t i = (int32_t)(x * 8u / UI_WIDTH), f = (int32_t)(x * 8u % UI_WIDTH) * 256 / (int32_t)UI_WIDTH;
               s = held[i] + (held[(i + 1) & 7] - held[i]) * f / 256; }
        s = s > 32767 ? 32767 : s < -32767 ? -32767 : s;
        int32_t y = 30 - s * depth * 26 / (32768 * 100);
        if (x) cv_line((int32_t)x - 1, prev, (int32_t)x, y, depth ? C_BELL : C_BELL_DIM);
        prev = y;
    }
    for (uint32_t v = 0; v < VOICES; v++) {     /* the three bells, on the first cycle */
        uint32_t ph = lfo_phase + v * 0x55555555u;
        int32_t px = (int32_t)((ph >> 16) * (UI_WIDTH / 2u) >> 16);
        cv_rect(px - 1, 55, 3, 3, v == selected ? C_BELL : C_OFF);
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_lfo_rows(void)
{
    char v[12];
    ui_row(0, "TARGET", lfo_target_names[snd.lfo_target], C_BELL);
    ui_row(1, "WAVE", lfo_wave_names[snd.lfo_wave], C_BELL);
    uint16_t h = lfo_hundredths[snd.lfo_rate];
    char *p = put_num(v, h / 100u, 0);
    *p++ = '.';
    *p++ = (char)('0' + h % 100u / 10u);
    *p++ = (char)('0' + h % 10u);
    put_str(p, " HZ");
    ui_row(2, "RATE", v, C_BELL);
    fmt(v, snd.lfo_depth, "%");
    ui_row(3, "DEPTH", v, C_BELL);
}

/* --- ARP: what it played last (height: the note), and the notes held */
static void draw_arp_pic(void)
{
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    for (uint32_t i = 0; i < 16u; i++) {
        int32_t x = (int32_t)UI_WIDTH - 13 - (int32_t)i * 13;
        uint8_t hgt = arp.history[i];
        if (i >= arp.hist_n) { cv_rect(x, 40, 10, 1, C_RULE); continue; }
        if (hgt == REST) { cv_rect(x + 4, 38, 2, 2, C_RULE); continue; }
        int32_t y = 40 - hgt * 34 / 255;
        cv_rect(x, y, 10, 3, i == 0 && held_n && arp.on ? C_BELL : C_BELL_DIM);
    }
    char b[40], *p = b;
    if (!held_n) {
        put_str(b, arp.on ? "HOLD KEYS TO PLAY" : "ARP OFF: PRESS ARP");
    } else {
        int32_t list[32];
        uint32_t n = arp_notes(list) / arp.range, shown = 0;   /* the held notes (RANGE repeats them higher) */
        for (uint32_t i = 0; i < n && shown < 6u; i++, shown++) {
            p = put_str(p, "");
            pitch_name(p, scale_step_pitch(list[i] + octave_steps()));
            while (*p) p++;
            *p++ = ' ';
            *p = 0;
        }
    }
    cv_text(0, 47, &UI_SMALL, b, held_n ? C_BELL : C_OFF, C_BG);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_arp_rows(void)
{
    char v[10];
    ui_row(0, "RATE", div_names[arp.speed], C_BELL);
    ui_row(1, "PATTERN", pattern_names[arp.pattern], C_BELL);
    fmt(v, arp.range, " OCT");
    ui_row(2, "RANGE", v, C_BELL);
    fmt(v, arp.chance, "%");
    ui_row(3, "CHANCE", v, C_BELL);
}

/* --- SEQ: the 32 steps, high notes higher; TIEs carry the note on, RESTs are dots */
static void draw_seq_pic(void)
{
    cv_begin(BOX_W, PIC_H, C_BG);
    char b[24], *p = put_str(b, "BELL ");
    *p++ = (char)('1' + seq.bell); *p = 0;
    cv_text(8, 0, &UI_SMALL, b, C_BELL, C_BG);
    uint16_t rc = seq.rec ? C_REC : C_OFF;
    p = put_str(b, seq.rec ? "REC " : "");
    put_str(p, mode_names[seq.mode]);
    cv_text((int32_t)BOX_W - 8 - text_w(&UI_SMALL, b), 0, &UI_SMALL, b, rc, C_BG);
    int32_t lo = 999, hi = -999;
    for (uint32_t i = 0; i < seq.len; i++) {
        uint8_t v = seq.steps[i];
        if (v >= TIE) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    uint32_t playing = players[PLAYER_SEQ].idx ? (players[PLAYER_SEQ].idx - 1u) % (seq.len ? seq.len : 1u) : 0;
    int32_t last_y = -1;
    for (uint32_t i = 0; i < SEQ_STEPS; i++) {
        int32_t x = 16 + (int32_t)i * 6;
        if (i >= seq.len) { cv_pset(x + 2, 52, C_RULE); last_y = -1; continue; }
        uint8_t v = seq.steps[i];
        if (v == REST) { cv_rect(x + 2, 51, 2, 2, C_OFF); last_y = -1; }
        else if (v == TIE) { if (last_y >= 0) cv_rect(x - 1, last_y + 1, 6, 1, C_BELL_DIM); }
        else {
            last_y = 47 - (hi > lo ? (v - lo) * 30 / (hi - lo) : 15);
            cv_rect(x, last_y, 5, 3, C_BELL);
        }
        if (seq.playing && !seq.rec && i == playing) cv_rect(x, 56, 5, 2, C_SEA);
    }
    if (seq.rec && seq.len < SEQ_STEPS) cv_rect(16 + (int32_t)seq.len * 6, 56, 5, 2, C_REC);
    cv_blit(BOX_X, PIC_Y);
}

static void draw_seq_rows(void)
{
    char v[12];
    ui_row(0, "MODE", mode_names[seq.mode], C_BELL);
    fmt(v, seq.len, seq.len == 1 ? " STEP" : " STEPS");
    ui_row(1, "LENGTH", seq.len ? v : "EMPTY", C_BELL);
    ui_row(2, "RATE", div_names[seq.speed], C_BELL);
    fmt(v, seq.chance, "%");
    ui_row(3, "CHANCE", v, C_BELL);
}

/* --- GLO: the scale, on a piano octave (the root in teal: the sea sits on it) */
static void draw_scale_pic(void)
{
    static const uint8_t white[] = {0, 2, 4, 5, 7, 9, 11};
    static const uint8_t black[] = {1, 3, 6, 8, 10};
    static const uint8_t black_x[] = {30, 60, 120, 150, 180};
    uint8_t root = scale_root_pc();
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_text(0, 0, &UI_BODY, scales[scale_now.scale].name, C_BELL, C_BG);
    for (uint32_t i = 0; i < 7; i++) {
        uint16_t c = scale_contains(white[i]) ? (white[i] == root ? C_SEA : C_BELL) : C_GRAPH;
        cv_rect(i * 30u, 20, 28, 40, C_RULE);
        cv_rect(i * 30u + 1, 21, 26, 38, c);
    }
    for (uint32_t i = 0; i < 5; i++) {
        int32_t x = black_x[i] - 10;
        cv_rect(x, 20, 20, 24, C_RULE);
        cv_rect(x + 1, 20, 18, 23, C_BG);
        uint16_t c = scale_contains(black[i]) ? (black[i] == root ? C_SEA : C_BELL) : C_BG;
        cv_rect(x + 2, 21, 16, 21, c);
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_glo_rows(void)
{
    char v[16];
    fmt(v, scale_now.size, scale_now.size == 1 ? " NOTE" : " NOTES");
    ui_row(0, "SCALE", v, C_BELL);
    uint32_t bpm = external_clock() ? external_bpm_value : glo.bpm;
    fmt(v, (int32_t)bpm, external_clock() ? " MIDI" : " BPM");
    ui_row(1, "TEMPO", v, C_KEY);
    fmt_ms(v, (uint32_t)glide * glide * 4u / 5u);
    ui_row(2, "GLIDE", glide ? v : "OFF", C_BELL);
    fmt(v, volume, "%");
    ui_row(3, "VOLUME", v, C_KEY);
}

/* ------------------------------------------------------------------------------------- knobs and keys */
static int32_t step_clamp(int32_t v, int32_t d, int32_t lo, int32_t hi)
{
    v += d;
    return v < lo ? lo : v > hi ? hi : v;
}

static uint8_t pct(uint8_t v, int32_t d) { return (uint8_t)step_clamp(v, d * 2, 0, 100); }

static void edit_page(uint8_t page, const int32_t s[4])
{
    switch (page) {
    case P_HOME:
        snd.sea = pct(snd.sea, s[0]); snd.bell = pct(snd.bell, s[1]);
        snd.reverb = pct(snd.reverb, s[2]); snd.echo = pct(snd.echo, s[3]);
        break;
    case P_EDIT:
        snd.wave = (uint8_t)step_clamp(snd.wave, s[0], 0, WAVES - 1);
        snd.detune = (uint8_t)step_clamp(snd.detune, s[1], 0, 50);
        snd.tone = pct(snd.tone, s[2]); snd.push = pct(snd.push, s[3]);
        break;
    case P_FX:
        snd.ring = (uint8_t)step_clamp(snd.ring, s[0], 0, 100);
        snd.color = pct(snd.color, s[1]);
        if (s[2]) { snd.spread = (uint8_t)step_clamp(snd.spread, s[2], 0, SPREADS - 1); bells_retune(); }
        snd.grit = pct(snd.grit, s[3]);
        break;
    case P_ENV:
        snd.attack = (uint8_t)step_clamp(snd.attack, s[0], 0, ENV_STEPS - 1);
        snd.decay = (uint8_t)step_clamp(snd.decay, s[1], 0, ENV_STEPS - 1);
        snd.sustain = (uint8_t)step_clamp(snd.sustain, s[2] * 5, 0, 100);
        snd.release = (uint8_t)step_clamp(snd.release, s[3], 0, RELEASE_FOREVER);
        break;
    case P_LFO:
        snd.lfo_target = (uint8_t)step_clamp(snd.lfo_target, s[0], 0, LFO_TARGETS - 1);
        snd.lfo_wave = (uint8_t)step_clamp(snd.lfo_wave, s[1], 0, LFO_WAVES - 1);
        snd.lfo_rate = (uint8_t)step_clamp(snd.lfo_rate, s[2], 0, LFO_RATES - 1);
        snd.lfo_depth = pct(snd.lfo_depth, s[3]);
        break;
    case P_ARP:
        if (s[0]) {
            arp.speed = (uint8_t)step_clamp(arp.speed, s[0], 0, DIVS - 1);
            player_restart(PLAYER_ARP, players[PLAYER_ARP].next);
            if (seq.rec && seq.mode == MODE_ARP) { seq.speed = arp.speed; player_restart(PLAYER_SEQ, players[PLAYER_ARP].next); }
        }
        arp.pattern = (uint8_t)step_clamp(arp.pattern, s[1], 0, PATTERNS - 1);
        arp.range = (uint8_t)step_clamp(arp.range, s[2], 1, 4);
        arp.chance = (uint8_t)step_clamp(arp.chance, s[3] * 5, 0, 100);
        break;
    case P_SEQ:
        if (!seq.rec) {
            seq.mode = (uint8_t)step_clamp(seq.mode, s[0], 0, MODES - 1);
            if (s[1]) {                         /* longer: silent steps (RESTs) are added; shorter, then longer again: they come back */
                seq.len = (uint8_t)step_clamp(seq.len, s[1], 1, SEQ_STEPS);
                seq_version++;
            }
        }
        if (s[2] && !(seq.rec && seq.mode == MODE_ARP)) {
            seq.speed = (uint8_t)step_clamp(seq.speed, s[2], 0, DIVS - 1);
            player_restart(PLAYER_SEQ, players[PLAYER_SEQ].next);
        }
        seq.chance = (uint8_t)step_clamp(seq.chance, s[3] * 5, 0, 100);
        break;
    case P_GLO:
        if (s[0]) { scale_select((uint8_t)step_clamp(scale_now.scale, s[0], 0, SCALE_COUNT - 1)); bells_retune(); }
        glo.bpm = (uint8_t)step_clamp(glo.bpm, s[1], 20, 240);
        glide = (uint8_t)step_clamp(glide, s[2] * 2, 0, 100);
        volume = (uint8_t)step_clamp(volume, s[3] * 2, 0, 100);
        break;
    }
}

static void update_fx(void)
{
    static uint32_t previous = ~0u;
    uint32_t bpm = external_clock() && external_bpm_value >= 30 && external_bpm_value <= 300 ? external_bpm_value : glo.bpm;
    uint32_t state = bpm | (uint32_t)snd.echo << 12 | (uint32_t)snd.reverb << 20;
    if (state == previous) return;
    previous = state;
    delay_configure(snd.echo != 0, delay_tempo_samples(bpm, ECHO_CLOCKS), ECHO_FEEDBACK, snd.echo);
    hall_configure(snd.reverb != 0, HALL_DECAY, HALL_DAMPING, snd.reverb);
    chorus_amount = 0;
}

static uint8_t button_page(uint32_t button)
{
    switch (button) {
    case BTN_HOME: return P_HOME;
    case BTN_ARP: return P_ARP;
    case BTN_SEQ: return P_SEQ;
    case BTN_EDIT: return P_EDIT;
    case BTN_ENV: return P_ENV;
    case BTN_LFO: return P_LFO;
    case BTN_FX: return P_FX;
    case BTN_GLO: return P_GLO;
    default: return 0xFF;
    }
}

static const char *page_title(uint8_t page, char *b)
{
    switch (page) {
    case P_HOME: {
        if (!voices[0].frozen && !voices[1].frozen && !voices[2].frozen) return "HOME";
        char *p = put_str(b, "HOME  HOLD");
        for (uint32_t v = 0; v < VOICES; v++) if (voices[v].frozen) { *p++ = ' '; *p++ = (char)('1' + v); }
        *p = 0;
        return b;
    }
    case P_ARP: return arp.on ? "ARP  ON" : "ARP  OFF";
    case P_SEQ: return seq.rec ? "SEQ  REC" : seq.playing ? "SEQ  PLAY" : "SEQ";
    case P_EDIT: return "EDIT  SEA";
    case P_ENV: return "ENV";
    case P_LFO: return "LFO";
    case P_FX: put_str(put_str(b, "FX  "), engine_names[snd.engine]); return b;
    default: return "GLO  SCALE";
    }
}

static uint16_t title_colour(uint8_t page)
{
    uint8_t f = page_focus(page);
    return f == FOCUS_SEA ? C_SEA : f == FOCUS_NONE ? C_KEY : C_BELL;
}

static const char *message;
static uint32_t message_at;
static void say(const char *text) { message = text; message_at = fm1_ms; }

static void draw_footer(uint8_t page)
{
    static char b[40];
    if (message) { ui_footer(message, C_BELL); return; }
    switch (page) {
    case P_HOME: {
        char *p = put_str(b, voices[selected].frozen ? "SEL: LET BELL " : "SEL: HOLD BELL ");
        *p++ = (char)('1' + selected);
        put_str(p, voices[selected].frozen ? " GO" : "");
        ui_footer(b, C_OFF);
        break;
    }
    case P_EDIT: ui_footer("THE SEA NEVER STOPS", C_OFF); break;
    case P_FX: ui_footer("THE BELL SETS THE PITCH", C_OFF); break;
    case P_ENV: ui_footer("HOW MUCH SEA REACHES THE BELL", C_OFF); break;
    case P_LFO: ui_footer("3 BELLS, A THIRD APART", C_OFF); break;
    case P_ARP: ui_footer(arp.on ? "HOLD KEYS: THE ARP PLAYS THEM" : "ARP: TURN THE ARP ON", C_OFF); break;
    case P_SEQ:
        if (seq.rec == 1) {
            ui_footer(seq.mode == MODE_ARP ? "REC ARP: HOLD KEYS TO START" : "REC LIVE: PLAY TO START", C_REC);
        } else if (seq.rec && seq.mode == MODE_STEP) {
            char *p = put_num(b, seq.len + 1, 0);
            put_str(p, "/32  F#3: REST  G#3: TIE");
            ui_footer(b, C_REC);
        } else if (seq.rec) {
            char *p = put_str(b, "REC ");
            p = put_str(p, mode_names[seq.mode]);
            *p++ = ' ';
            p = put_num(p, seq.len + 1, 0);
            put_str(p, "/32   REC: STOP");
            ui_footer(b, C_REC);
        } else {
            ui_footer(seq.len ? "REC: NEW   HOLD REC: CLEAR" : "REC: RECORD   SELECT: BELL", C_OFF);
        }
        break;
    default: {
        char *p = put_str(b, "KEYS: TRANSPOSE   ROOT ");
        put_str(p, note_names[scale_root_pc()]);
        ui_footer(b, C_OFF);
    }
    }
}

/* everything a page shows that can change, folded into one number: redraw when it changes */
#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
static uint32_t page_state(uint8_t page, uint32_t now)
{
    uint32_t h = 2166136261u;
    MIX(selected);
    switch (page) {
    case P_HOME:
        MIX(now / 80u);
        for (uint32_t v = 0; v < VOICES; v++) { MIX(voices[v].peak > 300); MIX(voices[v].peak > 2500); MIX(voices[v].peak > 9000); MIX((voices[v].shown + 128) >> 8); MIX(voices[v].frozen); MIX(voices[v].awake); }
        break;
    case P_EDIT: MIX(snd.wave); MIX(snd.detune); MIX(snd.push); break;
    case P_FX:
        MIX(now / 60u); MIX(snd.engine); MIX(snd.color); MIX(snd.spread); MIX(sea_pitch); MIX(snd.wave == WAVE_NOISE);
        break;
    case P_ENV: MIX(snd.attack); MIX(snd.decay); MIX(snd.sustain); MIX(snd.release); break;
    case P_LFO: MIX(snd.lfo_wave); MIX(snd.lfo_depth); MIX(lfo_phase >> 26); break;
    case P_ARP:
        MIX(arp.hist_n); MIX(arp.history[0]); MIX(arp.history[1]); MIX(players[PLAYER_ARP].idx); MIX(arp.on); MIX(held_n);
        for (uint32_t i = 0; i < held_n; i++) MIX(held_notes[i]);
        MIX(scale_now.octave); MIX(scale_now.scale); MIX(scale_now.transpose);
        break;
    case P_SEQ: MIX(seq_version); MIX(seq.rec); MIX(seq.playing); MIX(seq.bell); MIX(seq.mode); MIX(seq.playing ? players[PLAYER_SEQ].idx : 0); break;
    default: MIX(scale_now.scale); MIX(scale_now.transpose); break;
    }
    return h;
}

static uint32_t row_state(uint8_t page)
{
    uint32_t h = 2166136261u;
    switch (page) {
    case P_HOME: MIX(snd.sea); MIX(snd.bell); MIX(snd.reverb); MIX(snd.echo); break;
    case P_EDIT: MIX(snd.wave); MIX(snd.detune); MIX(snd.tone); MIX(snd.push); break;
    case P_FX: MIX(snd.ring); MIX(snd.color); MIX(snd.spread); MIX(snd.grit); break;
    case P_ENV: MIX(snd.attack); MIX(snd.decay); MIX(snd.sustain); MIX(snd.release); break;
    case P_LFO: MIX(snd.lfo_target); MIX(snd.lfo_wave); MIX(snd.lfo_rate); MIX(snd.lfo_depth); break;
    case P_ARP: MIX(arp.speed); MIX(arp.pattern); MIX(arp.range); MIX(arp.chance); break;
    case P_SEQ: MIX(seq.mode); MIX(seq.len); MIX(seq.speed); MIX(seq.chance); break;
    default: MIX(scale_now.scale); MIX(glo.bpm); MIX(glide); MIX(volume); MIX(external_clock() ? external_bpm_value : 0); break;
    }
    return h;
}

static uint32_t chain_state(void)
{
    uint32_t h = 2166136261u;
    MIX(selected); MIX(sea_pitch); MIX((voices[selected].shown + 128) >> 8);
    MIX(voices[selected].awake || voices[selected].frozen);
    return h;
}
#undef MIX

static void draw_page(uint8_t page, uint8_t whole, uint32_t now)
{
    switch (page) {
    case P_HOME: draw_harbour(now); if (whole) draw_home_rows(); break;
    case P_EDIT: draw_sea_pic(); if (whole) draw_edit_rows(); break;
    case P_FX: draw_strip(); if (whole) draw_fx_rows(); break;
    case P_ENV: draw_env_pic(); if (whole) draw_env_rows(); break;
    case P_LFO: draw_lfo_pic(); if (whole) draw_lfo_rows(); break;
    case P_ARP: draw_arp_pic(); if (whole) draw_arp_rows(); break;
    case P_SEQ: draw_seq_pic(); if (whole) draw_seq_rows(); break;
    default: draw_scale_pic(); if (whole) draw_glo_rows(); break;
    }
}

/* a new seed every power-on (the online emulator repeats its random numbers: a counter in flash helps there) */
static uint32_t boot_seed(void)
{
    uint32_t seed = fm1_random_word();
    if (seed != 0 && seed != 0x0bf34dadu) return seed;
    uint32_t saved = 0;
    blob_load(NBLOB - 1u, &saved, sizeof saved);
    saved += 0x9e3779b9u;
    blob_save(NBLOB - 1u, &saved, sizeof saved);
    return saved ^ seed;
}

/* ----------------------------------------------------------------------------------------------- the loop */
static void app_main(void)
{
    uint32_t prev_keys = 0;
    uint32_t held_since[14] = {0};
    uint16_t long_done = 0, was_on_page = 0;
    uint8_t page = P_HOME, drawn_page = 0xFF, key_note[27] = {0};
    uint32_t drawn_state = 0, drawn_rows = 0, drawn_footer = ~0u, title_state = ~0u, drawn_chain = ~0u;
    static char msg[32];
#if !BOARD_SMK37
    uint8_t battery_drawn = UINT8_MAX;
#endif

    bell_init();
    presets_init();
    seq_clear();
    players[PLAYER_SEQ].bell = players[PLAYER_ARP].bell = -1;
    uint32_t seed = boot_seed();
    arp.rnd = seed | 1u;
    scale_select((uint8_t)(1u + (seed >> 8) % (SCALE_COUNT - 3u)));   /* a random scale at power-on, on C (not FIFTHS, OCTAVES) */
    preset_load(0);

    draw_boot_logo();
    uint32_t boot_started = fm1_ms;
    clk.last_ms = fm1_ms;
    for (;;) {
        platform_poll();
        uint32_t now = fm1_ms, rel;
        uint32_t b = fm1_input_edges(&rel);
        uint32_t held = fm1_in.buttons;

        /* ---- buttons: pages on press; hold actions; taps on release */
        for (uint32_t i = 0; i < 14; i++) {
            uint32_t bit = 1u << i;
            if (b & bit) {
                held_since[i] = now;
                long_done &= (uint16_t)~bit;
                uint8_t to = button_page(i);
                if (to != 0xFF) {
                    if (page == to) was_on_page |= (uint16_t)bit; else was_on_page &= (uint16_t)~bit;
                    page = to;
                }
            }
            if ((held & bit) && !(long_done & bit) && now - held_since[i] >= LONG_MS && !(b & bit)) {
                long_done |= (uint16_t)bit;
                if (i == BTN_REC) {             /* hold REC: clear the SEQ */
                    seq.rec = 0;
                    seq_stop();
                    seq_clear();
                    say("SEQ CLEARED");
                }
                if (i == BTN_PLAY || i == BTN_SEL) {   /* hold PLAY/STOP or SEL: every bell lets go, held ones too */
                    if (i == BTN_PLAY) seq_stop();
                    live_all_up();
                    bell_silence();
                    for (uint32_t v = 0; v < VOICES; v++) if (bell_midi[v]) { midi_send(0x80u | v, bell_midi[v], 0); bell_midi[v] = 0; }
                    say(i == BTN_SEL ? "NO BELL HELD" : "ALL BELLS RELEASED");
                }
            }
            if ((rel & bit) && !(long_done & bit)) {
                if (i == BTN_ARP && (was_on_page & bit)) {
                    live_all_up();
                    arp.on = !arp.on;
                    held_n = 0;
                    if (seq.rec && seq.mode == MODE_ARP && !arp.on) rec_end();
                }
                if (i == BTN_PLAY) {
                    if (seq.playing || seq.rec) seq_stop(); else if (seq.len) seq_start(); else say("THE SEQ IS EMPTY: REC");
                }
                if (i == BTN_REC) {
                    if (seq.rec) rec_end();
                    else {
                        if (seq.mode == MODE_ARP && !arp.on) { live_all_up(); held_n = 0; arp.on = 1; }
                        rec_begin();
                        page = P_SEQ;
                    }
                }
                /* SEL: HOLD the selected bell (or let it go). Any of the three, in any combination. */
                if (i == BTN_SEL) {
                    voice_t *x = &voices[selected];
                    char *p = put_str(msg, "BELL ");
                    *p++ = (char)('1' + selected);
                    if (x->frozen) {
                        bell_hold(selected, 0);
                        bell_off(selected);
                        put_str(p, " LET GO");
                    } else if (bell_hold(selected, 1)) {
                        for (uint32_t q = 0; q < PLAYERS; q++)
                            if (players[q].bell == (int8_t)selected) players[q].bell = -1;   /* (the bell is held now: theirs no more) */
                        if (held_bell == (int32_t)selected) held_bell = -1;
                        put_str(p, " HELD");
                    } else {
                        put_str(p, " IS SILENT");
                    }
                    say(msg);
                }
            }
        }
        /* SAVE: the sound into a USER preset */
        if (b & (1u << BTN_SAVE)) {
            uint8_t slot = preset >= FACTORY ? preset : last_user;
            store.user[slot - FACTORY] = snd;
            store.magic = PRESET_MAGIC;
            int ok = blob_save(PRESET_BLOB, &store, sizeof store) >= 0;
            preset = last_user = slot;
            char *p = put_str(msg, ok ? "SAVED TO " : "COULD NOT SAVE ");
            preset_name(slot, p);
            say(msg);
        }
        if (b & (1u << BTN_OCT_DOWN) || b & (1u << BTN_OCT_UP)) {
            int32_t d = b & (1u << BTN_OCT_UP) ? 1 : -1;
            if (page == P_GLO) {                /* GLO: the whole instrument, an octave */
                scale_now.transpose = (int8_t)step_clamp(scale_now.transpose, d * 12, -31, 43);
                bells_retune();
            } else {
                scale_now.octave = (int8_t)step_clamp(scale_now.octave, d, -2, 2);
            }
        }

        /* ---- encoders: SELECT the bell (GLO: the scale; SEQ: the SEQ's bell); ALGORITHM the effect; PRESETS */
        int32_t e = fm1_enc_take(ENC_SELECT);
        if (e) {
            if (page == P_GLO) {
                scale_select((uint8_t)step_clamp(scale_now.scale, e, 0, SCALE_COUNT - 1));
                bells_retune();
            } else if (page == P_SEQ) {
                uint8_t next = (uint8_t)step_clamp(seq.bell, e, 0, VOICES - 1);
                if (next != seq.bell) { note_off(PLAYER_SEQ); seq.bell = next; }
            } else {
                uint8_t next = (uint8_t)step_clamp(selected, e, 0, VOICES - 1);
                if (next != selected) { live_all_up(); selected = next; }
            }
        }
        e = fm1_enc_take(ENC_ALGORITHM);
        if (e) {
            snd.engine = (uint8_t)step_clamp(snd.engine, e, 0, ENGINES - 1);
            put_str(put_str(msg, "BELLS: "), engine_names[snd.engine]);
            say(msg);
        }
        e = fm1_enc_take(ENC_PRESETS);
        if (e) {
            preset_load((uint8_t)step_clamp(preset, e, 0, FACTORY + USERS - 1));
            char *p = put_str(msg, "PRESET ");
            preset_name(preset, p);
            say(msg);
        }
        int32_t steps[4];
        for (uint32_t k = 0; k < 4; k++) steps[k] = fm1_enc_take(ENC_KNOB1 + k);
        if (steps[0] || steps[1] || steps[2] || steps[3]) edit_page(page, steps);

        /* ---- the keys: they play (or feed the ARP) on every page but GLO, where they transpose everything */
        uint32_t keys = fm1_in.notes, down = keys & ~prev_keys, up = prev_keys & ~keys;
        prev_keys = keys;
        uint8_t stepping = seq.rec && seq.mode == MODE_STEP;
        for (uint32_t k = 0; k < 27u; k++) {
            uint32_t bit = 1u << k;
            if (down & bit) {
                key_note[k] = 0;
                if (page == P_GLO) {
                    scale_now.transpose = (int8_t)((int32_t)k - 7);   /* C4 (the 8th key) = none */
                    bells_retune();
                } else if (stepping && (k == KEY_REST || k == KEY_TIE)) {
                    seq_write(k == KEY_REST ? REST : TIE);
                } else {
                    key_note[k] = (uint8_t)(KEY0 + k);
                    live_press(key_note[k]);
                    if (stepping) seq_write((uint8_t)(key_step(key_note[k]) + STEP_BIAS));
                }
            }
            if ((up & bit) && key_note[k]) {
                live_lift(key_note[k]);
                key_note[k] = 0;
            }
        }

        midi_in();
        clock_service();
        update_fx();
        if (message && now - message_at > 1500u) message = 0;

        /* ---- LEDs */
        uint32_t lit = keys;
        if (page == P_GLO) {
            int32_t k = scale_now.transpose + 7;
            while (k > 26) k -= 12;
            while (k < 0) k += 12;
            lit = 1u << k;
        } else if (stepping) {
            lit |= 1u << KEY_REST | 1u << KEY_TIE;
        }
        for (uint32_t k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), lit >> k & 1u);
        uint8_t any_held = voices[0].frozen || voices[1].frozen || voices[2].frozen;
        fm1_led_key(BTN_OCT_DOWN, scale_now.octave < 0);
        fm1_led_key(BTN_OCT_UP, scale_now.octave > 0);
        fm1_led_key(BTN_PLAY, seq.playing && (clk.tick / 48u) % 2u == 0);
        fm1_led_key(BTN_REC, seq.rec && (now / 200u) % 2u == 0);
        fm1_led_key(BTN_ARP, arp.on);
        fm1_led_key(BTN_SEQ, page == P_SEQ);
        fm1_led_key(BTN_HOME, page == P_HOME);
        fm1_led_key(BTN_SEL, any_held);
        fm1_led_key(BTN_EDIT, page == P_EDIT);
        fm1_led_key(BTN_ENV, page == P_ENV);
        fm1_led_key(BTN_LFO, page == P_LFO);
        fm1_led_key(BTN_FX, page == P_FX);
        fm1_led_key(BTN_GLO, page == P_GLO);
        fm1_led_key(BTN_SAVE, message && message == msg && now - message_at < 1500u && preset >= FACTORY);

        /* ---- the screen */
        if (drawn_page == 0xFF && now - boot_started < BOOT_LOGO_MS && !b && !keys) continue;
        idle_ok = 1;
        uint8_t whole = page != drawn_page;
        if (whole) {
            lcd_fill(0, 0, 240, 240, C_BG);
            ui_divider(40, C_RULE);
            ui_divider(211, C_RULE);
            drawn_page = page;
            drawn_footer = drawn_chain = title_state = ~0u;
#if !BOARD_SMK37
            battery_drawn = UINT8_MAX - 1u;
#endif
        }
        uint32_t ts = (uint32_t)page | (uint32_t)arp.on << 4 | (uint32_t)snd.engine << 5 | (uint32_t)seq.rec << 8 | (uint32_t)seq.playing << 10 |
                      (uint32_t)voices[0].frozen << 12 | (uint32_t)voices[1].frozen << 13 | (uint32_t)voices[2].frozen << 14;
        if (ts != title_state) {
            char tb[24];
            lcd_fill(UI_LEFT, 10, UI_WIDTH - UI_BATTERY_WIDTH - 4u, 26, C_BG);
            ui_text(UI_LEFT, 13, UI_WIDTH - UI_BATTERY_WIDTH - 8u, &UI_LARGE, page_title(page, tb), title_colour(page), 0);
            title_state = ts;
        }
        uint32_t cs = chain_state() ^ page_focus(page) * 0x9e3779b9u;
        if (cs != drawn_chain) { draw_chain(page_focus(page)); drawn_chain = cs; }
        uint32_t ps = page_state(page, now), rs = row_state(page);
        if (whole || ps != drawn_state || rs != drawn_rows) {
            draw_page(page, whole || rs != drawn_rows, now);
            drawn_state = ps;
            drawn_rows = rs;
        }
        uint32_t fs = (uint32_t)(uintptr_t)message ^ message_at * 31u ^ (uint32_t)voices[selected].frozen << 3 ^
                      (uint32_t)selected << 4 ^ (uint32_t)scale_root_pc() << 6 ^ (uint32_t)seq.rec << 10 ^
                      (uint32_t)seq.len << 12 ^ (uint32_t)seq.mode << 18 ^ (uint32_t)arp.on << 20 ^ (uint32_t)(seq.len != 0) << 21;
        if (fs != drawn_footer) { draw_footer(page); drawn_footer = fs; }
#if !BOARD_SMK37
        uint8_t battery_level = read_battery_level(now);
        if (battery_drawn != battery_level) { draw_battery(battery_level); battery_drawn = battery_level; }
#endif
        idle_ok = 0;
    }
}
