/* SPDX-License-Identifier: GPL-3.0-only */
/* 808: the TR-808 of the X0X firmware (Charles Vestal; x0x/drum808.c: 8W8's circuit models, the rim shot after
 * Yoshinosuke Horiuchi's sc808, MIT) as a kit engine (core.h kit_if_t, kit.c).
 *
 * Sixteen drums, every one its own: BD SD LT MT HT, the congas LC MC HC, RS, claves CL, maracas MA, CP, CB, CH,
 * OH, CY. On the 808 itself a tom and its conga (and RS / CL, CP / MA) share a channel and a switch picks one; here
 * each is a drum of its own (the pair still shares its voice, as on the 808: one cuts the other). Each drum's
 * parameters are the 8W8 panel's (level, tune, decay, its extra ones, drive and distortion type, its reverb and delay
 * sends), on the DRUM pages; the kit's LEVEL, ACC (velocity depth) and CHOKE are EDIT 1.
 *
 * Notes: the General MIDI map (36 BD, 38/40 SD, 41/43 LT, 45/47 MT, 48/50 HT, 64 LC, 63 MC, 62 HC, 37 RS, 75 CL,
 * 70 MA, 39 CP, 56 CB, 42/44 CH, 46 OH, 49/57 CY). The keys play the sixteen drums in that order from the first C
 * (k808_keys). The drum grid's lanes are BD SD CP CH OH MT RS CB (eng_drum.c DRUM_LANE_NOTE). Pan is left out: a
 * part is mono until it pans (fx.c mix_part). */
#include "x0x/drum808.c"

static void *kit_mem(uint32_t part);   /* kit.c */
static drum808_t *k808(uint32_t part) { return (drum808_t *)kit_mem(part); }

static const kit_lane_t K808_LANES[D8S_NUM] = {
    {"BD", "KICK"}, {"SD", "SNARE"}, {"LT", "LOTOM"}, {"MT", "MDTOM"}, {"HT", "HITOM"},
    {"LC", "LOCNG"}, {"MC", "MDCNG"}, {"HC", "HICNG"}, {"RS", "RIM"}, {"CL", "CLAVE"},
    {"MA", "MARAC"}, {"CP", "CLAP"}, {"CB", "COWBL"}, {"CH", "HATCL"}, {"OH", "HATOP"}, {"CY", "CYMB"},
};

/* a drum's parameters: the pot slot each one is, in 8W8's panel order (the Sound switches and Pan left out) */
typedef struct { const char *label; uint8_t slot; } k808_p_t;
#define KP_COMMON {"DRIVE", D8P_DRIVE}, {"DIST", D8P_DIST}, {"REV", D8P_REV}, {"DLY", D8P_DLY}
static const k808_p_t K808_BD[] = {{"LEVEL", D8P_LEVEL}, {"TONE", D8P_X1}, {"DECY", D8P_DECAY}, {"TUNE", D8P_TUNE},
                                   {"ATCK", D8P_X2}, {"DRIVE", D8P_DRIVE}, {"DIST", D8P_DIST}};   /* (no sends: 8W8) */
static const k808_p_t K808_SD[] = {{"LEVEL", D8P_LEVEL}, {"TONE", D8P_X2}, {"SNAP", D8P_X1}, {"TUNE", D8P_TUNE},
                                   {"DECY", D8P_DECAY}, KP_COMMON};
static const k808_p_t K808_TD[] = {{"LEVEL", D8P_LEVEL}, {"TUNE", D8P_TUNE}, {"DECY", D8P_DECAY}, KP_COMMON};
static const k808_p_t K808_MA[] = {{"LEVEL", D8P_LEVEL}, {"TUNE", D8P_TUNE}, {"DECY", D8P_DECAY}, {"ATCK", D8P_X1},
                                   KP_COMMON};
static const k808_p_t K808_DT[] = {{"LEVEL", D8P_LEVEL}, {"DECY", D8P_DECAY}, {"TUNE", D8P_TUNE}, KP_COMMON};
#undef KP_COMMON
static const struct { const k808_p_t *p; uint8_t n, track, alt; } K808_SOUND[D8S_NUM] = {
    [D8S_BD] = {K808_BD, NELEM(K808_BD), D8_BD, 0}, [D8S_SD] = {K808_SD, NELEM(K808_SD), D8_SD, 0},
    [D8S_LT] = {K808_TD, NELEM(K808_TD), D8_LT, 0}, [D8S_MT] = {K808_TD, NELEM(K808_TD), D8_MT, 0},
    [D8S_HT] = {K808_TD, NELEM(K808_TD), D8_HT, 0}, [D8S_LC] = {K808_TD, NELEM(K808_TD), D8_LT, 1},
    [D8S_MC] = {K808_TD, NELEM(K808_TD), D8_MT, 1}, [D8S_HC] = {K808_TD, NELEM(K808_TD), D8_HT, 1},
    [D8S_RS] = {K808_TD, NELEM(K808_TD), D8_RS, 0}, [D8S_CL] = {K808_TD, NELEM(K808_TD), D8_RS, 1},
    [D8S_MA] = {K808_MA, NELEM(K808_MA), D8_CP, 1}, [D8S_CP] = {K808_TD, NELEM(K808_TD), D8_CP, 0},
    [D8S_CB] = {K808_TD, NELEM(K808_TD), D8_CB, 0}, [D8S_CH] = {K808_TD, NELEM(K808_TD), D8_CH, 0},
    [D8S_OH] = {K808_DT, NELEM(K808_DT), D8_OH, 0}, [D8S_CY] = {K808_DT, NELEM(K808_DT), D8_CY, 0},
};

/* the descriptors (made once: the default is 8W8's for that drum and slot, x0x/drum808.c k_spec) */
static kit_param_t k808_desc[D8S_NUM][KIT_PARAMS];
static volatile uint8_t k808_made;   /* set last: the UI and the audio code may both be first; they write the same */
static void k808_tables(void)
{
    uint32_t s, i;
    if (k808_made)
        return;
    for (s = 0; s < D8S_NUM; s++)
        for (i = 0; i < K808_SOUND[s].n; i++) {
            uint32_t slot = K808_SOUND[s].p[i].slot;
            k808_desc[s][i].label = K808_SOUND[s].p[i].label;
            k808_desc[s][i].max = slot == D8P_DIST ? 6u : 127u;
            k808_desc[s][i].def = k_spec[s][slot].def;
            k808_desc[s][i].names = slot == D8P_DIST ? k_dist_names : 0;
        }
    k808_made = 1;
}
static uint32_t k808_nparams(uint32_t lane) { return lane < D8S_NUM ? K808_SOUND[lane].n : 0u; }
static const kit_param_t *k808_param(uint32_t lane, uint32_t i)
{
    k808_tables();
    return &k808_desc[lane % D8S_NUM][i % KIT_PARAMS];
}

/* each drum's note (its GM drum), for the keys; and every GM note 35..81 -> the drum (-1: none) */
static const uint8_t K808_NOTE[D8S_NUM] = {36, 38, 41, 45, 48, 64, 63, 62, 37, 75, 70, 39, 56, 42, 46, 49};
static int32_t k808_lane_of(uint32_t note)
{
    static const int8_t GM[47] = {
        D8S_BD, D8S_BD, D8S_RS, D8S_SD, D8S_CP, D8S_SD, D8S_LT, D8S_CH, D8S_LT, D8S_CH, D8S_MT, D8S_OH,   /* 35 */
        D8S_MT, D8S_HT, D8S_CY, D8S_HT, D8S_CY, D8S_CY, -1, -1, D8S_CY, D8S_CB, D8S_CY, -1,   /* 47 */
        D8S_CY, -1, -1, D8S_HC, D8S_MC, D8S_LC, -1, -1, -1, -1, -1, D8S_MA,   /* 59 */
        -1, -1, -1, -1, D8S_CL, -1, -1, -1, -1, -1, -1,   /* 71 */
    };
    return note >= 35u && note <= 81u ? GM[note - 35u] : -1;
}

static void k808_init(uint32_t part) { drum808_init(k808(part)); }
static void k808_set(uint32_t part, uint32_t lane, uint32_t i, int32_t v)
{
    if (lane < D8S_NUM && i < K808_SOUND[lane].n) {
        uint32_t slot = K808_SOUND[lane].p[i].slot;
        set_pot(k808(part), (int)lane, (int)slot, clamp(v, 0, slot == D8P_DIST ? 6 : 127));
    }
}
static int32_t k808_get(uint32_t part, uint32_t lane, uint32_t i)
{
    return lane < D8S_NUM && i < K808_SOUND[lane].n ? k808(part)->pot[lane][K808_SOUND[lane].p[i].slot] : 0;
}
/* vel 1..127: 100 is an unaccented 808 hit (D8_VEL_NORMAL), from 120 the accent */
static void k808_trigger(uint32_t part, uint32_t lane, uint32_t vel)
{
    drum808_t *d = k808(part);
    float v = vel >= 120u ? 1.0f : D8_VEL_NORMAL * (float)vel * 0.01f;
    d->sw[K808_SOUND[lane].track] = K808_SOUND[lane].alt;   /* (the channel's switch: this drum) */
    drum808_trigger(d, K808_SOUND[lane].track, v > 1.0f ? 1.0f : v);
}

/* the kit's LEVEL, ACC and CHOKE from EDIT 1 (P_E0..P_E2) */
static int k808_render(uint32_t part, float *dry, float *rev, float *dly, uint32_t n)
{
    drum808_t *d = k808(part);
    const int16_t *p = trk[part].p;
    uint32_t i;
    for (i = 0; i < 3u; i++)
        if (drum808_get(d, D8_KIT, (int)i) != p[P_E0 + i])
            drum808_set(d, D8_KIT, (int)i, p[P_E0 + i]);
    if (!drum808_active(d))
        return 0;
    drum808_render(d, dry, rev, dly, (int)n);
    return 1;
}

static const kit_if_t KIT_808 = {
    .nlanes = D8S_NUM,
    .gain = 2.2f,                /* (8W8's kit balance is ~10 dB under Felucca's parts) */
    .lanes = K808_LANES,
    .nparams = k808_nparams,
    .param = k808_param,
    .lane_of = k808_lane_of,
    .init = k808_init,
    .set = k808_set,
    .get = k808_get,
    .trigger = k808_trigger,
    .render = k808_render,
};

/* the keys: the sixteen drums from the first C (key 7), the other keys silent (note 0: no drum) */
static int32_t k808_keys(const track_t *t, uint32_t k)
{
    (void)t;
    return k >= 7u && k < 7u + D8S_NUM ? (int32_t)K808_NOTE[k - 7u] : 0;
}

static const preset_t K808_PRESETS[] = {
    {"808 KIT", {100, 127, 1, 0, 0, 0, 0, 0}, {0, 100, 127, 100}, 0, 0, FX(0, 0, 0, 0), PAT(12)},
};

#define K808_NONE {"-", F_INT, 0, 0, 0, 0, 0}
static const char *const K808_CHOKE[] = {"OFF", "CH>OH", "BOTH"};
static const engine_t ENG_808 = {
    .name = "808",
    .page_title = {"KIT", "-"},
    .edit = {
        {"LEVEL", F_PCT, 0, 127, 100, 0, 0},
        {"ACC", F_PCT, 0, 127, 127, 0, 0},
        {"CHOKE", F_ENUM, 0, 2, 1, K808_CHOKE, 0},
        K808_NONE, K808_NONE, K808_NONE, K808_NONE, K808_NONE,
    },
    .presets = K808_PRESETS,
    .npresets = NELEM(K808_PRESETS),
    .note_on = kit_voice_on,
    .render = kit_voice_render,
    .knob = {P_E0, P_E1, P_LEVEL, P_REV},
    .oneshot = 1,
    .keys = k808_keys,
    .kit = &KIT_808,
};
#undef K808_NONE
