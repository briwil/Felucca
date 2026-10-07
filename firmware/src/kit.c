/* SPDX-License-Identifier: GPL-3.0-only */
/* Kit engines (core.h kit_if_t): the glue between Felucca's parts and a drum machine that renders all its
 * drums at once (eng_909.c). Included at the end of engines.c.
 *
 * Parameters: kit_pot[track][drum][i], the knob values of every drum (0..max, the module's own "pots"). The UI
 * edits them (the DRUM pages, params.c SC_KIT); the audio code hands every change to the engine at the start of
 * its next block (kit_sync), so the engine's state is only ever touched from the audio context.
 *
 * Memory: a kit's state lives in its part's PHYS slots (eng_phys.c phys_slot[part]): a part plays one engine at a
 * time, and voice.c engine_block only switches once the old engine is silent. kit_live says whose state is there;
 * leaving a kit zeroes the memory, which is the state PHYS starts from at power-on.
 *
 * Floating point: the kit engines are float (as on the X0X). Only the audio context runs them (the main loop's
 * code is integer), so the audio interrupt needs no FPU registers saved. */

static int16_t kit_pot[NTRK][KIT_LANES][KIT_PARAMS] __attribute__((section(".pool")));
static uint8_t kit_live[NPART];  /* the engine index + 1 whose state is in the part's kit memory, 0 = zeroed */
static uint8_t kit_sel[NTRK];    /* the drum the DRUM pages edit (the last one played on the track) */
static uint8_t kit_pot_of[NTRK]; /* the engine index + 1 whose parameters kit_pot holds, 0 = none yet */

#define KIT_MEM_SIZE sizeof(phys_slot[0])
static void *kit_mem(uint32_t part) { return (void *)phys_slot[part % NPART]; }

/* the kit of the engine the track asked for (the UI), and of the one it plays (the audio context) */
static const kit_if_t *track_kit(const track_t *t) { return ENGINES[eng_idx(t->eng_req)]->kit; }
static const kit_if_t *kit_playing(const track_t *t) { return ENGINES[eng_idx(t->engine)]->kit; }

/* every drum's parameters to the kit's defaults (a preset load, a new engine; e: the engine) */
static void kit_defaults_of(track_t *t, uint32_t e)
{
    const kit_if_t *k = ENGINES[eng_idx(e)]->kit;
    uint32_t ti = (uint32_t)(t - trk) % NTRK, l, i;
    if (!k)
        return;
    kit_pot_of[ti] = (uint8_t)(eng_idx(e) + 1u);
    for (l = 0; l < KIT_LANES; l++)
        for (i = 0; i < KIT_PARAMS; i++)
            kit_pot[ti][l][i] = l < k->nlanes && i < k->nparams(l) ? (int16_t)k->param(l, i)->def : 0;
    if (kit_sel[ti] >= k->nlanes)
        kit_sel[ti] = 0;
}
static void kit_defaults(track_t *t) { kit_defaults_of(t, t->eng_req); }

/* the drum kit_sel names, kept inside the track's kit */
static uint32_t kit_lane_sel(const track_t *t)
{
    const kit_if_t *k = track_kit(t);
    uint32_t ti = (uint32_t)(t - trk) % NTRK;
    return k && kit_sel[ti] < k->nlanes ? kit_sel[ti] : 0u;
}

/* voice.c engine_block, after a switch: a kit's memory is zeroed when its part plays something else */
static void kit_switched(track_t *t)
{
    uint32_t part = (uint32_t)(t - trk) % NPART;
    if (kit_live[part] && kit_live[part] != t->engine + 1u) {
        memset(kit_mem(part), 0, KIT_MEM_SIZE);
        kit_live[part] = 0;
    }
}

/* the part's kit state, made when it is not there yet; 0 = the part plays no kit */
static const kit_if_t *kit_ready(track_t *t)
{
    const kit_if_t *k = kit_playing(t);
    uint32_t part = (uint32_t)(t - trk) % NPART, l, i;
    if (!k)
        return 0;
    if (kit_pot_of[part] != t->engine + 1u)           /* parameters of another engine, or none yet: defaults */
        kit_defaults_of(t, t->engine);
    if (kit_live[part] != t->engine + 1u) {
        memset(kit_mem(part), 0, KIT_MEM_SIZE);
        k->init(part);
        for (l = 0; l < k->nlanes; l++)
            for (i = 0; i < k->nparams(l); i++)
                k->set(part, l, i, kit_pot[part][l][i]);
        kit_live[part] = (uint8_t)(t->engine + 1u);
    }
    return k;
}

/* the UI's changes to the engine (a block start) */
static void kit_sync(track_t *t, const kit_if_t *k)
{
    uint32_t part = (uint32_t)(t - trk) % NPART, l, i;
    for (l = 0; l < k->nlanes; l++)
        for (i = 0; i < k->nparams(l); i++) {
            int32_t v = kit_pot[part][l][i];
            if (v != k->get(part, l, i))
                k->set(part, l, i, v);
        }
}

/* voice.c trk_note_on: a note on a kit part strikes its drum (no voice) */
static void kit_note(track_t *t, uint32_t note, uint32_t vel)
{
    const kit_if_t *k = kit_ready(t);
    int32_t lane;
    if (!k)
        return;
    lane = k->lane_of(note);
    if (lane >= 0)
        k->trigger((uint32_t)(t - trk) % NPART, (uint32_t)lane, vel < 1u ? 1u : vel > 127u ? 127u : vel);
}

/* seq.c input_on: a drum played from the keys or MIDI IN is the one the DRUM pages edit */
static void kit_played(track_t *t, uint32_t note)
{
    const kit_if_t *k = track_kit(t);
    int32_t lane = k ? k->lane_of(note) : -1;
    if (lane >= 0)
        kit_sel[(uint32_t)(t - trk) % NTRK] = (uint8_t)lane;
}

/* fx.c mix_part: one block of the kit into out (Felucca's part scale) and its own sends into rev / dly (the
 * same scale, before the part's LEVEL); returns 1 while it sounds. A float of 1.0 is KIT_FS: about the level
 * of Felucca's DRUM kit at the same LEVEL */
#define KIT_FS 36864.0f
#define KIT_MAXF 10.0f                                  /* (a float far out of range: clipped, no int overflow) */
static float kit_fdry[CTL], kit_frev[CTL], kit_fdly[CTL];
static int32_t kit_rev[CTL], kit_dly[CTL];
static inline int32_t kit_int(float x)
{
    x = x > KIT_MAXF ? KIT_MAXF : x < -KIT_MAXF ? -KIT_MAXF : x;
    return (int32_t)(x * KIT_FS);
}
static int kit_render(track_t *t, int32_t *out, uint32_t n)
{
    const kit_if_t *k = kit_ready(t);
    uint32_t i;
    int on;
    for (i = 0; i < n; i++)
        out[i] = kit_rev[i] = kit_dly[i] = 0;
    if (!k)
        return 0;
    kit_sync(t, k);
    for (i = 0; i < n; i++)
        kit_fdry[i] = kit_frev[i] = kit_fdly[i] = 0.0f;
    on = k->render((uint32_t)(t - trk) % NPART, kit_fdry, kit_frev, kit_fdly, n);
    if (!on)
        return 0;
    for (i = 0; i < n; i++) {
        out[i] = kit_int(kit_fdry[i]);
        kit_rev[i] = kit_int(kit_frev[i]);
        kit_dly[i] = kit_int(kit_fdly[i]);
    }
    return 1;
}

/* the drum a grid lane of a kit track plays (its GM note, eng_drum.c DRUM_LANE_NOTE), 0 = not a kit */
static const kit_lane_t *kit_grid_lane(const track_t *t, uint32_t l)
{
    const kit_if_t *k = track_kit(t);
    int32_t lane = k ? k->lane_of(DRUM_LANE_NOTE[l % NLANE]) : -1;
    return lane >= 0 ? &k->lanes[lane] : 0;
}

/* params.c page_desc, the DRUM pages: parameter i of the selected drum as a descriptor and its value; 0 = none */
static const param_desc_t *kit_page_desc(track_t *t, uint32_t i, int16_t **valp)
{
    static param_desc_t dd[KIT_PARAMS];
    param_desc_t *dp;
    const kit_if_t *k = track_kit(t);
    uint32_t lane = kit_lane_sel(t);
    const kit_param_t *kp;
    *valp = 0;
    if (!k || i >= k->nparams(lane) || i >= KIT_PARAMS)
        return 0;
    kp = k->param(lane, i);
    dp = &dd[i];                                      /* (one per column: the four are drawn together) */
    dp->label = kp->label;
    dp->fmt = kp->names ? F_ENUM : F_PCT;
    dp->min = 0;
    dp->max = kp->max;
    dp->def = kp->def;
    dp->names = kp->names;
    dp->unit = 0;
    *valp = &kit_pot[(uint32_t)(t - trk) % NTRK][lane][i];
    return dp;
}

/* the DRUM pages: shown for a kit engine, as many as the selected drum's parameters need (4 a page) */
static int kit_page_visible(const track_t *t, uint32_t first)
{
    const kit_if_t *k = track_kit(t);
    return k && first < k->nparams(kit_lane_sel(t));
}
