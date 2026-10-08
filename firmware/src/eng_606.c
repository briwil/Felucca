/* SPDX-License-Identifier: GPL-3.0-only */
/* 606: Charles Vestal's 6W6 TR-606 (Matthew Fecher's 606-Inspired-Synth-Drums voices, AudioKit Pro, MIT), ported
 * to C in x0x/drum606.c, as a kit engine (core.h kit_if_t, kit.c).
 *
 * Eight drums, the 606's own seven plus 6W6's clap: BD SD LT HT CH OH CY CP. Each drum's parameters are 6W6's
 * (level, tune, decay, its extra ones, drive and distortion type, its reverb and delay sends), on the DRUM pages;
 * the kit's LEVEL, ACC (velocity depth) and CHOKE are EDIT 1.
 *
 * Notes: 6W6's General MIDI map (35/36 BD, 38/40 SD, 41/43/45 LT, 47/48/50 HT, 42/44 CH, 46 OH, 49/51/57 CY,
 * 39 CP). The keys play the eight drums in that order from the first C (k606_keys). */
#include "x0x/drum606.c"

static void *kit_mem(uint32_t part);   /* kit.c */
static drum606_t *k606(uint32_t part) { return (drum606_t *)kit_mem(part); }

static const kit_lane_t K606_LANES[D6_NUM] = {
    {"BD", "KICK"}, {"SD", "SNARE"}, {"LT", "LOTOM"}, {"HT", "HITOM"},
    {"CH", "HATCL"}, {"OH", "HATOP"}, {"CY", "CYMB"}, {"CP", "CLAP"},
};

typedef struct { const char *label; uint8_t slot; } k606_p_t;
#define KP_TAIL {"DRIVE", D6P_DRIVE}, {"DIST", D6P_DIST}, {"REV", D6P_REV}, {"DLY", D6P_DLY}
static const k606_p_t K606_BD[] = {{"LEVEL", D6P_LEVEL}, {"TUNE", D6P_TUNE}, {"DECY", D6P_DECAY}, {"ATCK", D6P_X1},
                                   KP_TAIL};
static const k606_p_t K606_SD[] = {{"LEVEL", D6P_LEVEL}, {"TUNE", D6P_TUNE}, {"DECY", D6P_DECAY}, {"SNAP", D6P_X1},
                                   {"TONE", D6P_X2}, KP_TAIL};
static const k606_p_t K606_TD[] = {{"LEVEL", D6P_LEVEL}, {"TUNE", D6P_TUNE}, {"DECY", D6P_DECAY}, KP_TAIL};
static const k606_p_t K606_CP[] = {{"LEVEL", D6P_LEVEL}, {"TUNE", D6P_TUNE}, {"DECY", D6P_DECAY}, {"NOISE", D6P_X1},
                                   KP_TAIL};
#undef KP_TAIL
static const struct { const k606_p_t *p; uint8_t n; } K606_DRUM[D6_NUM] = {
    [D6_BD] = {K606_BD, NELEM(K606_BD)}, [D6_SD] = {K606_SD, NELEM(K606_SD)},
    [D6_LT] = {K606_TD, NELEM(K606_TD)}, [D6_HT] = {K606_TD, NELEM(K606_TD)},
    [D6_CH] = {K606_TD, NELEM(K606_TD)}, [D6_OH] = {K606_TD, NELEM(K606_TD)},
    [D6_CY] = {K606_TD, NELEM(K606_TD)}, [D6_CP] = {K606_CP, NELEM(K606_CP)},
};

static const char *const K606_DIST[D6_DIST_TYPES] = {"DIODE", "CLIP", "SAT", "BFZ", "PDIST", "FOLD", "CRUSH"};
static kit_param_t k606_desc[D6_NUM][KIT_PARAMS];
static volatile uint8_t k606_made;   /* (as k808_made) */
static void k606_tables(void)
{
    uint32_t v, i;
    if (k606_made)
        return;
    for (v = 0; v < D6_NUM; v++)
        for (i = 0; i < K606_DRUM[v].n; i++) {
            uint32_t slot = K606_DRUM[v].p[i].slot;
            k606_desc[v][i].label = K606_DRUM[v].p[i].label;
            k606_desc[v][i].max = slot == D6P_DIST ? D6_DIST_TYPES - 1u : 127u;
            k606_desc[v][i].def = drum606_default((int)v, (int)slot);
            k606_desc[v][i].names = slot == D6P_DIST ? K606_DIST : 0;
        }
    k606_made = 1;
}
static uint32_t k606_nparams(uint32_t lane) { return lane < D6_NUM ? K606_DRUM[lane].n : 0u; }
static const kit_param_t *k606_param(uint32_t lane, uint32_t i)
{
    k606_tables();
    return &k606_desc[lane % D6_NUM][i % KIT_PARAMS];
}

static const uint8_t K606_NOTE[D6_NUM] = {36, 38, 45, 50, 42, 46, 49, 39};
static int32_t k606_lane_of(uint32_t note)
{
    switch (note) {
    case 35: case 36: return D6_BD;
    case 38: case 40: return D6_SD;
    case 41: case 43: case 45: return D6_LT;
    case 47: case 48: case 50: return D6_HT;
    case 42: case 44: return D6_CH;
    case 46: return D6_OH;
    case 49: case 51: case 57: return D6_CY;
    case 39: return D6_CP;
    default: return -1;
    }
}

static void k606_init(uint32_t part) { drum606_init(k606(part)); }
static void k606_set(uint32_t part, uint32_t lane, uint32_t i, int32_t v)
{
    if (lane < D6_NUM && i < K606_DRUM[lane].n)
        drum606_set(k606(part), (int)lane, (int)K606_DRUM[lane].p[i].slot, (int)v);
}
static int32_t k606_get(uint32_t part, uint32_t lane, uint32_t i)
{
    return lane < D6_NUM && i < K606_DRUM[lane].n
               ? drum606_get(k606(part), (int)lane, (int)K606_DRUM[lane].p[i].slot) : 0;
}
static void k606_trigger(uint32_t part, uint32_t lane, uint32_t vel)
{
    drum606_trigger(k606(part), (int)lane, (int)vel);
}

/* the kit's LEVEL (100 = 6W6's level), ACC and CHOKE from EDIT 1 (P_E0..P_E2) */
static int k606_render(uint32_t part, float *dry, float *rev, float *dly, uint32_t n)
{
    drum606_t *d = k606(part);
    const int16_t *p = trk[part].p;
    float g;
    uint32_t i;
    if (drum606_get(d, D6_KIT, D6K_VEL) != p[P_E1])
        drum606_set(d, D6_KIT, D6K_VEL, p[P_E1]);
    if (drum606_get(d, D6_KIT, D6K_CHOKE) != p[P_E2])
        drum606_set(d, D6_KIT, D6K_CHOKE, p[P_E2]);
    if (!drum606_active(d))
        return 0;
    drum606_render(d, dry, rev, dly, (int)n);
    g = (float)p[P_E0] * 0.01f;
    if (g != 1.0f)
        for (i = 0; i < n; i++) {
            dry[i] *= g;
            rev[i] *= g;
            dly[i] *= g;
        }
    return 1;
}

static const uint8_t K606_ORDER[D6_NUM] = {D6_BD, D6_SD, D6_CH, D6_OH, D6_CP, D6_LT, D6_HT, D6_CY};
static const kit_if_t KIT_606 = {
    .nlanes = D6_NUM,
    .gain = 1.3f,                /* (the snare, hats and clap about the 808's; the 606 kick a little hotter) */
    .lanes = K606_LANES,
    .nparams = k606_nparams,
    .param = k606_param,
    .lane_of = k606_lane_of,
    .notes = K606_NOTE,
    .order = K606_ORDER,
    .init = k606_init,
    .set = k606_set,
    .get = k606_get,
    .trigger = k606_trigger,
    .render = k606_render,
};

/* the keys: the eight drums from the first C (key 7), the other keys silent */
static int32_t k606_keys(const track_t *t, uint32_t k)
{
    (void)t;
    return k >= 7u && k < 7u + D6_NUM ? (int32_t)K606_NOTE[k - 7u] : 0;
}

static const preset_t K606_PRESETS[] = {
    {"606 KIT", {100, 127, 1, 0, 0, 0, 0, 0}, {0, 100, 127, 100}, 0, 0, FX(0, 0, 0, 0), PAT(12)},
};

#define K606_NONE {"-", F_INT, 0, 0, 0, 0, 0}
static const char *const K606_CHOKE[] = {"OFF", "CH>OH", "BOTH"};
static const engine_t ENG_606 = {
    .name = "606",
    .page_title = {"KIT", "-"},
    .edit = {
        {"LEVEL", F_PCT, 0, 127, 100, 0, 0},
        {"ACC", F_PCT, 0, 127, 127, 0, 0},
        {"CHOKE", F_ENUM, 0, 2, 1, K606_CHOKE, 0},
        K606_NONE, K606_NONE, K606_NONE, K606_NONE, K606_NONE,
    },
    .presets = K606_PRESETS,
    .npresets = NELEM(K606_PRESETS),
    .note_on = kit_voice_on,
    .render = kit_voice_render,
    .knob = {P_E0, P_E1, P_LEVEL, P_REV},
    .oneshot = 1,
    .keys = k606_keys,
    .kit = &KIT_606,
};
#undef K606_NONE
