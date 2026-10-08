/* SPDX-License-Identifier: GPL-3.0-only */
/* Cesari 606 drum part: Charles Vestal's 6W6 (GPL-3.0) around Matthew Fecher's
 * 606-Inspired-Synth-Drums voices (AudioKit Pro, MIT), ported from C++ to C for
 * the FM-1.
 *
 * Same voices, fitted constants, pot tables, defaults, kit trims, velocity law,
 * drive stage and hat choke as 6W6's sd606_engine.cpp. The hats and cymbal are
 * 6W6's coupled-form metal voice (sd606_metal_voice.h) with its hardware-fitted
 * partial tables; the clap is 6W6's fork (sd606_clap_voice.h).
 *
 * What is NOT here: 6W6's own reverb, delay, glue, master drive and volume (the
 * firmware's sends and master do those), its mutes and state blob, its note map.
 *
 * What is DIFFERENT (for the FM-1's CPU, all measured against the C++ on the
 * host, tests/drum606_cmp.cpp):
 *   - every exp / sin / tanh / pow is fastmath.h's, in float (the C++ mixes in
 *     double); the clap's envelopes are float too;
 *   - the clap pitched DOWN (TUNE under the middle) runs its core at the lower
 *     rate and reads it back with a 4-point cubic, not 6W6's 128-tap windowed
 *     sinc (pitched up, or at the middle, the two are the same code path);
 *   - the clap's colour filters stop once their envelope is under -140 dB.
 *
 * Mono, 44.1 kHz, C99, float, no libm, no allocation. An idle drum costs nothing.
 * Threading: drum606_trigger / drum606_set are called between render blocks. */
#pragma once
#include <stdint.h>

enum { D6_BD, D6_SD, D6_LT, D6_HT, D6_CH, D6_OH, D6_CY, D6_CP, D6_NUM, D6_KIT = D6_NUM };

/* per-drum pot slots: X1 is BD attack / SD snappy / CP noise, X2 is SD tone */
enum { D6P_LEVEL, D6P_TUNE, D6P_DECAY, D6P_DRIVE, D6P_DIST, D6P_REV, D6P_DLY, D6P_X1, D6P_X2, D6P_NUM };
/* kit-wide: velocity depth (6W6 "Velocity"), hat choke (Off / CH>OH / Mutual) */
enum { D6K_VEL, D6K_CHOKE, D6K_NUM };

#define D6_DIST_TYPES 7
#define D6_PARTIALS 32
#define D6_CLAP_TAPS 192

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } d6_bq_t;
typedef struct { float a, z; } d6_lp1_t;
typedef struct { float x1, y1; } d6_dc_t;
typedef struct { uint32_t s; float in, out; } d6_wn_t;   /* WhiteNoise: xorshift + leaky differentiator */

typedef struct {
    int on;
    uint32_t silent;
    d6_wn_t noise;
    /* SweepSine body */
    float ph, hz, hz_end, amp, amp_k, pitch_k;
    float click_env, click_k, imp_env, imp_k;
    d6_lp1_t click_lp, body_lp, imp_lp;
    d6_bq_t imp_hp;
    d6_dc_t dc;
    float last_raw, click_amt, imp_amt;
} d6_bd_t;

typedef struct {
    int on;
    uint32_t rng, frame, frames;
    float decay, pitch, snappy, color, sel_dur, hold, fade;
    float body_ph, ring_ph, nsr_gain;
    d6_bq_t band1, band2;
    d6_lp1_t lp;
} d6_sd_t;

typedef struct {
    int on;
    const struct d6_tom_spec *spec;
    uint32_t rng_lo, rng_hi, rng_tail;
    float main_hz, glide_hz, glide_k, lower_hz, strike_hz;
    float main_ph, lower_ph, strike_ph;
    float up_hz[4], up_ph[4], up_env[4], up_k[4];
    float main_env, lower_env, strike_env, snap_env, burst_env, tail_env;
    float main_k, lower_k, strike_k, snap_k, burst_k, tail_k;
    float body_rise, body_rise_k, strike_rise, strike_rise_k, snap_rise, snap_rise_k, noise_rise, noise_rise_k;
    d6_bq_t lo_hp, lo_lp, hi_hp, hi_lp, fs_hp, fs_lp, tl_hp, tl_lp;
} d6_tom_t;

typedef struct {
    int on, n;
    uint32_t phase_rng, wob_rng, frame, frames, fade_frames;
    float inv_fade;
    d6_wn_t noise;
    d6_bq_t n_hp, n_lp;
    d6_dc_t dc;
    float inc[D6_PARTIALS], x[D6_PARTIALS], y[D6_PARTIALS], k[D6_PARTIALS], amp[D6_PARTIALS], wob[D6_PARTIALS];
    uint32_t bell;   /* bit i: partial i is a bell line */
    float wob_alpha, wob_drive, bell_amt, bell_k, bell_env, click_amt, click_k, click_env;
    float tonal, noise_mix, sat, trim, att_k, att_env, fast_k, slow_k, fast_w, fast_env, slow_env;
} d6_metal_t;

typedef struct { float full, dry; } d6_cf_t;
typedef struct {
    int on;
    uint32_t idx, max, core_idx, rng;
    float rate, decay, air, step, pos, noise_gain;
    uint32_t taps, at;
    float hist[2 * D6_CLAP_TAPS];
    float coef[4 * D6_CLAP_TAPS];
    d6_cf_t f[4];                 /* core frames at floor(pos) - 1 .. + 2 (pitched down only) */
    int32_t f_at;                 /* the core index f[1] holds */
    float air_k, air_in, air_out;
} d6_cp_t;

typedef struct {
    float hit, choke, choke_step, crush[2];
} d6_rt_t;

typedef struct drum606 {
    uint8_t pot[D6_NUM][D6P_NUM];     /* raw 0..127 (DIST 0..6), the stored form */
    uint8_t kit[D6K_NUM];
    float level[D6_NUM], drive[D6_NUM], rev[D6_NUM], dly[D6_NUM];
    float vel_depth;
    d6_rt_t rt[D6_NUM];
    d6_bd_t bd;
    d6_sd_t sd;
    d6_tom_t lt, ht;
    d6_metal_t ch, oh, cy;
    d6_cp_t cp;
} drum606_t;

void drum606_init(drum606_t *d);                             /* 6W6 defaults */
void drum606_trigger(drum606_t *d, int drum, int vel);       /* vel 1..127 */
/* adds one block into dry and the per-drum reverb / delay sends into rev / dly */
void drum606_render(drum606_t *d, float *dry, float *rev, float *dly, int n);
int drum606_active(const drum606_t *d);
void drum606_set(drum606_t *d, int drum, int slot, int value);  /* drum D6_KIT: slot is D6K_* */
int drum606_get(const drum606_t *d, int drum, int slot);
int drum606_default(int drum, int slot);
