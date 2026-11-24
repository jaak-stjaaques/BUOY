/* SPDX-License-Identifier: GPL-3.0-only */
/* BUOY's sound: the SEA and three BELLS.
 *
 *   SEA (EDIT)  one or two oscillators that never stop, always on the scale's root (low, C2..B3)
 *      |
 *      +--> BELL 1, 2, 3 (FX, ALGORITHM)   the effect that turns the sea into notes:
 *      |        RESONATOR  a tuned delay (a comb): it rings at the note's pitch
 *      |        SHIFTER    two grains read from a short memory of the sea, faster or slower: the sea moved to the note
 *      |        PRISM      narrow band-pass filters on the note and its overtones
 *      |    ENV opens and closes how much sea each bell gets (the volume); RING is how long a bell sings after that
 *      |
 *      +--> (dry, HOME's SEA)
 *
 * The notes never touch the oscillators: they only retune the effects. Every number is an integer (no FPU). */
#include "buoy_tables.h"

#define VOICES 3u
#define VOICE_BUF 4096u                         /* per bell: the comb's two delays, or the shifter's memory */
#define COMB_LEN 2048u
#define Q15 32768

enum { ENG_RESONATOR, ENG_SHIFTER, ENG_PRISM, ENGINES };
enum { WAVE_SINE, WAVE_TRIANGLE, WAVE_SAW, WAVE_SQUARE, WAVE_GLASS, WAVE_REED, WAVE_VOICE, WAVE_ORGAN, WAVE_NOISE,
       WAVES };
enum { SPREAD_OFF, SPREAD_OCT_UP, SPREAD_5TH, SPREAD_3RD, SPREAD_OCT_DOWN, SPREADS };
enum { LFO_PITCH, LFO_VOLUME, LFO_RING, LFO_COLOR, LFO_GRIT, LFO_PUSH, LFO_TARGETS };
enum { LFO_SINE, LFO_TRIANGLE, LFO_RANDOM, LFO_DRIFT, LFO_WAVES };
enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
#define RELEASE_FOREVER ENV_STEPS               /* the last step of RELEASE: the bell never stops being fed */

static const char *const engine_names[ENGINES] = {"RESONATOR", "SHIFTER", "PRISM"};
static const char *const engine_short[ENGINES] = {"RES", "SHIFT", "PRISM"};
static const char *const wave_names[WAVES] = {"SINE", "TRIANGLE", "SAW", "SQUARE", "GLASS", "REED", "VOICE",
                                              "ORGAN", "NOISE"};
static const char *const spread_names[SPREADS] = {"OFF", "OCTAVE UP", "SCALE 5TH", "SCALE 3RD", "OCTAVE DOWN"};
static const char *const lfo_target_names[LFO_TARGETS] = {"PITCH", "VOLUME", "RING", "COLOR", "GRIT", "PUSH"};
static const char *const lfo_wave_names[LFO_WAVES] = {"SINE", "TRIANGLE", "RANDOM", "DRIFT"};

/* a sound: everything PRESETS loads and SAVE keeps */
typedef struct {
    uint8_t engine, wave, detune, tone, push;   /* the sea: wave, osc 2 detune (cents), tone and push (%) */
    uint8_t ring, color, spread, grit;          /* the bells */
    uint8_t attack, decay, sustain, release;    /* indexes into env_ms[]; sustain in %; release ENV_STEPS = forever */
    uint8_t lfo_target, lfo_wave, lfo_rate, lfo_depth;
    uint8_t sea, bell, reverb, echo;            /* HOME's mix, % */
} sound_t;

static sound_t snd;
static volatile uint8_t glide = 20, volume = 80; /* GLO */
static volatile int32_t sea_pitch = 48 * 256;   /* the sea's note, in 1/256 semitones */

typedef struct {
    volatile int32_t target, partner;           /* the note and its SPREAD partner (offset), 1/256 semitones */
    volatile uint8_t gate, retrig, frozen;
    int32_t pitch;                              /* where the glide is now */
    int32_t env;                                /* 0 .. 1 << 24 */
    uint8_t stage, awake, quiet, engine;
    uint32_t w, ph[2], noise, lfo_random[2];
    int32_t lp[2], fb, dc_x, dc_y, gain;
    int32_t svf_lp[5], svf_bp[5];
    volatile int32_t shown;                     /* the note the screen shows (pitch + LFO), 1/256 semitones */
    volatile uint16_t peak;                     /* this block's loudest sample, for the screen */
} voice_t;

static voice_t voices[VOICES];
#ifdef DINGHY_HOST
static int16_t voice_buf[VOICES][VOICE_BUF];
#else
static int16_t voice_buf[VOICES][VOICE_BUF] __attribute__((section(".pool")));
#endif

/* ------------------------------------------------------------------------------------------- pitch helpers */
static uint32_t inc_of(int32_t p)               /* 1/256 semitones -> an oscillator's phase step (2^32 a cycle) */
{
    if (p < 12 * 256) p = 12 * 256;
    if (p > 118 * 256) p = 118 * 256;
    uint32_t n = (uint32_t)p >> 8, f = (uint32_t)p & 255u;
    uint32_t a = inc_c9[n % 12u] >> (10u - n / 12u);
    uint32_t m = n + 1u, b = inc_c9[m % 12u] >> (10u - m / 12u);
    return a + (uint32_t)(((uint64_t)(b - a) * f) >> 8);
}

static uint32_t period_of(int32_t p)            /* 1/256 semitones -> one cycle in samples, Q8 */
{
    if (p < 24 * 256) p = 24 * 256;
    if (p > 108 * 256) p = 108 * 256;
    uint32_t n = (uint32_t)p >> 8, f = (uint32_t)p & 255u;
    uint32_t a = (period_c9_x64[n % 12u] << (10u - n / 12u)) >> 6;
    uint32_t m = n + 1u, b = (period_c9_x64[m % 12u] << (10u - m / 12u)) >> 6;
    return a - (uint32_t)(((uint64_t)(a - b) * f) >> 8);
}

static uint32_t ratio_q16(int32_t semis)        /* 2^(semis/12), semis in 1/256: the shifter's speed, Q16 */
{
    if (semis < -48 * 256) semis = -48 * 256;
    if (semis > 47 * 256) semis = 47 * 256;
    int32_t whole = semis >> 8, frac = semis & 255;  /* (>> rounds down, so frac is 0..255) */
    int32_t octave = whole >= 0 ? whole / 12 : -((-whole + 11) / 12);
    int32_t k = whole - octave * 12;
    uint32_t a = semitone_q16[k], b = semitone_q16[k + 1];
    uint32_t r = a + (((b - a) * (uint32_t)frac) >> 8);
    return octave >= 0 ? r << octave : r >> -octave;
}

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static inline int32_t soft(int32_t x)           /* a soft ceiling near 31000 */
{
    int32_t m = x < 0 ? -x : x;
    if (m <= 22000) return x;
    m = 22000 + 9000 * (m - 22000) / (9000 + (m - 22000));
    return x < 0 ? -m : m;
}

static inline int32_t white(uint32_t *s)
{
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return (int32_t)(*s >> 16) - 32768;
}

/* -------------------------------------------------------------------------------------------- the commands
 * The main loop plays the bells with these; the audio interrupt reads them once a block. */
static void bell_play(uint32_t v, int32_t pitch, int32_t partner)
{
    voice_t *x = &voices[v];
    if (x->frozen) return;
    x->partner = partner;
    x->target = pitch;
    x->gate = 1;
    x->retrig = 1;
}

static void bell_release(uint32_t v)
{
    if (!voices[v].frozen) voices[v].gate = 0;
}

static void bell_freeze(uint8_t on)             /* SEL: every bell that sounds now is held as it is */
{
    for (uint32_t v = 0; v < VOICES; v++) {
        voice_t *x = &voices[v];
        if (on) x->frozen = x->awake && (x->gate || x->env > (1 << 18));
        else { x->frozen = 0; }
    }
}

static void bell_silence(void)                  /* PLAY/STOP while stopped: let every bell go */
{
    for (uint32_t v = 0; v < VOICES; v++) { voices[v].frozen = 0; voices[v].gate = 0; }
}

/* ------------------------------------------------------------------------------------------------ the LFO */
static uint32_t lfo_phase;
static int32_t lfo_value(uint32_t v)           /* -32767 .. 32767; each bell a third of a cycle later */
{
    uint32_t ph = lfo_phase + v * 0x55555555u;
    int32_t x = (int32_t)(ph >> 16) - 32768;   /* a saw, -32768 .. 32767 */
    int32_t m = x < 0 ? -x : x;
    voice_t *vo = &voices[v];
    switch (snd.lfo_wave) {
    case LFO_SINE: { int32_t s = (int32_t)((int64_t)x * (32768 - m) >> 13); return s > 32767 ? 32767 : s < -32767 ? -32767 : s; }
    case LFO_TRIANGLE: { int32_t t = 32767 - 2 * m; return t < -32767 ? -32767 : t; }
    case LFO_RANDOM: return (int32_t)(vo->lfo_random[1] >> 16) - 32768;
    default: {                                  /* DRIFT: from one random value to the next, smoothly */
        int32_t a = (int32_t)(vo->lfo_random[0] >> 16) - 32768, b = (int32_t)(vo->lfo_random[1] >> 16) - 32768;
        uint32_t t = (ph >> 17);                /* 0 .. 32767 */
        uint32_t s = (t * t >> 15) * (3u * 32768u - 2u * t) >> 15;   /* smoothstep */
        return a + (int32_t)(((int64_t)(b - a) * (int32_t)s) >> 15);
    }
    }
}

static void lfo_advance(uint32_t frames)
{
    uint32_t before = lfo_phase;
    lfo_phase += lfo_inc[snd.lfo_rate < LFO_RATES ? snd.lfo_rate : LFO_RATES - 1u] * frames;
    for (uint32_t v = 0; v < VOICES; v++) {     /* a new random value as each bell's own cycle turns over */
        uint32_t off = v * 0x55555555u;
        if (lfo_phase + off < before + off) {
            voices[v].lfo_random[0] = voices[v].lfo_random[1];
            voices[v].lfo_random[1] = voices[v].lfo_random[1] * 1664525u + 1013904223u + v * 7919u;
        }
    }
}

static int32_t clamp100(int32_t x) { return x < 0 ? 0 : x > 100 ? 100 : x; }

/* ------------------------------------------------------------------------------------------------ the sea */
static uint32_t sea_ph[2], sea_noise = 0x2545f491u;
static int32_t sea_lp, sea_pink[3];
static int32_t sea_buf[HALF_FRAMES];

static void sea_render(uint32_t frames)
{
    uint32_t inc = inc_of(sea_pitch);
    uint32_t inc2 = inc + (uint32_t)((uint64_t)inc * snd.detune * 3786u / 6553600u);   /* + cents */
    int32_t k = 1500 + snd.tone * 312;          /* the tone: a gentle low-pass, open at 100 */
    uint8_t wave = snd.wave, two = snd.detune != 0;
    const int16_t *t = sea_tables[wave < SEA_TABLES ? wave : 0];
    for (uint32_t i = 0; i < frames; i++) {
        int32_t s;
        if (wave == WAVE_NOISE) {
            int32_t n = white(&sea_noise);
            sea_pink[0] += (n - sea_pink[0]) >> 1;
            sea_pink[1] += (n - sea_pink[1]) >> 3;
            sea_pink[2] += (n - sea_pink[2]) >> 5;
            s = (n + 2 * sea_pink[0] + 3 * sea_pink[1] + 5 * sea_pink[2]) / 6;
        } else {
            uint32_t p = sea_ph[0], f = (p >> 16) & 255u;
            int32_t a = t[p >> 24], b = t[((p >> 24) + 1u) & 255u];
            s = a + (((b - a) * (int32_t)f) >> 8);
            sea_ph[0] = p + inc;
            if (two) {
                p = sea_ph[1]; f = (p >> 16) & 255u;
                a = t[p >> 24]; b = t[((p >> 24) + 1u) & 255u];
                s = (s + a + (((b - a) * (int32_t)f) >> 8)) >> 1;
                sea_ph[1] = p + inc2;
            }
        }
        if (snd.tone < 100) { sea_lp += ((s - sea_lp) * k) >> 15; s = sea_lp; }
        sea_buf[i] = s;
    }
}

/* ------------------------------------------------------------------------------------------- the envelope */
static uint32_t env_step(uint8_t index)         /* the change a sample, for a full sweep in env_ms[index] */
{
    if (index >= ENV_STEPS) return 0;
    uint32_t frames = (uint32_t)env_ms[index] * (FS / 100u) / 10u;
    return frames ? (1u << 24) / frames + 1u : 1u << 24;
}

/* ---------------------------------------------------------------------------------------------- one bell */
static int32_t bell_mix[VOICES][HALF_FRAMES];

static void voice_render(uint32_t v, uint32_t frames)
{
    voice_t *x = &voices[v];
    int32_t *out = bell_mix[v];
    if (x->retrig) {
        x->retrig = 0;
        x->awake = 1;
        x->quiet = 0;
        if (!x->frozen) x->stage = ENV_ATTACK;
        if (!x->pitch) x->pitch = x->target;
    }
    if (!x->awake) { x->peak = 0; return; }

    /* the glide: GLIDE 0..100 -> 0 .. 8 s */
    int32_t target = x->target, delta = target - x->pitch;
    uint32_t glide_frames = (uint32_t)glide * glide * (FS / 1250u);
    if (!glide_frames || (delta < 4 && delta > -4)) x->pitch = target;
    else {
        uint32_t kq = (frames << 16) / glide_frames;
        if (kq > 65536u) kq = 65536u;
        x->pitch += (int32_t)(((int64_t)delta * (int32_t)kq) >> 16);
    }

    /* the LFO, for this bell */
    int32_t l = snd.lfo_depth ? lfo_value(v) : 0;
    int32_t mod = l * snd.lfo_depth / 100;      /* -32767 .. 32767 */
    int32_t mod50 = mod * 50 / 32768;           /* +- 50 points */
    uint8_t tg = snd.lfo_target;
    int32_t pitch = x->pitch + (tg == LFO_PITCH ? mod >> 6 : 0);   /* +- 2 semitones */
    int32_t ring = clamp100(snd.ring + (tg == LFO_RING ? mod50 : 0));
    int32_t color = clamp100(snd.color + (tg == LFO_COLOR ? mod50 : 0));
    int32_t grit = clamp100(snd.grit + (tg == LFO_GRIT ? mod50 : 0));
    int32_t push = clamp100(snd.push + (tg == LFO_PUSH ? mod50 : 0));
    int32_t tremolo = Q15;
    if (tg == LFO_VOLUME) tremolo = Q15 - (int32_t)((int64_t)(32767 - l) * snd.lfo_depth / 200);
    x->shown = pitch;
    uint8_t spread = snd.spread != SPREAD_OFF;
    int32_t pitch2 = pitch + x->partner;
    uint32_t t60 = ring_ms[ring];

    /* the envelope's speeds */
    uint32_t up = env_step(snd.attack), down = env_step(snd.decay), rel = env_step(snd.release);
    int32_t sustain = (int32_t)((uint32_t)snd.sustain * (1u << 24) / 100u);
    if (!x->gate && !x->frozen && x->stage != ENV_RELEASE) x->stage = ENV_RELEASE;

    uint8_t engine = snd.engine;
    if (engine != x->engine) {                  /* a new effect starts from silence */
        x->engine = engine;
        for (uint32_t i = 0; i < VOICE_BUF; i++) voice_buf[v][i] = 0;
        x->lp[0] = x->lp[1] = x->fb = 0;
        for (uint32_t i = 0; i < 5; i++) x->svf_lp[i] = x->svf_bp[i] = 0;
    }
    int16_t *buf = voice_buf[v];
    int32_t noise_amount = grit * 160;          /* GRIT's noise, Q15 of full scale */
    int32_t peak = 0;

    /* ---- the engine's coefficients, once a block */
    int32_t feed = Q15, g[2] = {0, 0}, k = 4000 + color * 287;
    uint32_t delay[2] = {0, 0};
    uint32_t ph_inc[2] = {0, 0};
    uint8_t up_shift[2] = {0, 0}, wbits = 11;
    int32_t F[5] = {0}, wgt[5] = {0}, q = Q15;
    uint8_t nf = 0;
    if (engine == ENG_RESONATOR) {
        int32_t comp = ((Q15 - k) << 8) / k;    /* the loop's low-pass delays a little: take it off the comb */
        for (uint32_t c = 0; c < 2u; c++) {
            uint32_t d = period_of(c ? pitch2 : pitch);
            d = d > (uint32_t)comp + 512u ? d - (uint32_t)comp : 512u;
            if (d > (COMB_LEN - 2u) << 8) d = (COMB_LEN - 2u) << 8;
            delay[c] = d;
            int32_t loss = (int32_t)((d >> 8) * 5127u / t60);
            g[c] = Q15 - loss;
            if (g[c] < 16384) g[c] = 16384;
            if (g[c] > 32760) g[c] = 32760;
        }
        feed = (int32_t)isqrt32((uint32_t)(Q15 - g[0]) << 15);
    } else if (engine == ENG_SHIFTER) {
        wbits = (uint8_t)(11 - grit / 34);      /* GRIT: shorter grains, rougher */
        for (uint32_t c = 0; c < 2u; c++) {
            int32_t r = (int32_t)ratio_q16((c ? pitch2 : pitch) - sea_pitch) - 65536;
            up_shift[c] = r > 0;
            uint32_t a = (uint32_t)(r < 0 ? -r : r);
            ph_inc[c] = a << (16u - wbits);
        }
        g[0] = ring * 280;                      /* RING: feedback, 0 .. 0.85: the shimmer stacks the interval */
    } else {
        /* PRISM: the note's first four overtones (fewer the darker), and the SPREAD note */
        uint32_t base = inc_of(pitch);
        static const int32_t falloff[4] = {Q15, 23000, 16000, 11000};
        for (uint32_t h = 1; h <= 4u; h++) {
            if (base > 584000000u / h) break;    /* (no filter above 6 kHz) */
            int32_t w = falloff[h - 1];
            for (uint32_t j = 1; j < h; j++) w = w * color / 100;
            if (h > 1 && w < 300) continue;
            uint32_t inc = base * h;
            uint32_t th = (uint32_t)(((uint64_t)inc * 205887u) >> 32);   /* pi f / FS, Q16 */
            uint32_t th3 = ((th * th) >> 16) * th >> 16;
            F[nf] = (int32_t)(th - th3 / 6u);    /* 2 sin(theta), Q15 */
            wgt[nf++] = w;
        }
        if (spread) {
            uint32_t inc = inc_of(pitch2);
            if (inc < 584000000u) {
                uint32_t th = (uint32_t)(((uint64_t)inc * 205887u) >> 32);
                uint32_t th3 = ((th * th) >> 16) * th >> 16;
                F[nf] = (int32_t)(th - th3 / 6u);
                wgt[nf++] = 19000;
            }
        }
        uint32_t b12 = base >> 12;
        q = b12 ? (int32_t)((1709000000u / b12) / t60) : Q15;   /* RING: the same seconds as the resonator's */
        if (q > 45000) q = 45000;
        if (q < 24) q = 24;
        feed = (int32_t)isqrt32((uint32_t)q << 15);
    }
    int32_t partner_level = spread ? 19000 : 0;

    for (uint32_t i = 0; i < frames; i++) {
        /* the envelope */
        if (!x->frozen) {
            switch (x->stage) {
            case ENV_ATTACK:
                x->env += (int32_t)up;
                if (x->env >= 1 << 24) { x->env = 1 << 24; x->stage = ENV_DECAY; }
                break;
            case ENV_DECAY:
                x->env -= (int32_t)down;
                if (x->env <= sustain) { x->env = sustain; x->stage = ENV_SUSTAIN; }
                break;
            case ENV_RELEASE:
                if (rel) { x->env -= (int32_t)rel; if (x->env < 0) x->env = 0; }
                break;
            default: break;
            }
        }
        /* what the sea gives this bell: the sea, pushed, plus GRIT's noise; the envelope opens it */
        int32_t in = sea_buf[i] * push / 100 + (white(&x->noise) * noise_amount >> 15);
        in = (int32_t)(((int64_t)in * (x->env >> 9)) >> 15);
        int32_t y;
        if (engine == ENG_RESONATOR) {
            int32_t fin = in * feed >> 15;
            int32_t sum = 0;
            for (uint32_t c = 0; c < (spread ? 2u : 1u); c++) {
                int16_t *ring_buf = buf + c * COMB_LEN;
                uint32_t d = delay[c], back = x->w - (d >> 8), fr = d & 255u;
                int32_t a = ring_buf[back & (COMB_LEN - 1u)], b = ring_buf[(back - 1u) & (COMB_LEN - 1u)];
                int32_t s = a + (((b - a) * (int32_t)fr) >> 8);
                x->lp[c] += ((s - x->lp[c]) * k) >> 15;
                int32_t o = soft(fin + (x->lp[c] * g[c] >> 15));
                ring_buf[x->w & (COMB_LEN - 1u)] = (int16_t)o;
                sum += c ? o * partner_level >> 15 : o;
            }
            /* a DC blocker */
            x->dc_y = sum - x->dc_x + (x->dc_y * 32604 >> 15);
            x->dc_x = sum;
            y = x->dc_y;
        } else if (engine == ENG_SHIFTER) {
            uint32_t mask = VOICE_BUF - 1u, sh = 16u - wbits;
            buf[x->w & mask] = (int16_t)soft(in + (x->fb * g[0] >> 15));
            int32_t sum = 0;
            for (uint32_t c = 0; c < (spread ? 2u : 1u); c++) {
                int32_t pair = 0;
                for (uint32_t tap = 0; tap < 2u; tap++) {
                    uint32_t p = x->ph[c] + (tap ? 0x80000000u : 0u);
                    uint32_t d = (up_shift[c] ? ~p : p) >> sh;               /* the delay, Q16 samples */
                    int32_t win = (int32_t)((p < 0x80000000u ? p : ~p) >> 16);  /* a triangle, 0 .. 32767 */
                    uint32_t back = x->w - 3u - (d >> 16), fr = (d >> 8) & 255u;
                    int32_t a = buf[back & mask], b = buf[(back - 1u) & mask];
                    pair += (a + (((b - a) * (int32_t)fr) >> 8)) * win >> 15;
                }
                x->ph[c] += ph_inc[c];
                sum += c ? pair * partner_level >> 15 : pair;
            }
            x->lp[0] += ((sum - x->lp[0]) * k) >> 15;   /* COLOR */
            y = x->lp[0];
            x->fb = y;
        } else {
            int32_t fin = in * feed >> 15, sum = 0;
            for (uint32_t f = 0; f < nf; f++) {
                int32_t bp = x->svf_bp[f], lp = x->svf_lp[f];
                lp += (int32_t)(((int64_t)F[f] * bp) >> 15);
                int32_t hp = fin - lp - (int32_t)(((int64_t)q * bp) >> 15);
                bp += (int32_t)(((int64_t)F[f] * hp) >> 15);
                if (bp > 4000000) bp = 4000000;
                if (bp < -4000000) bp = -4000000;
                x->svf_bp[f] = bp;
                x->svf_lp[f] = lp;
                sum += bp * wgt[f] >> 15;
            }
            y = sum;
        }
        x->w++;
        /* the bell's own level keeper: a loud resonance is brought down, never a quiet one up */
        int32_t m = y < 0 ? -y : y;
        if (m > peak) peak = m;
        y = (int32_t)(((int64_t)y * x->gain) >> 15);
        y = soft(y);
        out[i] = y * tremolo >> 15;
    }
    /* the level keeper: what this block would have peaked at, against 16000 */
    int32_t want = peak > 16000 ? (int32_t)(16000u * 32768u / (uint32_t)peak) : Q15;
    if (want < x->gain) x->gain = want;
    else x->gain += (want - x->gain) >> 6;
    int32_t shown = (int32_t)(((int64_t)peak * x->gain) >> 15);
    x->peak = (uint16_t)(shown > 32767 ? 32767 : shown);
    /* asleep once the envelope is shut and the ring has died (about 0.2 s under -72 dB) */
    if (!x->gate && !x->frozen && x->env == 0 && peak < 8) {      /* (RELEASE FOREVER: the env never gets there) */
        if (++x->quiet > 35u) { x->awake = 0; x->peak = 0; }
    } else {
        x->quiet = 0;
    }
}

static void bell_init(void)
{
    for (uint32_t v = 0; v < VOICES; v++) {
        voices[v].noise = 0x9e3779b9u * (v + 1u);
        voices[v].gain = Q15;
        voices[v].lfo_random[0] = voices[v].lfo_random[1] = 0x68e31da4u * (v + 3u);
        voices[v].engine = 0xFF;
    }
}

/* -------------------------------------------------------------------------------------- everything, mixed */
static void bell_render(int32_t *out, uint32_t frames)
{
    sea_render(frames);
    for (uint32_t v = 0; v < VOICES; v++) voice_render(v, frames);
    lfo_advance(frames);
    /* bells 1 left, 2 in the middle, 3 right; the dry sea in the middle */
    static const int16_t left[VOICES] = {32767, 26000, 15000}, right[VOICES] = {15000, 26000, 32767};
    int32_t bell = snd.bell * 328, sea = snd.sea * 200;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t l = 0, r = 0;
        for (uint32_t v = 0; v < VOICES; v++) {
            if (!voices[v].awake && !voices[v].peak) continue;
            l += bell_mix[v][i] * left[v] >> 15;
            r += bell_mix[v][i] * right[v] >> 15;
        }
        int32_t dry = sea_buf[i] * sea >> 15;
        out[2u * i] += (l * bell >> 15) + dry;
        out[2u * i + 1u] += (r * bell >> 15) + dry;
    }
}
