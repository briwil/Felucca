/* SPDX-License-Identifier: GPL-3.0-only */
/* The 606: see drum606.h. Each section names the C++ it was ported from (6W6:
 * github.com/charlesvestal/schwung-6W6, src/dsp and src/vendor/606). The fitted
 * constants are copied exactly; the comments that explain them are 6W6's and
 * Matthew Fecher's, shortened. */
#include "drum606.h"
#include "fastmath.h"

#define S6_SR 44100.0f
#define S6_ISR (1.0f / 44100.0f)
#define S6_PI 3.14159265358979323846f
#define S6_2PI 6.28318530717958647692f
#define S6_T60 6.9077553f                    /* ln(1000): a T60 as a time constant */

/* ---- SynthDrumCommon.hpp ---------------------------------------------- */

static inline float s6_flush(float x) { return fm_fabsf(x) < 1.0e-20f ? 0.0f : x; }
static inline float s6_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float s6_lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline uint32_t s6_seed(uint32_t s) { return s ? s : 0x12345678u; }
static inline uint32_t s6_xs(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static inline float s6_bip(uint32_t *s) { return (float)s6_xs(s) * (2.0f / 4294967295.0f) - 1.0f; }
static inline float s6_bip24(uint32_t *s) { return (float)(s6_xs(s) >> 8) * (2.0f / 16777215.0f) - 1.0f; }
static inline float s6_bip_lo24(uint32_t *s) { return (float)(s6_xs(s) & 0x00FFFFFFu) / 8388608.0f - 1.0f; }

static inline float s6_wrap(float p)
{
    while (p >= S6_2PI)
        p -= S6_2PI;
    while (p < 0.0f)
        p += S6_2PI;
    return p;
}

static inline float s6_decay_k(float sec) { return fm_expf(-1.0f / (S6_SR * fm_maxf(0.0005f, sec))); }
static inline float s6_pole_k(float sec) { return fm_expf(-1.0f / (S6_SR * fm_maxf(0.00001f, sec))); }
static inline float s6_t60_k(float sec) { return fm_expf(-S6_T60 / (S6_SR * fm_maxf(0.0001f, sec))); }

static inline float s6_dc(d6_dc_t *d, float x)
{
    float y = x - d->x1 + 0.995f * d->y1;
    d->x1 = x;
    d->y1 = y;
    return y;
}

static inline void s6_lp1_set(d6_lp1_t *f, float hz)
{
    f->a = 1.0f - fm_expf(-S6_2PI * s6_clamp(hz, 10.0f, S6_SR * 0.45f) * S6_ISR);
}
static inline float s6_lp1(d6_lp1_t *f, float x) { return f->z += f->a * (x - f->z); }

enum { S6_LP, S6_HP, S6_BP };
static void s6_bq_set(d6_bq_t *f, int type, float hz, float q)
{
    float w0 = S6_2PI * s6_clamp(hz, 10.0f, S6_SR * 0.45f) * S6_ISR;
    float c = fm_cosf(w0), s = fm_sinf(w0), alpha = s / (2.0f * fm_maxf(0.05f, q)), ia = 1.0f / (1.0f + alpha);
    switch (type) {
    case S6_HP: f->b0 = (1.0f + c) * 0.5f; f->b1 = -(1.0f + c); f->b2 = f->b0; break;
    case S6_BP: f->b0 = 0.5f * s; f->b1 = 0.0f; f->b2 = -0.5f * s; break;
    default: f->b0 = (1.0f - c) * 0.5f; f->b1 = 1.0f - c; f->b2 = f->b0; break;
    }
    f->b0 *= ia;
    f->b1 *= ia;
    f->b2 *= ia;
    f->a1 = -2.0f * c * ia;
    f->a2 = (1.0f - alpha) * ia;
}
static inline void s6_bq_reset(d6_bq_t *f) { f->z1 = f->z2 = 0.0f; }
static inline float s6_bq(d6_bq_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}
static inline float s6_bq_fl(d6_bq_t *f, float x)   /* HiHats.hpp LowPassBiquad: flushes its state */
{
    float y = f->b0 * x + f->z1;
    f->z1 = s6_flush(f->b1 * x - f->a1 * y + f->z2);
    f->z2 = s6_flush(f->b2 * x - f->a2 * y);
    return y;
}

static inline void s6_wn_init(d6_wn_t *w, uint32_t seed)
{
    w->s = s6_seed(seed);
    w->in = w->out = 0.0f;
}
static inline float s6_wn(d6_wn_t *w)
{
    float x = s6_bip(&w->s), y = x - w->in + 0.995f * w->out;
    w->in = x;
    w->out = y;
    return y;
}

/* ---- BassDrum.hpp (Super 606 XL kick) ---------------------------------- */

#define S6BD_TUNE 0.28f
#define S6BD_TONE 0.34f
#define S6BD_DRIVE 0.18f

static void s6bd_init(d6_bd_t *b, uint32_t seed)
{
    s6_wn_init(&b->noise, seed);
    b->ph = 0.0f;
    b->hz = b->hz_end = 100.0f;
    b->amp = 0.0f;
    b->amp_k = 0.999f;
    b->pitch_k = 0.001f;
    b->click_env = b->imp_env = 0.0f;
    b->click_k = b->imp_k = s6_decay_k(0.1f);
    s6_lp1_set(&b->click_lp, 1400.0f);
    s6_lp1_set(&b->body_lp, 900.0f);
    s6_lp1_set(&b->imp_lp, 2600.0f);
    b->click_lp.z = b->body_lp.z = b->imp_lp.z = 0.0f;
    s6_bq_set(&b->imp_hp, S6_HP, 1350.0f, 0.707f);
    s6_bq_reset(&b->imp_hp);
    b->dc.x1 = b->dc.y1 = 0.0f;
    b->last_raw = 0.0f;
    b->on = 0;
    b->silent = 0;
}

static void s6bd_trigger(d6_bd_t *b, float transient, float decay_pct, float semis)
{
    const float a = 1.0f, t = S6BD_TUNE - 0.5f;
    float dcy = s6_clamp(decay_pct, 0.0f, 1.0f), tr = s6_clamp(transient, 0.0f, 1.0f);
    float decay = s6_lerp(-1.75f, 3.0f, dcy * dcy);   /* most of the knob short and punchy, the top the long tail */
    float attack = 0.10f + tr * 0.50f;
    float ratio, start, end, shape, thud;
    b->click_amt = b->imp_amt = 0.10f + tr * 0.50f;
    ratio = fm_exp2f(semis * (1.0f / 12.0f));
    start = (120.0f + t * 16.0f) * ratio;
    end = (53.0f + t * 8.0f) * ratio;
    shape = s6_clamp((attack + b->click_amt + b->imp_amt) / 3.0f, 0.0f, 1.5f);
    thud = s6_clamp(shape * shape, 0.0f, 1.5f);
    /* the body: a sine sweeping start -> end */
    b->amp = 0.92f + thud * 0.22f + a * 0.08f;
    b->ph = -0.33f * S6_PI;
    b->hz = start * (1.0f + thud * 0.28f);
    b->hz_end = end;
    b->amp_k = s6_decay_k(0.22f + decay * 0.12f);
    b->pitch_k = 1.0f - fm_expf(-1.0f / (S6_SR * fm_maxf(0.0005f, s6_lerp(0.022f, 0.008f, s6_clamp(thud, 0.0f, 1.0f)))));
    b->click_k = s6_decay_k(0.0020f + attack * 0.0045f + b->click_amt * 0.0040f);
    b->click_env = fm_maxf(b->click_env, 0.02f + attack * 0.06f + b->click_amt * 0.28f + a * 0.02f);
    b->imp_k = s6_decay_k(0.0008f + b->imp_amt * 0.0080f);
    b->imp_env = fm_maxf(b->imp_env, 0.02f + b->imp_amt * 0.55f + thud * 0.08f + a * 0.04f);
    s6_lp1_set(&b->body_lp, s6_lerp(620.0f, 1200.0f, S6BD_TONE));
    b->last_raw = 0.0f;
    b->on = 1;
    b->silent = 0;
}

static inline float s6bd_env(float *v, float k)
{
    *v *= k;
    if (*v < 1.0e-6f)
        *v = 0.0f;
    return *v;
}

static float s6bd_process(d6_bd_t *b)
{
    float noise, raw, body, click, delta, imp, out;
    if (!b->on)
        return 0.0f;
    noise = s6_wn(&b->noise);
    b->hz += (b->hz_end - b->hz) * b->pitch_k;
    b->ph += S6_2PI * b->hz * S6_ISR;
    if (b->ph >= S6_2PI)
        b->ph -= S6_2PI;
    raw = fm_sinf(b->ph) * b->amp;
    b->amp *= b->amp_k;
    if (b->amp < 1.0e-6f)
        b->amp = 0.0f;
    body = s6_lp1(&b->body_lp, raw);
    click = s6_lp1(&b->click_lp, noise) * s6bd_env(&b->click_env, b->click_k);
    delta = raw - b->last_raw;
    b->last_raw = raw;
    imp = s6_lp1(&b->imp_lp, s6_bq(&b->imp_hp, delta)) * s6bd_env(&b->imp_env, b->imp_k);
    out = body * (0.92f + S6BD_DRIVE * 0.10f) + click * (0.06f + b->click_amt * 0.90f) +
          imp * (0.08f + b->imp_amt * 2.20f);
    out = s6_dc(&b->dc, fm_tanhf(out * (1.12f + S6BD_DRIVE * 0.12f))) * 0.70f;   /* kVoiceOutputTrim */
    if (b->amp > 1.0e-6f || b->click_env > 1.0e-6f || b->imp_env > 1.0e-6f)
        b->silent = 0;
    else if (fm_fabsf(out) < 1.0e-5f) {
        if (++b->silent >= 32u)
            b->on = 0;
    } else
        b->silent = 0;
    return out;
}

/* ---- Snare.hpp (Super 606 snare: a tuned shell and filtered wires) ------ */

#define S6SD_BODY_DUR (11016.0f / 44100.0f)
#define S6SD_NOISE_DUR (16323.0f / 44100.0f)
#define S6SD_SETTLED_HZ 201.09442f
#define S6SD_BEND_HZ 149.23143f
#define S6SD_BEND_T 0.0133309f
#define S6SD_ATT_T 0.0007633316f
#define S6SD_ATT_SHAPE 3.2748916f
#define S6SD_BODY_T60 0.243f
#define S6SD_BODY_LEVEL 0.710f
#define S6SD_TONE_LEVEL 0.391f
#define S6SD_TRANS_LEVEL 0.44f
#define S6SD_TRANS_T60 0.030f
#define S6SD_BODY_PH 1.1711004f
#define S6SD_IMP_START 0.0002763113f
#define S6SD_IMP_END 0.0013649489f
#define S6SD_IMP_LEVEL (-0.3000014f)
#define S6SD_IMP_DECAY 0.0025407782f
#define S6SD_W1_HZ 2998.628f
#define S6SD_W1_Q 1.2771896f
#define S6SD_W2_HZ 4596.2905f
#define S6SD_W2_Q 0.30369505f
#define S6SD_W2_AMT 1.597958f
#define S6SD_WLP_HZ 19677.396f
#define S6SD_WATT_T 0.0031f
#define S6SD_WATT_SHAPE 1.3f
#define S6SD_W_T60 0.395f
#define S6SD_W_LEVEL 1.28f
#define S6SD_RING_LEVEL 0.014f
#define S6SD_RING_ATT_T 0.012f
#define S6SD_RING_PH (-1.1f)
#define S6SD_FULL 0.999f
#define S6SD_GATE_CURVE 11.05f
#define S6SD_GATE_TARGET 1.5881701e-5f

static void s6sd_stop(d6_sd_t *s)
{
    s->on = 0;
    s->frame = s->frames = 0;
    s->body_ph = s->ring_ph = 0.0f;
    s6_bq_reset(&s->band1);
    s6_bq_reset(&s->band2);
    s->lp.z = 0.0f;
}

static void s6sd_init(d6_sd_t *s, uint32_t seed)
{
    s->rng = seed ? seed : 0x606606u;
    s->decay = s->pitch = s->snappy = s->color = 1.0f;
    s->sel_dur = s->hold = S6SD_NOISE_DUR;
    s->fade = 0.0f;
    s->nsr_gain = 1.0f;
    s6sd_stop(s);
}

static void s6sd_trigger(d6_sd_t *s, float decay_pct, float pitch, float snappy, float color)
{
    float removed, f;
    s->decay = s6_clamp(decay_pct, 0.01f, 1.0f);
    s->pitch = s6_clamp(pitch, 0.25f, 4.0f);
    s->snappy = s6_clamp(snappy, 0.0f, 1.0f);
    s->color = s6_clamp(color, 0.25f, 4.0f);
    s->sel_dur = s->decay >= S6SD_FULL ? S6SD_NOISE_DUR : S6SD_NOISE_DUR * s->decay;
    removed = fm_maxf(0.0f, S6SD_NOISE_DUR - s->sel_dur);   /* short settings fold the end down, not chop it */
    s->fade = fm_minf(s->sel_dur, removed);
    s->hold = fm_maxf(0.0f, s->sel_dur - s->fade);
    f = s->sel_dur * S6_SR;
    s->frames = (uint32_t)f;
    if ((float)s->frames < f)
        s->frames++;
    if (!s->frames)
        s->frames = 1;
    s->frame = 0;
    s->body_ph = S6SD_BODY_PH;
    s->ring_ph = S6SD_RING_PH;
    s6_bq_set(&s->band1, S6_BP, S6SD_W1_HZ * s->color, S6SD_W1_Q);
    s6_bq_set(&s->band2, S6_BP, S6SD_W2_HZ * s->color, S6SD_W2_Q);
    s6_bq_reset(&s->band1);
    s6_bq_reset(&s->band2);
    s6_lp1_set(&s->lp, S6SD_WLP_HZ * s->color);
    s->lp.z = 0.0f;
    s->nsr_gain = 1.0f;                       /* sqrt(sr / 44100) */
    s->on = 1;
}

static inline float s6sd_gate(const d6_sd_t *s, float time)
{
    float p;
    if (s->decay >= S6SD_FULL || time < s->hold)
        return 1.0f;
    if (s->fade <= 0.0f)
        return 0.0f;
    p = (time - s->hold) / s->fade;
    if (p >= 1.0f)
        return 0.0f;
    return fm_maxf(0.0f, (1.0f + S6SD_GATE_TARGET) * fm_expf(-S6SD_GATE_CURVE * p) - S6SD_GATE_TARGET);
}

static float s6sd_process(d6_sd_t *s)
{
    float time, gate, bend, body = 0.0f, noise = 0.0f, out;
    if (!s->on || s->frame >= s->frames) {
        s->on = 0;
        return 0.0f;
    }
    time = (float)s->frame / S6_SR;
    gate = s6sd_gate(s, time);
    bend = S6SD_BEND_HZ * fm_expf(-time / S6SD_BEND_T);   /* (the shell's bend; the wires' ring shares it) */
    {   /* the shell: the note pitches, the short bend stays */
        float tuned = time * s->pitch;
        if (tuned < S6SD_BODY_DUR && time < s->sel_dur) {
            float hz = S6SD_SETTLED_HZ * s->pitch + bend;
            float e = time * (1.0f / S6SD_ATT_T), att, env, tenv, imp = 0.0f;
            att = e > 18.0f ? 1.0f : fm_powf(1.0f - fm_expf(-e), S6SD_ATT_SHAPE);   /* (1 - e^-18 is 1.0f) */
            env = att * fm_expf(-S6_T60 * tuned / S6SD_BODY_T60);
            tenv = att * fm_expf(-S6_T60 * time / S6SD_TRANS_T60);
            if (time >= S6SD_IMP_START && time < S6SD_IMP_END)
                imp = S6SD_IMP_LEVEL * fm_expf(-(time - S6SD_IMP_START) / S6SD_IMP_DECAY);
            body = s6_flush(((S6SD_TONE_LEVEL * env + S6SD_TRANS_LEVEL * tenv) * fm_sinf(s->body_ph) + imp) *
                            S6SD_BODY_LEVEL * gate);
            s->body_ph = s6_wrap(s->body_ph + S6_2PI * hz / S6_SR);
        }
    }
    if (time < s->sel_dur) {   /* the wires: kept moving at zero SNAP so later hits stay repeatable */
        float white = s6_bip_lo24(&s->rng);
        float b1 = s6_bq(&s->band1, white), b2 = s6_bq(&s->band2, white);
        float colored = s6_lp1(&s->lp, b1 - S6SD_W2_AMT * b2);
        float ap = fm_minf(time / S6SD_WATT_T, 1.0f);
        float att = ap >= 1.0f ? 1.0f : 1.0f - fm_powf(1.0f - ap, S6SD_WATT_SHAPE);
        float tail = fm_expf(-S6_T60 * time / S6SD_W_T60);
        float ring_hz = (S6SD_SETTLED_HZ + bend) * s->color;
        noise = S6SD_W_LEVEL * s->nsr_gain * att * tail * colored;
        noise += S6SD_RING_LEVEL * (1.0f - fm_expf(-time / S6SD_RING_ATT_T)) * tail * fm_sinf(s->ring_ph);
        s->ring_ph = s6_wrap(s->ring_ph + S6_2PI * ring_hz / S6_SR);
        noise = s6_flush(noise * gate);
    }
    out = s6_flush(body + noise * s->snappy);
    if (++s->frame >= s->frames)
        s->on = 0;
    return out;
}

/* ---- Toms.hpp (Super 606 low and high toms) ----------------------------- */

typedef struct { float ratio, level, t60, ph; } d6_mode_t;
struct d6_tom_spec {
    float main_hz, glide_hz, glide_t, main_t60, main_ph, body_att;
    float lower_ratio, lower_level, lower_t60, lower_ph;
    float h2, h3;
    float strike_hz, strike_level, strike_t60, strike_ph, strike_rise;
    d6_mode_t up[4];
    float snap_t60, burst_t60, tail_t60, noise_rise;
    float lo_snap, lo_burst, lo_hp, lo_lp;
    float hi_snap, hi_burst, hi_hp, hi_lp;
    float fs_level, fs_hp, fs_lp;
    float tail_level, tail_hp, tail_lp;
    float trim;
};

static const struct d6_tom_spec D6_HIGH_TOM = {
    208.0f, 0.0f, 0.050f, 0.218f, 2.674f, 0.00012f,
    135.616f / 208.0f, 0.090f, 0.315f, 1.059f,
    0.0095f, 0.0032f,
    285.0f, 0.71f, 0.0125f, 0.420f, 0.00003f,
    {{305.2f / 208.0f, 0.002617f, 0.800f, -2.987f},
     {345.7f / 208.0f, 0.001954f, 0.800f, 0.618f},
     {367.0f / 208.0f, 0.003381f, 0.400f, 1.002f},
     {386.7f / 208.0f, 0.003546f, 0.360f, 0.597f}},
    0.012f, 0.080f, 0.300f, 0.0015f,
    0.500f, 0.030f, 300.0f, 3000.0f,
    0.030f, 0.009f, 700.0f, 12000.0f,
    0.0f, 700.0f, 1100.0f,
    0.010f, 300.0f, 1500.0f,
    0.80f,
};
static const struct d6_tom_spec D6_LOW_TOM = {
    124.435f, 39.921f, 0.050622f, 0.3276f, 2.2826f, 0.00012f,
    1.0f, 0.0f, 0.100f, 0.0f,
    0.0095f, 0.0032f,
    367.0f, 1.05f, 0.005f, 0.200f, 0.00015f,
    {{1.0f, 0.0f, 0.100f, 0.0f}, {1.0f, 0.0f, 0.100f, 0.0f}, {1.0f, 0.0f, 0.100f, 0.0f}, {1.0f, 0.0f, 0.100f, 0.0f}},
    0.012f, 0.080f, 0.300f, 0.0015f,
    0.500f, 0.030f, 300.0f, 3000.0f,
    0.030f, 0.009f, 700.0f, 12000.0f,
    1.100f, 700.0f, 1100.0f,
    0.010f, 300.0f, 1500.0f,
    0.80f,
};
#define S6TOM_SILENCE 5.0e-6f

static void s6tom_init(d6_tom_t *t, uint32_t seed)
{
    uint32_t s = seed ? seed : 0x606606u;
    t->rng_lo = s6_seed(s ^ 0x60001604u);
    t->rng_hi = s6_seed(s ^ 0x60001607u);
    t->rng_tail = s6_seed(s ^ 0x60001605u);
    t->spec = &D6_HIGH_TOM;
    t->on = 0;
}

static void s6tom_trigger(d6_tom_t *t, const struct d6_tom_spec *sp, float decay_pct, float pitch)
{
    float decay = s6_clamp(decay_pct, 0.05f, 1.0f), r = s6_clamp(pitch, 0.25f, 4.0f);
    int i;
    t->spec = sp;
    t->main_hz = sp->main_hz * r;
    t->glide_hz = sp->glide_hz * r;
    t->glide_k = s6_pole_k(sp->glide_t);
    t->lower_hz = t->main_hz * sp->lower_ratio;
    t->strike_hz = sp->strike_hz * r;
    t->main_ph = sp->main_ph;
    t->lower_ph = sp->lower_ph;
    t->strike_ph = sp->strike_ph;
    t->main_env = t->lower_env = t->strike_env = 1.0f;
    for (i = 0; i < 4; i++) {
        t->up_hz[i] = t->main_hz * sp->up[i].ratio;
        t->up_ph[i] = sp->up[i].ph;
        t->up_env[i] = 1.0f;
        t->up_k[i] = s6_t60_k(sp->up[i].t60 * decay);
    }
    t->snap_env = t->burst_env = t->tail_env = 1.0f;
    t->main_k = s6_t60_k(sp->main_t60 * decay);
    t->lower_k = s6_t60_k(sp->lower_t60 * decay);
    t->strike_k = s6_t60_k(sp->strike_t60 * decay);
    t->snap_k = s6_t60_k(sp->snap_t60 * decay);
    t->burst_k = s6_t60_k(sp->burst_t60 * decay);
    t->tail_k = s6_t60_k(sp->tail_t60 * decay);
    t->body_rise = t->strike_rise = t->snap_rise = t->noise_rise = 0.0f;
    t->body_rise_k = 1.0f - s6_pole_k(sp->body_att);
    t->strike_rise_k = 1.0f - s6_pole_k(sp->strike_rise);
    t->snap_rise_k = 1.0f - s6_pole_k(0.0004f);
    t->noise_rise_k = 1.0f - s6_pole_k(sp->noise_rise);
    s6_bq_set(&t->lo_hp, S6_HP, sp->lo_hp * r, 0.70710678f);
    s6_bq_set(&t->lo_lp, S6_LP, sp->lo_lp * r, 0.70710678f);
    s6_bq_set(&t->hi_hp, S6_HP, sp->hi_hp * r, 0.70710678f);
    s6_bq_set(&t->hi_lp, S6_LP, sp->hi_lp * r, 0.70710678f);
    if (sp->fs_level != 0.0f) {
        s6_bq_set(&t->fs_hp, S6_HP, sp->fs_hp * r, 0.70710678f);
        s6_bq_set(&t->fs_lp, S6_LP, sp->fs_lp * r, 0.70710678f);
    }
    s6_bq_set(&t->tl_hp, S6_HP, sp->tail_hp * r, 0.70710678f);
    s6_bq_set(&t->tl_lp, S6_LP, sp->tail_lp * r, 0.70710678f);
    s6_bq_reset(&t->lo_hp);
    s6_bq_reset(&t->lo_lp);
    s6_bq_reset(&t->hi_hp);
    s6_bq_reset(&t->hi_lp);
    s6_bq_reset(&t->fs_hp);
    s6_bq_reset(&t->fs_lp);
    s6_bq_reset(&t->tl_hp);
    s6_bq_reset(&t->tl_lp);
    t->on = 1;
}

static float s6tom_process(d6_tom_t *t)
{
    const struct d6_tom_spec *sp = t->spec;
    float s, c, main, lower = 0.0f, strike, ups = 0.0f, lo, hi_src, hi, tail, snap, burst, ex, out;
    int i, quiet = 1;
    if (!t->on)
        return 0.0f;
    t->main_ph = s6_wrap(t->main_ph + S6_2PI * (t->main_hz + t->glide_hz) / S6_SR);
    if (sp->lower_level != 0.0f)
        t->lower_ph = s6_wrap(t->lower_ph + S6_2PI * t->lower_hz / S6_SR);
    t->strike_ph = s6_wrap(t->strike_ph + S6_2PI * t->strike_hz / S6_SR);
    for (i = 0; i < 4; i++)
        if (sp->up[i].level != 0.0f)
            t->up_ph[i] = s6_wrap(t->up_ph[i] + S6_2PI * t->up_hz[i] / S6_SR);
    t->body_rise += (1.0f - t->body_rise) * t->body_rise_k;
    t->strike_rise += (1.0f - t->strike_rise) * t->strike_rise_k;
    t->snap_rise += (1.0f - t->snap_rise) * t->snap_rise_k;
    t->noise_rise += (1.0f - t->noise_rise) * t->noise_rise_k;

    s = fm_sinf(t->main_ph);
    c = fm_cosf(t->main_ph);
    main = (s + sp->h2 * 2.0f * s * c + sp->h3 * s * (3.0f - 4.0f * s * s)) * t->main_env;   /* sin 2x, sin 3x */
    if (sp->lower_level != 0.0f)
        lower = sp->lower_level * fm_sinf(t->lower_ph) * t->lower_env;
    strike = sp->strike_level * fm_sinf(t->strike_ph) * t->strike_env * t->strike_rise;
    for (i = 0; i < 4; i++)
        if (sp->up[i].level != 0.0f)
            ups += sp->up[i].level * fm_sinf(t->up_ph[i]) * t->up_env[i];

    lo = s6_bq(&t->lo_lp, s6_bq(&t->lo_hp, s6_bip24(&t->rng_lo)));
    hi_src = s6_bip24(&t->rng_hi);
    hi = s6_bq(&t->hi_lp, s6_bq(&t->hi_hp, hi_src));
    tail = s6_bq(&t->tl_lp, s6_bq(&t->tl_hp, s6_bip24(&t->rng_tail)));
    snap = t->snap_rise * t->snap_env;
    burst = t->noise_rise * t->burst_env;
    ex = lo * (sp->lo_snap * snap + sp->lo_burst * burst) + hi * (sp->hi_snap * snap + sp->hi_burst * burst) +
         tail * sp->tail_level * t->noise_rise * t->tail_env;
    if (sp->fs_level != 0.0f)
        ex += s6_bq(&t->fs_lp, s6_bq(&t->fs_hp, hi_src)) * sp->fs_level * snap;
    out = ((main + lower + ups) * t->body_rise + strike + ex) * sp->trim;

    t->main_env = s6_flush(t->main_env * t->main_k);
    t->glide_hz = s6_flush(t->glide_hz * t->glide_k);
    t->lower_env = s6_flush(t->lower_env * t->lower_k);
    t->strike_env = s6_flush(t->strike_env * t->strike_k);
    for (i = 0; i < 4; i++) {
        t->up_env[i] = s6_flush(t->up_env[i] * t->up_k[i]);
        if (sp->up[i].level * t->up_env[i] > S6TOM_SILENCE)
            quiet = 0;
    }
    t->snap_env = s6_flush(t->snap_env * t->snap_k);
    t->burst_env = s6_flush(t->burst_env * t->burst_k);
    t->tail_env = s6_flush(t->tail_env * t->tail_k);
    if (t->main_env <= S6TOM_SILENCE && sp->lower_level * t->lower_env <= S6TOM_SILENCE && quiet)
        t->on = 0;
    return out;
}

/* ---- sd606_metal_voice.h, sd606_metal_hw.h, sd606_cymbal.h -------------
 * The hats and cymbal: 32 partials each as coupled-form ("magic circle")
 * oscillators, x += k y; y -= k x with k = 2 sin(w/2) -- sin() exactly, given
 * the seed y0 = cos(p + w/2) -- plus filtered noise, a two-rate envelope and a
 * slow per-line wobble retuned every 32 samples. */

typedef struct { float hz, amp; uint8_t bell; } d6_partial_t;
typedef struct {
    const d6_partial_t *p;
    float n_hp, n_lp, tonal, noise, sat, trim, att_t, click, click_t, bell, bell_t;
    float fast_w, fast_t, slow_t;
    uint8_t scales;
    float ref_dur, min_decay, min_dur, gate_fade, wob_depth, wob_corr;
} d6_hat_spec_t;

static const d6_partial_t D6_HAT_PARTIALS[D6_PARTIALS] = {
    {3721.7f, 0.159f, 0}, {3909.9f, 0.119f, 0}, {4131.1f, 0.082f, 0}, {4556.3f, 0.125f, 0},
    {4792.8f, 0.170f, 0}, {5049.2f, 0.158f, 0}, {5317.7f, 0.148f, 0}, {5600.9f, 0.368f, 0},
    {5753.8f, 0.212f, 0}, {5909.4f, 0.284f, 0}, {6221.9f, 0.899f, 1}, {6550.3f, 0.365f, 0},
    {6905.5f, 0.904f, 1}, {7307.3f, 1.000f, 1}, {7702.9f, 0.450f, 0}, {8108.4f, 0.393f, 0},
    {8545.3f, 0.705f, 0}, {9003.3f, 0.332f, 0}, {9472.3f, 0.174f, 0}, {9720.6f, 0.123f, 0},
    {9980.9f, 0.154f, 0}, {10537.4f, 0.230f, 0}, {11109.7f, 0.299f, 0}, {11707.5f, 0.187f, 0},
    {12332.2f, 0.183f, 0}, {12964.5f, 0.193f, 0}, {13310.6f, 0.084f, 0}, {13676.2f, 0.159f, 0},
    {14041.5f, 0.122f, 0}, {14424.2f, 0.152f, 0}, {15184.8f, 0.086f, 0}, {16024.9f, 0.104f, 0},
};
static const d6_partial_t D6_CYM_PARTIALS[D6_PARTIALS] = {
    {1202.2f, 0.315f, 0}, {1383.9f, 0.193f, 0}, {1760.1f, 0.240f, 0}, {1842.8f, 0.171f, 0},
    {1941.6f, 0.315f, 0}, {2339.3f, 0.240f, 0}, {2456.9f, 0.419f, 0}, {2571.8f, 0.218f, 0},
    {2707.2f, 0.167f, 0}, {2838.2f, 0.357f, 0}, {3119.8f, 0.637f, 0}, {3288.6f, 0.412f, 0},
    {3381.8f, 0.379f, 0}, {3487.7f, 0.323f, 0}, {3660.3f, 1.000f, 1}, {4041.1f, 0.404f, 0},
    {4475.4f, 0.196f, 0}, {4987.8f, 0.197f, 0}, {5239.9f, 0.194f, 0}, {5496.6f, 0.386f, 0},
    {5784.2f, 0.329f, 0}, {6087.3f, 0.923f, 1}, {6416.1f, 0.428f, 0}, {6771.3f, 0.593f, 0},
    {7124.4f, 1.000f, 1}, {7496.1f, 0.435f, 0}, {7870.5f, 0.361f, 0}, {8278.6f, 0.330f, 0},
    {8731.3f, 0.633f, 0}, {9751.0f, 0.146f, 0}, {10795.0f, 0.192f, 0}, {13331.0f, 0.170f, 0},
};
/* kHwOpenHatSpec, kHwClosedHatSpec (6W6's owner's own 606, measured), kCymbalSpec */
static const d6_hat_spec_t D6_OPEN_HAT = {
    D6_HAT_PARTIALS, 6000.0f, 19000.0f, 0.180f, 0.500f, 0.45f, 1.63f,
    0.0003f, 0.35f, 0.003f, 0.70f, 0.040f,
    0.05f, 0.0125f, 0.200f, 1, 1.5085f,
    0.004f, 0.050f, 0.500f, 0.0007f, 0.020f,
};
static const d6_hat_spec_t D6_CLOSED_HAT = {
    D6_HAT_PARTIALS, 7600.0f, 16000.0f, 0.324f, 0.595f, 0.80f, 1.75f,
    0.0003f, 0.35f, 0.003f, 0.70f, 0.040f,
    0.85f, 0.01125f, 0.0625f, 1, 0.2080f,
    0.004f, 0.028f, 0.045f, 0.0007f, 0.020f,
};
static const d6_hat_spec_t D6_CYMBAL = {
    D6_CYM_PARTIALS, 3400.0f, 16000.0f, 0.420f, 1.800f, 0.60f, 1.30f,
    0.0006f, 0.22f, 0.004f, 0.55f, 0.120f,
    0.750f, 0.0225f, 0.225f, 1, 1.4710f,
    0.006f, 0.070f, 0.650f, 0.0011f, 0.026f,
};
#define S6MT_FULL_RATIO 0.40f                     /* kFullLevelSampleRateRatio */
#define S6MT_MUTED_RATIO 0.48f                    /* kMutedSampleRateRatio */
#define S6MT_RETUNE 32u
#define S6MT_RETUNE_STEP 5.65685425f              /* sqrt(32) */

static void s6mt_init(d6_metal_t *m, uint32_t seed)
{
    uint32_t s = seed ? seed : 0x606606u;
    m->phase_rng = s6_seed(s ^ 0x9E3779B9u);
    m->wob_rng = s6_seed(s ^ 0x51D3B7A1u);
    s6_wn_init(&m->noise, s ^ 0xA511E9B3u);
    m->on = 0;
    m->n = 0;
}

static void s6mt_trigger(d6_metal_t *m, const d6_hat_spec_t *sp, float decay_pct, float pitch)
{
    float decay = s6_clamp(decay_pct, 0.0f, 1.0f), r = s6_clamp(pitch, 1.0f / 16.0f, 16.0f), sc, dur, f;
    int i;
    m->n = D6_PARTIALS;
    m->bell = 0;
    for (i = 0; i < D6_PARTIALS; i++) {
        float ratio = sp->p[i].hz * r / S6_SR, level = 1.0f, ph, w;
        if (ratio >= S6MT_MUTED_RATIO)
            level = 0.0f;
        else if (ratio > S6MT_FULL_RATIO) {
            float tp = (S6MT_MUTED_RATIO - ratio) / (S6MT_MUTED_RATIO - S6MT_FULL_RATIO);
            level = tp * tp * (3.0f - 2.0f * tp);
        }
        ph = (s6_bip(&m->phase_rng) + 1.0f) * 0.3f * S6_PI;   /* close enough to land as one hit */
        w = m->inc[i] = level > 0.0f ? S6_2PI * ratio : 0.0f;
        m->amp[i] = sp->p[i].amp * level;
        if (sp->p[i].bell)
            m->bell |= 1u << i;
        m->k[i] = 2.0f * fm_sinf(w * 0.5f);
        m->x[i] = fm_sinf(ph);
        m->y[i] = fm_cosf(ph + w * 0.5f);   /* (cos(p) here would scale the line by 1/cos(w/2)) */
        m->wob[i] = 0.0f;
    }
    m->bell_amt = fm_maxf(0.0f, sp->bell);
    m->bell_k = s6_decay_k(fm_maxf(0.005f, sp->bell_t));
    m->bell_env = 1.0f;
    m->click_amt = fm_maxf(0.0f, sp->click);
    m->click_k = s6_decay_k(fm_maxf(0.0005f, sp->click_t));
    m->click_env = 1.0f;
    s6_bq_set(&m->n_hp, S6_HP, sp->n_hp * r, 0.70f);
    s6_bq_set(&m->n_lp, S6_LP, sp->n_lp * r, 0.707f);
    s6_bq_reset(&m->n_hp);
    s6_bq_reset(&m->n_lp);
    m->dc.x1 = m->dc.y1 = 0.0f;
    m->tonal = sp->tonal;
    m->noise_mix = sp->noise;
    m->sat = fm_maxf(0.0f, sp->sat);
    m->trim = sp->trim;
    if (sp->wob_depth > 0.0f) {
        m->wob_alpha = 1.0f - s6_decay_k(fm_maxf(0.005f, sp->wob_corr));
        m->wob_drive = sp->wob_depth * fm_sqrtf(6.0f * m->wob_alpha);
    } else
        m->wob_alpha = m->wob_drive = 0.0f;
    sc = sp->scales ? decay : 1.0f;
    m->fast_k = s6_decay_k(fm_maxf(sp->min_decay, sc * sp->fast_t));
    m->slow_k = s6_decay_k(fm_maxf(sp->min_decay, sc * sp->slow_t));
    m->fast_w = s6_clamp(sp->fast_w, 0.0f, 1.0f);
    m->fast_env = m->slow_env = 1.0f;
    m->att_k = 1.0f - s6_decay_k(sp->att_t);
    m->att_env = 0.0f;
    dur = fm_maxf(sp->min_dur, decay * sp->ref_dur);
    f = dur * S6_SR + 0.5f;
    m->frames = (uint32_t)f;
    if (!m->frames)
        m->frames = 1;
    m->fade_frames = (uint32_t)(sp->gate_fade * S6_SR + 0.5f);
    if (m->fade_frames > m->frames)
        m->fade_frames = m->frames;
    m->inv_fade = m->fade_frames ? 1.0f / (float)m->fade_frames : 0.0f;
    m->frame = 0;
    m->on = 1;
}

static float s6mt_process(d6_metal_t *m)
{
    float bell_gain, plain = 0.0f, bells = 0.0f, noise, env, src, out;
    int i;
    if (!m->on || m->frame >= m->frames) {
        m->on = 0;
        return 0.0f;
    }
    if (m->wob_drive > 0.0f && (m->frame % S6MT_RETUNE) == 0u) {   /* the slow walk, once per 32 samples */
        float step = m->wob_drive * S6MT_RETUNE_STEP, dk = m->wob_alpha * (float)S6MT_RETUNE;
        for (i = 0; i < m->n; i++) {
            m->wob[i] += step * s6_bip(&m->wob_rng) - dk * m->wob[i];
            m->k[i] = 2.0f * fm_sinf(m->inc[i] * (1.0f + m->wob[i]) * 0.5f);
        }
    }
    bell_gain = 1.0f + m->bell_amt * m->bell_env;
    m->bell_env = s6_flush(m->bell_env * m->bell_k);
    for (i = 0; i < m->n; i++) {
        float nx = m->x[i] + m->k[i] * m->y[i];
        m->y[i] -= m->k[i] * nx;
        m->x[i] = nx;
        if (m->bell & (1u << i))
            bells += nx * m->amp[i];
        else
            plain += nx * m->amp[i];
    }
    noise = s6_bq_fl(&m->n_lp, s6_bq(&m->n_hp, s6_wn(&m->noise)));
    m->att_env += m->att_k * (1.0f - m->att_env);
    m->fast_env = s6_flush(m->fast_env * m->fast_k);
    m->slow_env = s6_flush(m->slow_env * m->slow_k);
    env = m->att_env * (m->fast_w * m->fast_env + (1.0f - m->fast_w) * m->slow_env);
    if (m->fade_frames) {
        uint32_t left = m->frames - m->frame;
        if (left < m->fade_frames)
            env *= (float)left * m->inv_fade;
    }
    src = (plain + bells * bell_gain) * m->tonal + noise * m->noise_mix;
    if (m->sat > 0.0f)
        src = fm_tanhf(src * m->sat);
    out = src * env + noise * m->click_amt * m->click_env;
    m->click_env = s6_flush(m->click_env * m->click_k);
    out = s6_flush(s6_dc(&m->dc, out * m->trim));
    if (++m->frame >= m->frames)
        m->on = 0;
    return out;
}

/* ---- sd606_clap_voice.h (6W6's fork of Clap.hpp) ------------------------
 * Gaussian-ish noise through four fitted 192-tap colour filters, gated by the
 * 606's four short bursts, a long terminal tail, a snap and two floors. */
#include "drum606_clap.h"   /* the four colour filters (generated from 6W6's tables) */

#define S6CP_DUR 0.301f
#define S6CP_TRIM 0.1793169072680733f
#define S6CP_SAT 3.0f
#define S6CP_AIR_HZ 3000.0f
#define S6CP_AIR_DENSITY 0.75f
#define S6CP_TERMINAL 0.03278f
#define S6CP_SNAP 0.032778f
#define S6CP_FOUNDATION 0.165f
#define S6CP_FLOOR 0.250f
static const float S6CP_ONSET[4] = {0.0f, 0.010737f, 0.021735f, 0.032778f};
static const float S6CP_LEVEL[4] = {2.00f, 1.85f, 1.16f, 0.46f};
static const float S6CP_T60[4] = {0.0125f, 0.014f, 0.019f, 0.025f};

static inline float s6cp_gauss(uint32_t *s)
{
    float sum = 0.0f;
    int i;
    for (i = 0; i < 6; i++)
        sum += (float)(s6_xs(s) >> 8) * (2.0f / 16777215.0f) - 1.0f;
    return sum * 0.70710678f;
}

static inline float s6cp_ramp(float t, float a, float b)   /* raisedCosineRamp */
{
    return t <= a ? 0.0f : t >= b ? 1.0f : 0.5f - 0.5f * fm_cosf(S6_PI * (t - a) / (b - a));
}

static inline float s6cp_gate(float t, float onset, float att, float hold, float t60)   /* raisedSineGate */
{
    float age = t - onset, g = 1.0f;
    if (age < 0.0f)
        return 0.0f;
    if (att > 0.0f && age < att) {
        float s = fm_sinf(0.5f * S6_PI * age / att);
        g = s * s;
    }
    return g * fm_expf(-S6_T60 * fm_maxf(age - att - hold, 0.0f) / fm_maxf(t60, 1.0e-6f));
}

static float s6cp_cubic(const float *src, float pos)   /* CompressedSpectralCurve::cubicSample */
{
    int i = (int)fm_floorf(pos);
    float t = pos - (float)i, p[4];
    int j;
    for (j = 0; j < 4; j++) {
        int k = i - 1 + j;
        p[j] = k >= 0 && k < D6_CLAP_TAPS ? src[k] : 0.0f;
    }
    return 0.5f * (2.0f * p[1] + (-p[0] + p[2]) * t + (2.0f * p[0] - 5.0f * p[1] + 4.0f * p[2] - p[3]) * t * t +
                   (-p[0] + 3.0f * p[1] - 3.0f * p[2] + p[3]) * t * t * t);
}

/* the colour bank for a source step (tune up: the filters squeezed, energy kept) */
static void s6cp_configure(d6_cp_t *c, float step)
{
    int ch, tap;
    if (step < 1.0f)
        step = 1.0f;
    if (step == 1.0f) {
        c->taps = D6_CLAP_TAPS;
        for (ch = 0; ch < 4; ch++)
            for (tap = 0; tap < D6_CLAP_TAPS; tap++)
                c->coef[ch * D6_CLAP_TAPS + tap] = D6_CLAP_COLOR[ch][tap];
    } else {
        float fn = (float)D6_CLAP_TAPS / step;
        uint32_t n = (uint32_t)fn;
        if ((float)n < fn)
            n++;
        c->taps = n < 1u ? 1u : n > D6_CLAP_TAPS ? D6_CLAP_TAPS : n;
        for (ch = 0; ch < 4; ch++) {
            float *dst = &c->coef[ch * D6_CLAP_TAPS], src_e = 0.0f, raw_e = 0.0f, g;
            for (tap = 0; tap < D6_CLAP_TAPS; tap++)
                src_e += D6_CLAP_COLOR[ch][tap] * D6_CLAP_COLOR[ch][tap];
            for (tap = 0; tap < (int)c->taps; tap++) {
                dst[tap] = s6cp_cubic(D6_CLAP_COLOR[ch], (float)tap * step);
                raw_e += dst[tap] * dst[tap];
            }
            g = fm_sqrtf(src_e / fm_maxf(raw_e, 1.0e-20f));
            for (tap = 0; tap < (int)c->taps; tap++)
                dst[tap] *= g;
        }
    }
    for (tap = 0; tap < 2 * D6_CLAP_TAPS; tap++)
        c->hist[tap] = 0.0f;
    c->at = 0;
}

static inline void s6cp_push(d6_cp_t *c, float x)
{
    x = s6_flush(x);
    c->hist[c->at] = x;
    c->hist[c->at + c->taps] = x;
    c->at = c->at + 1u == c->taps ? 0u : c->at + 1u;
}

static inline float s6cp_fir(const d6_cp_t *c, int ch)
{
    const float *h = c->hist + (c->at + c->taps - 1u), *k = &c->coef[ch * D6_CLAP_TAPS];
    float a = 0.0f;
    uint32_t i, n = c->taps;
    for (i = 0; i < n; i++)
        a += k[i] * h[-(int32_t)i];
    return s6_flush(a);
}

/* ClapCore::process: one frame at the core's own rate. A colour filter runs only
 * while something uses it: OPENING for the bursts and the snap (until they are
 * under -140 dB), HANDOFF until the fast colour has taken over, FAST until the
 * late one has, LATE from its crossfade on. */
static d6_cf_t s6cp_core(d6_cp_t *c)
{
    d6_cf_t f;
    float t = (float)c->core_idx / c->rate, decay = c->decay, dds = 1.0f + 0.45f * c->air;
    float open_end = S6CP_TERMINAL + 0.0005f + 3.0f * 0.025f * decay;
    float op = 0.0f, ho = 0.0f, fa = 0.0f, la = 0.0f, out = 0.0f, dry = 0.0f, ta = t - S6CP_TERMINAL;
    int b;
    s6cp_push(c, s6cp_gauss(&c->rng));
    if (t < open_end)
        op = s6cp_fir(c, 0);
    if (ta >= 0.0f && t < 0.080f)
        ho = s6cp_fir(c, 1);
    if (t >= 0.055f && t < 0.170f)
        fa = s6cp_fir(c, 2);
    if (t >= 0.110f)
        la = s6cp_fir(c, 3);
    if (t < open_end) {
        float sh = 0.0f;
        for (b = 0; b < 4; b++)
            sh += S6CP_LEVEL[b] * s6cp_gate(t, S6CP_ONSET[b], b ? 0.00016f : 0.0f, 0.00030f, S6CP_T60[b] * decay);
        dry = op * sh;
    }
    out = dry;
    if (ta >= 0.0f) {
        float att = 1.0f, da = fm_maxf(ta - 0.00020f - 0.019f, 0.0f), env, fm, lm, col;
        if (ta < 0.00020f) {
            float s = fm_sinf(0.5f * S6_PI * ta / 0.00020f);
            att = s * s;
        }
        env = 0.72f * att *
              (0.955f * fm_expf(-S6_T60 * da / (0.225f * decay * dds)) +
               0.045f * fm_expf(-S6_T60 * da / (0.890f * decay * dds)));
        fm = s6cp_ramp(t, 0.055f, 0.080f);
        col = (1.0f - fm) * ho + fm * fa;
        lm = s6cp_ramp(t, 0.110f, 0.170f);
        col = (1.0f - lm) * col + lm * la;
        out += env * col;
    }
    if (t < open_end) {
        float snap = 0.55f * op * s6cp_gate(t, S6CP_SNAP, 0.00012f, 0.00035f, 0.025f * decay);
        out += snap;
        dry += snap;
    }
    if (t >= S6CP_FOUNDATION) {
        out += 0.022f * decay * la * s6cp_gate(t, S6CP_FOUNDATION, 0.020f, 0.0f, 0.760f * decay * dds);
        if (t >= S6CP_FLOOR)
            out += 0.004f * decay * la * s6cp_gate(t, S6CP_FLOOR, 0.015f, 0.0f, 0.760f * decay * dds);
    }
    c->core_idx++;
    f.full = s6_flush(out);
    f.dry = s6_flush(dry);
    return f;
}

static void s6cp_init(d6_cp_t *c, uint32_t seed)
{
    c->rng = seed ? seed : 0x6D2B79F5u;
    c->on = 0;
}

static void s6cp_trigger(d6_cp_t *c, float decay_pct, float pitch, float noise)
{
    float r = s6_clamp(fm_fabsf(pitch), 0.5f, 2.0f), want = 44100.0f * r, core;
    int i;
    noise = s6_clamp(noise, 0.0f, 1.0f);
    c->decay = s6_clamp(decay_pct, 0.05f, 1.0f);
    c->noise_gain = 2.0f * fm_minf(noise, 0.5f);
    c->air = noise > 0.5f ? 2.0f * (noise - 0.5f) : 0.0f;
    core = fm_minf(want, S6_SR);
    c->step = core / S6_SR;
    c->rate = core;
    s6cp_configure(c, want / core);
    for (i = 0; i < D6_CLAP_TAPS; i++)   /* prewarm */
        s6cp_push(c, s6cp_gauss(&c->rng));
    c->core_idx = 0;
    if (c->step != 1.0f) {   /* pitched down: the core at its own rate, read back with a cubic */
        c->f[0].full = c->f[0].dry = 0.0f;
        for (i = 1; i < 4; i++)
            c->f[i] = s6cp_core(c);
        c->pos = 0.0f;
    }
    c->air_k = fm_expf(-S6_2PI * fm_minf(S6CP_AIR_HZ, 0.35f * S6_SR) / S6_SR);
    c->air_in = c->air_out = 0.0f;
    c->max = (uint32_t)(S6_SR * S6CP_DUR);
    if ((float)c->max < S6_SR * S6CP_DUR)
        c->max++;
    c->idx = 0;
    c->on = 1;
}

static inline float s6cp_hermite(float p0, float p1, float p2, float p3, float t)
{
    return 0.5f * (2.0f * p1 + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
}

static float s6cp_process(d6_cp_t *c)
{
    d6_cf_t raw;
    float time, fade = 1.0f, full, cal, sat;
    if (!c->on)
        return 0.0f;
    if (c->step == 1.0f)
        raw = s6cp_core(c);
    else {
        float t = c->pos;
        raw.full = s6cp_hermite(c->f[0].full, c->f[1].full, c->f[2].full, c->f[3].full, t);
        raw.dry = s6cp_hermite(c->f[0].dry, c->f[1].dry, c->f[2].dry, c->f[3].dry, t);
        c->pos += c->step;
        while (c->pos >= 1.0f) {
            c->pos -= 1.0f;
            c->f[0] = c->f[1];
            c->f[1] = c->f[2];
            c->f[2] = c->f[3];
            c->f[3] = s6cp_core(c);
        }
    }
    time = (float)c->idx / S6_SR;
    if (time > 0.290f) {
        float fp = fm_minf((time - 0.290f) / (S6CP_DUR - 0.290f), 1.0f), fb = 0.5f + 0.5f * fm_cosf(S6_PI * fp);
        fade = fb * fb * fb * fb;
    }
    full = raw.full * fade;
    cal = S6CP_SAT * fm_tanhf(full * (1.0f / S6CP_SAT));
    sat = cal;
    if (c->air > 0.0f || c->noise_gain != 1.0f) {
        float dry = S6CP_SAT * fm_tanhf(raw.dry * fade * (1.0f / S6CP_SAT)), nz = cal - dry;
        if (c->air > 0.0f) {
            float lw = s6cp_ramp(time, 0.045f, 0.170f), sp = c->air;
            c->air_out = s6_flush(c->air_k * (c->air_out + nz - c->air_in));
            c->air_in = s6_flush(nz);
            sat = dry + (1.0f + sp * (0.40f + 0.60f * lw)) * nz +
                  sp * (0.20f + 0.55f * lw) * S6CP_AIR_DENSITY * fm_tanhf(2.0f * nz / S6CP_AIR_DENSITY) +
                  sp * (0.15f + 0.50f * lw) * c->air_out;
        } else
            sat = dry + c->noise_gain * nz;
    }
    if (++c->idx >= c->max)
        c->on = 0;
    return s6_flush(sat * S6CP_TRIM);
}

/* ---- sd606_shape.h: the drive stage (9W9's distortion types) ------------ */

static float s6_shape(float x, float drive, int type, float *st)
{
    const float k = drive > 0.01f ? drive : 0.01f;
    switch (type) {
    case 1: {   /* Clip: asymmetric soft clip, even harmonics */
        const float v = (fm_tanhf(x * k + 0.35f) - 0.33638f) * 0.958f;
        return k < 1.0f ? v / k : v;
    }
    case 2: {   /* SAT: warm, parallel, keeps the transient */
        const float u = x * k + 0.08f * k * x * x, wet = u / (1.0f + fm_fabsf(u)), m = k < 1.0f ? k : 1.0f;
        const float v = (1.0f - 0.65f * m) * x * (k < 1.0f ? k : 1.0f) + 0.65f * m * wet * 1.35f;
        return k < 1.0f ? v / k : v;
    }
    case 3: {   /* BFZ: thick fuzz wall, faded in over drive 0.85 .. 2 */
        const float u = x * k * 2.5f + 0.22f, wet = (u / (1.0f + fm_fabsf(u)) - 0.18033f) * 1.05f;
        const float m = s6_clamp((k - 0.85f) / 1.15f, 0.0f, 1.0f);
        return (1.0f - m) * x + m * wet;
    }
    case 4: {   /* PDIST: biased cubic crunch */
        const float u = s6_clamp(x * k + 0.12f, -1.0f, 1.0f), y0 = 0.12f - (0.12f * 0.12f * 0.12f) / 3.0f;
        return ((u - u * u * u / 3.0f) - y0) * (1.5f / 1.479f);
    }
    case 5: {   /* Fold: wavefolder */
        float v = x * k, body = s6_clamp(x * k, -1.0f, 1.0f);
        int i;
        for (i = 0; i < 3; i++) {
            if (v > 1.0f)
                v = 2.0f - v;
            if (v < -1.0f)
                v = -2.0f - v;
        }
        v = 0.62f * v + 0.38f * body;
        return k < 1.0f ? v / k : v;
    }
    case 6: {   /* Crush: quantise and decimate */
        const float steps = 1.5f + 9.0f / k, hold = k < 1.0f ? 1.0f : 1.0f + (k - 1.0f) * 1.7f;
        const float q = fm_floorf(x * steps + 0.5f) / steps;
        st[1] += 1.0f;
        if (st[1] >= hold) {
            st[1] -= hold;
            st[0] = q;
        }
        return st[0];
    }
    default:    /* Diode: the machine's back-to-back diode rounding */
        return fm_tanhf(k * x) / fm_tanhf(k);
    }
}

/* ---- sd606_engine.cpp: pots, kit balance, velocity, choke, render ------- */

#define D6_FULL_VEL_GAIN 1.9921260f   /* a full-velocity hit: the old Accent pot's default */
#define D6_DRIVE_WET_POT 8            /* drive 0 is exactly dry; the stage fades in up to pot 8 */
#define D6_CHOKE_STEP (1.0f / (0.002f * 44100.0f))   /* a 2 ms fade, not a click */

/* each pot: min, max, exponential?, default (sd606_params.h); a 0 max is "not a pot of this drum" */
typedef struct { float min, max; uint8_t exp, def; } d6_pot_t;
#define S6P_LVL {0.0f, 2.0f, 0, 64}
#define S6P_DRV {0.85f, 12.0f, 1, 8}
#define S6P_DST {0.0f, 6.0f, 0, 0}
#define S6P_SND {0.0f, 1.0f, 0, 0}
#define S6P_TUN {0.5f, 2.0f, 1, 64}
#define S6P_NONE {0.0f, 0.0f, 0, 0}
static const d6_pot_t D6_POT[D6_NUM][D6P_NUM] = {
    /*          LEVEL  TUNE                         DECAY                   DRIVE  DIST   REV    DLY    X1 / X2 */
    [D6_BD] = {S6P_LVL, {-9.78f, 14.22f, 0, 40}, {0.0f, 1.0f, 0, 24}, S6P_DRV, S6P_DST, S6P_SND, S6P_SND, {0.0f, 1.0f, 0, 120}, S6P_NONE},
    [D6_SD] = {S6P_LVL, {0.5f, 2.0f, 1, 70}, {0.0f, 1.0f, 0, 76}, S6P_DRV, S6P_DST, S6P_SND, S6P_SND, {0.0f, 1.0f, 0, 64}, {0.5f, 2.0f, 1, 52}},
    [D6_LT] = {S6P_LVL, S6P_TUN, {0.0f, 1.0f, 0, 124}, S6P_DRV, S6P_DST, S6P_SND, S6P_SND, S6P_NONE, S6P_NONE},
    [D6_HT] = {S6P_LVL, S6P_TUN, {0.0f, 1.0f, 0, 124}, S6P_DRV, S6P_DST, S6P_SND, S6P_SND, S6P_NONE, S6P_NONE},
    [D6_CH] = {S6P_LVL, S6P_TUN, {0.0f, 1.0f, 0, 102}, S6P_DRV, S6P_DST, S6P_SND, S6P_SND, S6P_NONE, S6P_NONE},
    [D6_OH] = {S6P_LVL, S6P_TUN, {0.0f, 1.0f, 0, 102}, S6P_DRV, S6P_DST, S6P_SND, S6P_SND, S6P_NONE, S6P_NONE},
    [D6_CY] = {S6P_LVL, S6P_TUN, {0.0f, 1.0f, 0, 102}, S6P_DRV, S6P_DST, S6P_SND, S6P_SND, S6P_NONE, S6P_NONE},
    [D6_CP] = {S6P_LVL, S6P_TUN, {0.0f, 1.0f, 0, 102}, S6P_DRV, S6P_DST, S6P_SND, S6P_SND, {0.0f, 1.0f, 0, 64}, S6P_NONE},
};
#undef S6P_LVL
#undef S6P_DRV
#undef S6P_DST
#undef S6P_SND
#undef S6P_TUN
#undef S6P_NONE
static const uint8_t D6_KIT_DEF[D6K_NUM] = {127, 1};

/* the kit balance at pot centre (6W6's measured trims) */
static const float D6_TRIM[D6_NUM] = {0.751f, 0.446f, 0.308f, 0.309f, 0.183f, 0.251f, 0.203f, 0.460f};

static float d6_value(int drum, int slot, int pot)
{
    const d6_pot_t *p = &D6_POT[drum][slot];
    float t = (float)pot * (1.0f / 127.0f);
    if (p->exp && p->min > 0.0f)
        return p->min * fm_powf(p->max / p->min, t);
    return p->min + (p->max - p->min) * t;
}

int drum606_default(int drum, int slot)
{
    if (drum == D6_KIT)
        return slot >= 0 && slot < D6K_NUM ? D6_KIT_DEF[slot] : 0;
    return drum >= 0 && drum < D6_NUM && slot >= 0 && slot < D6P_NUM ? D6_POT[drum][slot].def : 0;
}

void drum606_set(drum606_t *d, int drum, int slot, int v)
{
    if (drum == D6_KIT) {
        if (slot == D6K_VEL)
            d->kit[slot] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
        else if (slot == D6K_CHOKE)
            d->kit[slot] = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v);
        d->vel_depth = (float)d->kit[D6K_VEL] * (1.0f / 127.0f);
        return;
    }
    if (drum < 0 || drum >= D6_NUM || slot < 0 || slot >= D6P_NUM)
        return;
    v = v < 0 ? 0 : v > (slot == D6P_DIST ? D6_DIST_TYPES - 1 : 127) ? (slot == D6P_DIST ? D6_DIST_TYPES - 1 : 127) : v;
    d->pot[drum][slot] = (uint8_t)v;
    switch (slot) {
    case D6P_LEVEL: d->level[drum] = d6_value(drum, slot, v) * D6_TRIM[drum]; break;
    case D6P_DRIVE: d->drive[drum] = d6_value(drum, slot, v); break;
    case D6P_REV: d->rev[drum] = d6_value(drum, slot, v); break;
    case D6P_DLY: d->dly[drum] = d6_value(drum, slot, v); break;
    default: break;
    }
}

int drum606_get(const drum606_t *d, int drum, int slot)
{
    if (drum == D6_KIT)
        return slot >= 0 && slot < D6K_NUM ? d->kit[slot] : 0;
    return drum >= 0 && drum < D6_NUM && slot >= 0 && slot < D6P_NUM ? d->pot[drum][slot] : 0;
}

void drum606_init(drum606_t *d)
{
    int v, s;
    for (v = 0; v < D6_NUM; v++) {
        for (s = 0; s < D6P_NUM; s++)
            drum606_set(d, v, s, D6_POT[v][s].def);
        d->rt[v].hit = 1.0f;
        d->rt[v].choke = 1.0f;
        d->rt[v].choke_step = 0.0f;
        d->rt[v].crush[0] = d->rt[v].crush[1] = 0.0f;
    }
    for (s = 0; s < D6K_NUM; s++)
        drum606_set(d, D6_KIT, s, D6_KIT_DEF[s]);
    /* distinct seeds, so one voice's per-hit variation does not move in lockstep with another's */
    s6bd_init(&d->bd, 0x6060u);
    s6sd_init(&d->sd, 0x6063u);
    s6tom_init(&d->lt, 0x6061u);
    s6tom_init(&d->ht, 0x6062u);
    s6mt_init(&d->ch, 0x6064u);
    s6mt_init(&d->oh, 0x6065u);
    s6mt_init(&d->cy, 0x6066u);
    s6cp_init(&d->cp, 0x0606C1A9u);
}

static int d6_on(const drum606_t *d, int v)
{
    switch (v) {
    case D6_BD: return d->bd.on;
    case D6_SD: return d->sd.on;
    case D6_LT: return d->lt.on;
    case D6_HT: return d->ht.on;
    case D6_CH: return d->ch.on;
    case D6_OH: return d->oh.on;
    case D6_CY: return d->cy.on;
    default: return d->cp.on;
    }
}

int drum606_active(const drum606_t *d)
{
    int v;
    for (v = 0; v < D6_NUM; v++)
        if (d6_on(d, v) && d->rt[v].choke > 0.0f)
            return 1;
    return 0;
}

static void d6_choke(drum606_t *d, int v)
{
    if (d->rt[v].choke > 0.0f && d->rt[v].choke_step == 0.0f)
        d->rt[v].choke_step = -D6_CHOKE_STEP;
}

/* velocity: a full-velocity hit reaches the accent level; the kit's velocity depth is how far softer hits fall */
void drum606_trigger(drum606_t *d, int v, int vel)
{
    const uint8_t *p;
    float tune, decay;
    if (v < 0 || v >= D6_NUM || vel <= 0)
        return;
    vel = vel > 127 ? 127 : vel;
    d->rt[v].hit = D6_FULL_VEL_GAIN * (1.0f - d->vel_depth * (1.0f - (float)vel * (1.0f / 127.0f)));
    d->rt[v].choke = 1.0f;
    d->rt[v].choke_step = 0.0f;
    d->rt[v].crush[0] = d->rt[v].crush[1] = 0.0f;
    /* the 606's closed hat cuts its open hat; here a switch (the cymbal stays out of it) */
    if (v == D6_CH && d->kit[D6K_CHOKE] >= 1)
        d6_choke(d, D6_OH);
    if (v == D6_OH && d->kit[D6K_CHOKE] == 2)
        d6_choke(d, D6_CH);
    p = d->pot[v];
    tune = d6_value(v, D6P_TUNE, p[D6P_TUNE]);
    decay = d6_value(v, D6P_DECAY, p[D6P_DECAY]);
    switch (v) {
    case D6_BD: s6bd_trigger(&d->bd, d6_value(v, D6P_X1, p[D6P_X1]), decay, tune); break;
    case D6_SD: s6sd_trigger(&d->sd, decay, tune, d6_value(v, D6P_X1, p[D6P_X1]), d6_value(v, D6P_X2, p[D6P_X2])); break;
    case D6_LT: s6tom_trigger(&d->lt, &D6_LOW_TOM, decay, tune); break;
    case D6_HT: s6tom_trigger(&d->ht, &D6_HIGH_TOM, decay, tune); break;
    case D6_CH: s6mt_trigger(&d->ch, &D6_CLOSED_HAT, decay, tune); break;
    case D6_OH: s6mt_trigger(&d->oh, &D6_OPEN_HAT, decay, tune); break;
    case D6_CY: s6mt_trigger(&d->cy, &D6_CYMBAL, decay, tune); break;
    default: s6cp_trigger(&d->cp, decay, tune, d6_value(v, D6P_X1, p[D6P_X1])); break;
    }
}

static inline float d6_raw(drum606_t *d, int v)
{
    switch (v) {
    case D6_BD: return s6bd_process(&d->bd);
    case D6_SD: return s6sd_process(&d->sd);
    case D6_LT: return s6tom_process(&d->lt);
    case D6_HT: return s6tom_process(&d->ht);
    case D6_CH: return s6mt_process(&d->ch);
    case D6_OH: return s6mt_process(&d->oh);
    case D6_CY: return s6mt_process(&d->cy);
    default: return s6cp_process(&d->cp);
    }
}

/* one drum, one block, through its own drive stage, level, accent and choke fade; post-fader sends */
static void d6_lane(drum606_t *d, int v, float *dry, float *rev, float *dly, int n)
{
    d6_rt_t *r = &d->rt[v];
    const int dpot = d->pot[v][D6P_DRIVE], type = d->pot[v][D6P_DIST];
    const float drive = d->drive[v], wet = (float)dpot * (1.0f / (float)D6_DRIVE_WET_POT);
    const float g = d->level[v] * r->hit, sr = d->rev[v], sl = d->dly[v];
    int i;
    for (i = 0; i < n; i++) {
        float x, y;
        if (r->choke_step < 0.0f) {
            r->choke += r->choke_step;
            if (r->choke <= 0.0f) {
                r->choke = 0.0f;
                r->choke_step = 0.0f;
            }
        }
        if (r->choke <= 0.0f)
            return;   /* choked out: silent (and frozen) until its next hit, as 6W6 */
        x = d6_raw(d, v);
        y = x;
        if (dpot > 0) {
            y = s6_shape(x, drive, type, r->crush);
            if (dpot < D6_DRIVE_WET_POT)
                y = x + wet * (y - x);
        }
        y *= g * r->choke;
        dry[i] += y;
        rev[i] += y * sr;
        dly[i] += y * sl;
    }
}

void drum606_render(drum606_t *d, float *dry, float *rev, float *dly, int n)
{
    int v;
    for (v = 0; v < D6_NUM; v++)
        if (d6_on(d, v) && d->rt[v].choke > 0.0f)
            d6_lane(d, v, dry, rev, dly, n);
}
