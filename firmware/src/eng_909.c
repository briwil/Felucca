/* SPDX-License-Identifier: GPL-3.0-only */
/* 909: the TR-909 of the X0X firmware (Charles Vestal; x0x/drum909.c: 9W9's models, grown out of ER-99 by
 * Matthew Cieplak, all GPL-3.0) as a kit engine (core.h kit_if_t, kit.c).
 *
 * Eleven drums: BD SD LT MT HT RS CP CH OH CR RD, circuit-modelled except the hats and cymbals (ER-99's samples,
 * kept as 6-bit block floating point: x0x/drum909.c d9_bfp6). Every drum has its own parameters (the 9W9 panel:
 * tune, decay, level, drive and distortion type, its reverb and delay sends, ..), edited on the DRUM pages; the
 * kit's ACC and VEL (accent, velocity depth) are EDIT 1.
 *
 * Notes: the General MIDI drum map (36 BD, 38 SD, 41/43 LT, 45/47 MT, 48/50 HT, 37 RS, 39 CP, 42/44 CH, 46 OH,
 * 49/52/55/57 CR, 51/53/59 RD; 56, the grid's BELL lane, the crash). The keys play it from the first C.
 * Pan is left out: a part is mono until it pans (fx.c mix_part). */
#include "x0x/drum909.c"

static void *kit_mem(uint32_t part);   /* kit.c */
/* the 909 in a part's kit memory (kit.c kit_mem) */
static drum909_t *k909(uint32_t part) { return (drum909_t *)kit_mem(part); }

static const kit_lane_t K909_LANES[DR_NUM] = {
    {"BD", "KICK"}, {"SD", "SNARE"}, {"LT", "LO TOM"}, {"MT", "MID TOM"}, {"HT", "HI TOM"},
    {"RS", "RIM"}, {"CP", "CLAP"}, {"CH", "HAT CL"}, {"OH", "HAT OP"}, {"CR", "CRASH"}, {"RD", "RIDE"},
};

/* the panel's labels in the font's capitals, <= 5 characters */
static const char *const K909_DIST[] = {"DIODE", "CLIP", "SAT", "BFZ", "PDIST", "FOLD", "CRUSH"};
static const struct { const char *x0x, *label; } K909_LABEL[] = {
    {"Tune", "TUNE"}, {"Attack", "ATCK"}, {"Decay", "DECY"}, {"Level", "LEVEL"}, {"P.Dpth", "SWEEP"},
    {"Pitch", "SWTIM"}, {"Drive", "DRIVE"}, {"Dist", "DIST"}, {"Tone", "TONE"}, {"Snappy", "SNAP"},
    {"Rev", "REV"}, {"Dly", "DLY"}, {"Tail", "TAIL"}, {"Accent", "ACC"}, {"Veloc", "VEL"},
};
static int k909_same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* a drum's parameters as the DRUM pages show them: the 909's own, without Pan (the 909's index of each) */
static uint8_t k909_map[DR_NUM][KIT_PARAMS], k909_n[DR_NUM];
static kit_param_t k909_desc[DR_NUM][KIT_PARAMS];
static void k909_tables(void)
{
    uint32_t v, i, j;
    if (k909_n[0])
        return;
    for (v = 0; v < DR_NUM; v++) {
        uint32_t n = 0;
        for (i = 0; i < (uint32_t)drum909_nparams((int)v) && n < KIT_PARAMS; i++) {
            const x0x_param_t *p = drum909_param((int)v, (int)i);
            const char *lb = 0;
            if (k909_same(p->name, "Pan"))
                continue;
            for (j = 0; j < NELEM(K909_LABEL); j++)
                if (k909_same(p->name, K909_LABEL[j].x0x))
                    lb = K909_LABEL[j].label;
            k909_map[v][n] = (uint8_t)i;
            k909_desc[v][n].label = lb ? lb : "-";
            k909_desc[v][n].max = p->max;
            k909_desc[v][n].def = p->def;
            k909_desc[v][n].names = p->max < 127 ? (p->max + 1u == NELEM(K909_DIST) ? K909_DIST : 0) : 0;
            n++;
        }
        k909_n[v] = (uint8_t)n;
    }
}

static uint32_t k909_nparams(uint32_t lane)
{
    k909_tables();
    return lane < DR_NUM ? k909_n[lane] : 0u;
}
static const kit_param_t *k909_param(uint32_t lane, uint32_t i)
{
    k909_tables();
    return &k909_desc[lane % DR_NUM][i % KIT_PARAMS];
}

/* General MIDI 35..59 -> the drum (-1: none) */
static const int8_t K909_GM[25] = {
    DR_BD, DR_BD, DR_RS, DR_SD, DR_CP, DR_SD, DR_LT, DR_CH, DR_LT, DR_CH, DR_MT, DR_OH,   /* 35 .. 46 */
    DR_MT, DR_HT, DR_CR, DR_HT, DR_RD, DR_CR, DR_RD, -1, DR_CR, DR_CR, DR_CR, DR_RD, -1,  /* 47 .. 59 */
};
static int32_t k909_lane_of(uint32_t note)
{
    if (note >= 35u && note <= 59u)
        return K909_GM[note - 35u];
    return -1;
}

static void k909_init(uint32_t part) { drum909_init(k909(part)); }
static void k909_set(uint32_t part, uint32_t lane, uint32_t i, int32_t v)
{
    if (lane < DR_NUM && i < k909_n[lane])
        drum909_set(k909(part), (int)lane, k909_map[lane][i], v);
}
static int32_t k909_get(uint32_t part, uint32_t lane, uint32_t i)
{
    return lane < DR_NUM && i < k909_n[lane] ? drum909_get(k909(part), (int)lane, k909_map[lane][i]) : 0;
}
static void k909_trigger(uint32_t part, uint32_t lane, uint32_t vel)
{
    drum909_trigger(k909(part), (int)lane, (float)vel * (1.0f / 127.0f));
}

/* the kit's own ACC and VEL from EDIT 1 (P_E0, P_E1) */
static int k909_render(uint32_t part, float *dry, float *rev, float *dly, uint32_t n)
{
    drum909_t *d = k909(part);
    const int16_t *p = trk[part].p;
    if (drum909_get(d, DR_KIT, 0) != p[P_E0])
        drum909_set(d, DR_KIT, 0, p[P_E0]);
    if (drum909_get(d, DR_KIT, 1) != p[P_E1])
        drum909_set(d, DR_KIT, 1, p[P_E1]);
    if (!drum909_active(d))
        return 0;
    drum909_render(d, dry, rev, dly, (int)n);
    return 1;
}

static const kit_if_t KIT_909 = {
    .nlanes = DR_NUM,
    .lanes = K909_LANES,
    .nparams = k909_nparams,
    .param = k909_param,
    .lane_of = k909_lane_of,
    .init = k909_init,
    .set = k909_set,
    .get = k909_get,
    .trigger = k909_trigger,
    .render = k909_render,
};

/* the engine (voice.c never plays it: kit_note takes its notes) */
static void k909_note_on(track_t *t, voice_t *v) { (void)t; v->active = 0; }
static void k909_voice(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    (void)t; (void)v; (void)out; (void)n; (void)m;
}
static int32_t k909_keys(const track_t *t, uint32_t k)
{
    (void)t;
    return clamp(29 + 12 * song.octave + (int32_t)k, 0, 127);   /* (DRUM's map: the first C is the kick) */
}

static const preset_t K909_PRESETS[] = {
    {"909 KIT", {42, 127, 0, 0, 0, 0, 0, 0}, {0, 100, 127, 100}, 0, 0, FX(0, 0, 0, 0), PAT(12)},
};

#define K909_NONE {"-", F_INT, 0, 0, 0, 0, 0}
static const engine_t ENG_909 = {
    .name = "909",
    .page_title = {"KIT", "-"},
    .edit = {
        {"ACC", F_PCT, 0, 127, 42, 0, 0},
        {"VEL", F_PCT, 0, 127, 127, 0, 0},
        K909_NONE, K909_NONE, K909_NONE, K909_NONE, K909_NONE, K909_NONE,
    },
    .presets = K909_PRESETS,
    .npresets = NELEM(K909_PRESETS),
    .note_on = k909_note_on,
    .render = k909_voice,
    .knob = {P_E0, P_E1, P_LEVEL, P_REV},
    .oneshot = 1,
    .keys = k909_keys,
    .kit = &KIT_909,
};
#undef K909_NONE
