/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Keyboard, scale, arpeggiator, sequencer and transport.
 * Everything here runs in the audio ISR, once per CTL-sample block, and
 * ends in trk_note_on / trk_note_off: engines never see where a note came from.
 * Four tracks, one transport: every track's pattern loops on its own LEN / DIV /
 * SWING / GATE (polymeter). The keys play the selected track; MIDI IN by GLO > SYSTEM ROUT (G_ROUTE):
 * CH1-4 (0) channels 1..4 play parts 1..4, channels 5..16 are ignored (midi_control.c midi_event); SEL (1)
 * every channel the selected track. Their CC1 (mod wheel), CC11 (expression) and channel aftertouch go to
 * the same track's modulation matrix (mod.c; the selected track: the one selected then).
 * A note into an armed track (song.rec) while
 * the transport runs is recorded into its pattern, quantised to its (swung) steps, with its
 * held length as TIE steps (rec_note, rec_hold, rec_release). With the ARP on, the notes the arp
 * plays are recorded, not the keys held (what plays back is what was heard). A key or MIDI note plays a
 * chord when the track's CHRD is on (chord.c): its notes go through the same input as so many keys. */
static const uint16_t SCALE_MASK[] = {
    0xFFF,                                   /* CHR */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),   /* MAJ */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),   /* MIN */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),   /* DOR */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),   /* MIX */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),                          /* PEN */
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),                         /* MPEN */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),   /* HARM */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),   /* PHRY */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),   /* LYD */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10),   /* LOC */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),   /* MEL (ascending) */
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),              /* BLUES (minor) */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 8) | (1 << 10),              /* WHOLE */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 10), /* DIMHW */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 9) | (1 << 11), /* DIMWH */
};

#define KB_SILENT 255u
static uint32_t kb_prev;
static uint8_t kb_note[27], kb_trk[27];  /* per key: the note it started and on which track */
static uint8_t last_note = 60;
static volatile uint8_t transport_req;   /* 1 start, 2 stop, 3 restart (from the UI) */
static volatile uint8_t panic_req;       /* bit per track: release every sounding note (preset / engine change) */
static volatile uint8_t midi_hint;       /* MIDI IN played track n - 1, which is not the selected one (the UI says so) */

static uint32_t trk_index(const track_t *t) { return (uint32_t)(t - trk); }

static uint32_t trk_midi_ch(uint32_t i) { return i % NTRK; }   /* MIDI channel 0..15 of track i (keys -> MIDI out) */

static int drum_track(const track_t *t)
{
    const engine_t *e = ENGINES[eng_idx(t->eng_req)];
    return e->kit != 0;                                 /* CESARI: a kit engine (kit.c): the grid (DRUM: notes) */
}

/* the 27 keys from F: black or white, and the key's place among the keys of its colour (white 0..15, black
 * 0..10). The DRUM grid: white keys are steps, black keys 1..8 lanes, 9 ACC, 10 / 11 the page */
static int key_black(uint32_t k) { return (int)((0x54Au >> ((k + 5u) % 12u)) & 1u); }
static uint32_t key_place(uint32_t k)
{
    uint32_t i, n = 0;
    for (i = 0; i < k; i++)
        n += (uint32_t)(key_black(i) == key_black(k));
    return n;
}

static uint32_t scale_mask(const track_t *t)
{
    return SCALE_MASK[clamp(t->p[P_SCALE], 0, sizeof SCALE_MASK / sizeof SCALE_MASK[0] - 1)];
}

/* note n moved down onto the track's scale (ROOT, SCALE): QNT SNAP's keys and QNT SEQ's sequenced notes */
static int32_t scale_snap(const track_t *t, int32_t n)
{
    uint32_t mask = scale_mask(t), guard = 12;
    while (guard-- && !((mask >> (uint32_t)((n - t->p[P_ROOT] + 120) % 12)) & 1u))
        n--;
    return n;
}

enum { QN_OFF, QN_SNAP, QN_WHITE, QN_SEQ };      /* P_QUANT */

static uint32_t kb_map(const track_t *t, uint32_t k)
{
    static const int8_t DEGREE[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    const engine_t *e = ENGINES[eng_idx(t->eng_req)];   /* (the engine it switches to) */
    int32_t n;
    if (e->keys && (n = e->keys(t, k)) >= 0)           /* the engine's own key map (DRUM's GM map, slices) */
        return (uint32_t)n;
    n = 53 + (int32_t)k;
    if (t->p[P_QUANT] == QN_SNAP || t->p[P_QUANT] == QN_SEQ)   /* SNAP (and SEQ): every key, rounded down */
        return (uint32_t)clamp(scale_snap(t, n + 12 * song.octave + t->p[P_TRANS]), 0, 127);
    if (t->p[P_QUANT] == QN_WHITE) {                    /* WHITE: white keys walk the scale, black keys are silent */
        uint32_t mask = scale_mask(t), i;
        int32_t count = 0, degree = DEGREE[n % 12], oct;
        if (degree < 0)
            return KB_SILENT;
        /* C4 is the root. Walk scale degrees on successive white keys, including
         * below C4; scales with 5, 6, 8 or 12 notes still have no duplicated degrees. */
        degree += (n / 12 - 5) * 7;
        for (i = 0; i < 12u; i++)
            count += (mask >> i) & 1u;
        oct = degree / count;
        degree %= count;
        if (degree < 0) {
            degree += count;
            oct--;
        }
        for (i = 0; i < 12u; i++)
            if ((mask >> i) & 1u) {
                if (!degree)
                    break;
                degree--;
            }
        n = 60 + t->p[P_ROOT] + 12 * oct + (int32_t)i;
    }
    return (uint32_t)clamp(n + 12 * song.octave + t->p[P_TRANS], 0, 127);
}

#include "chord.c"

/* ------------------------------------------------------------- arp --- */
static void arp_add(track_t *t, uint32_t note)
{
    uint32_t i;
    if (t->p[P_AHOLD] && t->arp_phys == 0u)
        t->nheld = 0;                               /* new chord replaces the latched one */
    t->arp_phys++;                                  /* every key-down: arp_remove counts every key-up */
    for (i = 0; i < t->nheld; i++)
        if (t->held[i] == note)
            return;                                 /* repeated note-on: not a new note */
    if (t->nheld < 16u)
        t->held[t->nheld++] = (uint8_t)note;
    if (t->nheld == 1u) {
        t->arp_pos = 0xFFFFFFF;                     /* fire on this block */
        t->arp_idx = 0xFFFFFFFFu;
    }
}

static void arp_remove(track_t *t, uint32_t note)
{
    uint32_t i, k = 0;
    if (t->arp_phys)
        t->arp_phys--;
    if (t->p[P_AHOLD])
        return;
    for (i = 0; i < t->nheld; i++)
        if (t->held[i] != note)
            t->held[k++] = t->held[i];
    t->nheld = (uint8_t)k;
}

static void rec_note(track_t *t, uint32_t note, uint32_t vel);
static void rec_release(track_t *t, uint32_t note);
static int rec_on(const track_t *t) { return ((song.rec >> trk_index(t)) & 1u) && song.playing && !chain.running; }

#include "motion.c"

static void arp_off(track_t *t)                  /* the sounding arp note ends (a recorded one too) */
{
    trk_note_off(t, t->arp_note);
    if (rec_on(t))
        rec_release(t, t->arp_note);
    t->arp_note = 0;
}

/* n: samples the arp advances by (the external clock's while it runs, 0 stopped); gate_n: samples its
 * gate counts down by (real time when the external transport stops: a tapped or latched note still ends) */
static volatile uint32_t beat_pos, beat_n;      /* samples into the beat, the beat of the bar (0..3) */
static void arp_step(track_t *t, uint32_t n, uint32_t gate_n)
{
    uint32_t period, cnt, list[64], len = 0, i, j, o;
    int32_t sw;
    if (t->arp_note) {
        if (t->arp_off <= gate_n)
            arp_off(t);
        else
            t->arp_off -= gate_n;
    }
    if (!t->p[P_AMODE] || !t->nheld) {
        if (!t->nheld && t->arp_note)
            arp_off(t);
        return;
    }
    period = div_samples((uint32_t)t->p[P_ARATE]);
    sw = t->p[P_ASWING] * (int32_t)period / 250;
    t->arp_pos += n;
    if (t->arp_pos < period + (uint32_t)((t->arp_idx & 1u) ? sw : -sw) && t->arp_pos != 0xFFFFFFF + n)
        return;
    t->arp_pos = 0;
    /* build the note list: held notes (sorted or as played) over OCT octaves */
    for (i = 0; i < t->nheld; i++)
        list[i] = t->held[i];
    cnt = t->nheld;
    if (!t->p[P_AORDER])
        for (i = 1; i < cnt; i++)
            for (j = i; j > 0 && list[j - 1] > list[j]; j--) {
                uint32_t x = list[j];
                list[j] = list[j - 1];
                list[j - 1] = x;
            }
    for (o = 0; o < (uint32_t)t->p[P_AOCT]; o++)
        for (i = 0; i < cnt && len < 64u; i++)
            list[len++] = clamp((int32_t)list[i] + 12 * (int32_t)o, 0, 127);
    if (t->p[P_AMODE] == 6) { list[0] = t->held[t->nheld - 1u]; len = 1; } /* REPEAT: last played note, no octave traversal */
    t->arp_idx++;
    switch (t->p[P_AMODE]) {
    case 2:
        j = len - 1u - t->arp_idx % len;
        break;
    case 3: {
        uint32_t cyc = len > 1u ? 2u * len - 2u : 1u, k = t->arp_idx % cyc;
        j = k < len ? k : cyc - k;
        break;
    }
    case 4:
        j = rng() % len;
        break;
    default:
        j = t->arp_idx % len;
        break;
    }
    if (t->arp_note)
        arp_off(t);
    if ((uint32_t)(rng() & 127u) <= (uint32_t)t->p[P_APROB]) {
        t->arp_note = (uint8_t)list[j];
        t->arp_off = period * (uint32_t)t->p[P_AGATE] / 128u;
        if (rec_on(t))
            rec_note(t, t->arp_note, 100);           /* recording: what the arp plays */
        trk_note_on(t, t->arp_note, 100);
    }
}

static void arp_tick(track_t *t, uint32_t n) { arp_step(t, n, n); }

/* -------------------------------------------------------- note input --- */
/* the length of step idx in samples: SWING (the track's + the global, at most 100) makes the even steps longer
 * and the odd ones shorter, so every odd step starts late */
static uint32_t step_samples(const track_t *t, uint32_t period, uint32_t idx)
{
    return swing_step_len(t, period, idx);   /* core.h: own + global, at most 100 */
}

/* CESARI: live recording: a note goes into the nearest step of its row (the one playing, or the next one when it
 * is past the middle of the playing one), at the velocity played; an overdub onto a note there takes its place.
 * Held, it grows: its length is the time to its release, rounded to steps (a drum: always one step). A note
 * recorded into the step about to play sounds already: that step does not trigger it again (rskip) */
static uint32_t rec_q8(const track_t *t, uint32_t period)   /* where the sequencer is, in 1/256 steps since PLAY */
{
    uint32_t cur = step_samples(t, period, t->seq_count);
    uint32_t f = cur ? (uint32_t)(((uint64_t)(t->seq_pos < cur ? t->seq_pos : cur) << 8) / cur) : 0u;
    return t->seq_count * 256u + f;
}
static void rec_note(track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t period = div_samples((uint32_t)t->p[P_SDIV]), q = rec_q8(t, period), next = (q & 255u) > 128u;
    uint32_t c = (q >> 8) + next, len = row_len_of(t, &t->pat, note), k;
    int32_t i = pat_add(&t->pat, note, c % len, 1u, vel ? vel : 100u);
    if (i < 0)
        return;                                     /* (the pattern is full) */
    t->pat.note[i].chance = 0;
    t->pat.note[i].nudge = 0;
    t->seq_active = 1;
    if (next) {                                     /* it sounds now: the step must not trigger it again */
        if (t->rskip_at != c)
            t->rskip_n = 0;
        t->rskip_at = c;
        if (t->rskip_n < SEQ_EV)
            t->rskip[t->rskip_n++] = (uint8_t)note;
    }
    if (note_kit(t))
        return;                                     /* (a drum: one step) */
    for (k = 0; k < t->rh_n && t->rh_note[k] != note; k++)
        ;
    if (k == t->rh_n) {
        if (t->rh_n >= 4u)
            return;
        t->rh_n++;
    }
    t->rh_note[k] = (uint8_t)note;
    t->rh_idx[k] = (uint16_t)i;
    t->rh_t0[k] = c * 256u;
}

/* a recorded note's key is up: its length, rounded to steps (at least one, at most its row's length) */
static void rec_release(track_t *t, uint32_t note)
{
    uint32_t k, j, q, len, max;
    note_t *n;
    for (k = 0; k < t->rh_n && t->rh_note[k] != note; k++)
        ;
    if (k == t->rh_n)
        return;
    q = rec_q8(t, div_samples((uint32_t)t->p[P_SDIV]));
    n = &t->pat.note[t->rh_idx[k] % NNOTE];
    if (t->rh_idx[k] < t->pat.n && n->row == note) {
        max = row_len_of(t, &t->pat, note);
        len = q > t->rh_t0[k] ? (q - t->rh_t0[k] + 128u) >> 8 : 1u;
        n->len = (uint8_t)(len < 1u ? 1u : len > max ? max : len);
    }
    for (j = k; j + 1u < t->rh_n; j++) {
        t->rh_note[j] = t->rh_note[j + 1u];
        t->rh_idx[j] = t->rh_idx[j + 1u];
        t->rh_t0[j] = t->rh_t0[j + 1u];
    }
    t->rh_n--;
}

static void input_on(track_t *t, uint32_t note, uint32_t vel)
{
    last_note = (uint8_t)note;
    kit_played(t, note);                               /* a kit: the drum the DRUM pages edit */
    if (rec_on(t) && !t->p[P_AMODE])               /* (ARP on: arp_tick records its notes) */
        rec_note(t, note, vel);
    if (t->p[P_AMODE])
        arp_add(t, note);
    else
        trk_note_on(t, note, vel);
}

static int midi_note_held(const track_t *t, uint32_t note);
static int midi_local_held(const track_t *t, uint32_t note);
static void input_off(track_t *t, uint32_t note)
{
    if (midi_note_held(t, note))
        return;
    rec_release(t, note);
    arp_remove(t, note);                            /* both: the note may have started in the */
    trk_note_off(t, note);                          /* other mode (ARP switched while held) */
}

/* the effect of key k in the FX layer (PF_N none): the first PF_M1 white keys (F3 .. A4) in order, black keys
 * 1..4 (F#3 G#3 A#3 C#4) the track mutes */
static uint32_t perf_key(uint32_t k)
{
    uint32_t p = key_place(k);
    return !key_black(k) ? (p < PF_M1 ? p : PF_N) : p < NTRK ? PF_M1 + p : PF_N;
}

/* key k plays kb_note[k] on track t: its chord (chord.c; the note alone with CHRD OFF). A note another key
 * holds already sounds: it is not started again (nor sent to MIDI OUT); the key keeps its notes */
static void key_on(uint32_t k, track_t *t)
{
    uint32_t n = chord_build(t, kb_note[k], kb_chord[k]), i, mc = trk_midi_ch(trk_index(t));
    kb_chn[k] = 0;
    for (i = 0; i < n; i++) {
        uint32_t x = kb_chord[k][i];
        if (midi_local_held(t, x))
            continue;
        input_on(t, x, 100);
        midi_out_event(0x09u | (0x90u | mc) << 8 | x << 16 | 100u << 24);
    }
    kb_chn[k] = (uint8_t)n;
    last_note = kb_note[k];                         /* (step entry, the SAMPLE zone: the key's note) */
}

/* key k is up: the notes it started end, but those another key still holds */
static void key_off(uint32_t k, track_t *t)
{
    uint32_t n = kb_chn[k], i, mc = trk_midi_ch(trk_index(t));
    kb_chn[k] = 0;
    for (i = 0; i < n; i++) {
        uint32_t x = kb_chord[k][i];
        if (midi_local_held(t, x))
            continue;
        input_off(t, x);
        midi_out_event(0x08u | (0x80u | mc) << 8 | x << 16);
    }
}

#ifdef FM1_INPUT_LAT
/* the press latency stats (hal/fm1_input.h fm1_in_stat, console `inp`): from the first scan that saw
 * key k closed to its note-on here, and to the note's first sample leaving the DMA (kb_out_tick:
 * audio.c, the TIMER4 tick at which the block being rendered starts to play) */
static uint32_t kb_out_tick;
static __attribute__((noinline)) void kb_lat(uint32_t k)
{
    uint32_t t0 = fm1_in.note_t0[k], d = fm1_ticks() - t0, o = kb_out_tick - t0;
    fm1_in_stat.kb_n++;
    fm1_in_stat.kb_sum += d;
    fm1_in_stat.dac_sum += o;
    if (d > fm1_in_stat.kb_max)
        fm1_in_stat.kb_max = d;
    if (o > fm1_in_stat.dac_max)
        fm1_in_stat.dac_max = o;
}
#endif

static void keyboard_block(void)
{
    uint32_t cur = fm1_in.notes, ch, k;
    uint32_t lay, fx;
    ch = cur ^ kb_prev;                           /* keys also sound while entering steps */
    if (!ch)
        return;
    lay = (fm1_in.buttons & kb_mask) || kb_lock;  /* (#83: kb_lock, a layer locked open with no button held) */
    fx = (fm1_in.buttons & perf_mask) || (kb_lock & 2u);
    for (k = 0; k < 27u; k++) {
        if (!((ch >> k) & 1u))
            continue;
        if ((cur >> k) & 1u) {                    /* the selected track; the key-up goes to the same one */
            kb_trk[k] = song.sel;
            if (lay) {                            /* a layer's button held (or locked): the key is the layer's */
                kb_note[k] = KB_SILENT;           /* (ui_layer.c), with FX an effect (perform.c) */
                kb_layer |= 1u << k;
                if (fx)
                    perf_press(perf_key(k), 1);
                continue;
            }
            if (song.grid == 2u)                  /* NAME (ui_name.c): every key types, none sounds */
                kb_note[k] = KB_SILENT;
            else if (song.grid) {                 /* the grid: a row's key plays its drum, the rest are the UI's */
                const kit_if_t *kk = note_kit(&trk[kb_trk[k]]);
                kb_note[k] = kk && key_black(k) && key_place(k) < 9u && key_place(k) < kk->nlanes
                                 ? (uint8_t)lane_row(kk, grid_lane_of(kk, key_place(k))) : KB_SILENT;
            }
            else
                kb_note[k] = (uint8_t)kb_map(&trk[kb_trk[k]], k);
            if (kb_note[k] == KB_SILENT)
                continue;
            key_on(k, &trk[kb_trk[k]]);
#ifdef FM1_INPUT_LAT
            kb_lat(k);
#endif
        } else {
            if ((kb_layer >> k) & 1u) {
                kb_layer &= ~(1u << k);
                perf_press(perf_key(k), 0);
                continue;
            }
            if (kb_note[k] == KB_SILENT)
                continue;
            key_off(k, &trk[kb_trk[k] % NTRK]);
        }
    }
    kb_prev = cur;
}

/* -------------------------------------------------------- sequencer --- */
static void seq_start(void)
{
    uint32_t i;
    motion_begin();
    chain_start();
    for (i = 0; i < NTRK; i++) {                   /* every track from its step 0, together */
        track_t *t = &trk[i];
        t->seq_idx = 0;
        t->seq_count = 0;
        t->seq_pos = SEQ_FRESH;                    /* step 0 starts on the first block */
        t->seq_ev_n = 0;
        t->rskip_n = 0;
        t->rh_n = 0;
    }
    song.tick = 0;
    beat_pos = 0; beat_n = 0;                      /* the ARP LED's beat from the top too */
    song.playing = 1;
    slicer_start();                                /* slicer.c: its step 0 with the sequencer's */
    perf_start();                                  /* perform.c: its 1/16 grid too */
}

/* every sequenced note of the track ends now; the ones due in this step are dropped */
static void seq_release(track_t *t)
{
    uint32_t i;
    for (i = 0; i < t->seq_on_n; i++)
        trk_note_off(t, t->seq_on[i].row);
    t->seq_on_n = 0;
    t->seq_ev_n = 0;
    t->slide_glide = 0;                             /* live MONO / LEG keys must not glide after it */
}

static void seq_stop(void)
{
    uint32_t i;
    song.playing = 0;
    if (song.g[G_CLOCK])                           /* external clock: the arp stops with the transport, */
        for (i = 0; i < NPART; i++)                /* its sounding note too (HOLD keeps the latched chord) */
            if (trk[i].arp_note)
                arp_off(&trk[i]);
    for (i = 0; i < NTRK; i++) {
        seq_release(&trk[i]);
        trk[i].rh_n = 0;                           /* a recorded note held over the stop: as far as it got */
    }
    chain_stop();
    motion_end();
}

/* CESARI: playing notes. As step c (seq_count) starts, every note whose row is at its start there (the row's
 * place: c modulo its length) is due: at once, or later in the step by its nudge; one nudged early is due in the
 * step before, from the end back. Its chance is rolled then (a loop condition counts its row's loops). A due note
 * plays when the sequencer gets there (seq_fire) and ends after its length: (len - 1) steps and GATE of one; a
 * drum's is half a step (its note-off). QNT SEQ snaps a synth's notes to the scale as they play */
static void seq_queue(track_t *t, uint32_t at, const note_t *n, uint32_t period, uint32_t loop)
{
    uint32_t dur;
    if (t->seq_ev_n >= SEQ_EV || !chance_roll(n->chance, loop, rng()))
        return;
    dur = note_kit(t) ? period / 2u : (n->len - 1u) * period + period * (uint32_t)t->p[P_SGATE] / 128u;
    t->seq_ev[t->seq_ev_n].at = at;
    t->seq_ev[t->seq_ev_n].dur = dur ? dur : 1u;
    t->seq_ev[t->seq_ev_n].row = n->row;
    t->seq_ev[t->seq_ev_n].vel = n->vel;
    t->seq_ev_n++;
}
static __attribute__((noinline)) void seq_enter(track_t *t, uint32_t period)
{
    const pat_t *p = seq_pat(t);
    uint32_t c = t->seq_count, cur = step_samples(t, period, c), i;
    for (i = 0; i < p->n && i < NNOTE; i++) {
        const note_t *n = &p->note[i];
        uint32_t len = row_len_of(t, p, n->row);
        if (n->start >= len)
            continue;                               /* (past its row's length: silent) */
        if (n->start == c % len && (n->nudge >= 0 || !c))
            seq_queue(t, n->nudge > 0 ? cur * (uint32_t)n->nudge / 24u : 0u, n, period, c / len);
        if (n->nudge < 0 && n->start == (c + 1u) % len)
            seq_queue(t, cur - cur * (uint32_t)(-n->nudge) / 24u, n, period, (c + 1u) / len);
    }
}
/* a due note plays: again if it sounds already (its old note-off first); not a live-recorded one (rskip) */
static void seq_play(track_t *t, const seq_ev_t *e)
{
    uint32_t x = e->row, i;
    const engine_t *en = ENGINES[eng_idx(t->eng_req)];
    if (t->rskip_n && t->rskip_at == t->seq_count)
        for (i = 0; i < t->rskip_n; i++)
            if (t->rskip[i] == x)
                return;
    if (t->p[P_QUANT] == QN_SEQ && !(en->keys && en->keys(t, 0) >= 0))
        x = (uint32_t)clamp(scale_snap(t, (int32_t)x), 0, 127);
    for (i = 0; i < t->seq_on_n && t->seq_on[i].row != x; i++)
        ;
    if (i < t->seq_on_n) {
        trk_note_off(t, x);
        t->seq_on[i] = t->seq_on[--t->seq_on_n];
    } else if (t->seq_on_n >= SEQ_ON) {             /* (full: the oldest ends) */
        trk_note_off(t, t->seq_on[0].row);
        t->seq_on[0] = t->seq_on[--t->seq_on_n];
    }
    trk_note_on(t, x, e->vel);
    t->seq_on[t->seq_on_n].row = (uint8_t)x;
    t->seq_on[t->seq_on_n].left = e->dur;
    t->seq_on_n++;
}
/* the notes due by sample `upto` of the step play */
static void seq_fire(track_t *t, uint32_t upto)
{
    uint32_t i = 0;
    while (i < t->seq_ev_n)
        if (t->seq_ev[i].at <= upto) {
            seq_ev_t e = t->seq_ev[i];
            t->seq_ev[i] = t->seq_ev[--t->seq_ev_n];
            seq_play(t, &e);
        } else
            i++;
}

static void seq_tick(track_t *t, uint32_t n)
{
    uint32_t period, len, i = 0;
    while (i < t->seq_on_n) {                       /* note-offs (also while stopped: the last ones end) */
        if (t->seq_on[i].left <= n) {
            trk_note_off(t, t->seq_on[i].row);
            t->seq_on[i] = t->seq_on[--t->seq_on_n];
        } else
            t->seq_on[i++].left -= n;
    }
    if (!song.playing)
        return;
    period = div_samples((uint32_t)t->p[P_SDIV]);
    len = t->p[P_SLEN] > 0 ? (uint32_t)t->p[P_SLEN] : 1u;
    if (t->seq_pos == SEQ_FRESH) {                  /* PLAY: step 0 now (a song: where its carry says) */
        t->seq_pos = chain.running ? chain.carry : 0u;
        t->seq_count = 0;
        t->seq_idx = 0;
        motion_step(t, 0, chain.running ? &chain.source[chain.slot].motion : &motion);
        seq_enter(t, period);
    } else
        t->seq_pos += n;
    for (;;) {
        uint32_t cur_len = step_samples(t, period, t->seq_count);
        seq_fire(t, t->seq_pos < cur_len ? t->seq_pos : cur_len - 1u);
        if (t->seq_pos < cur_len)
            break;
        t->seq_pos -= cur_len;
        for (i = 0; i < t->seq_ev_n; i++)           /* (nudged early into the next step: from its start) */
            t->seq_ev[i].at = t->seq_ev[i].at > cur_len ? t->seq_ev[i].at - cur_len : 0u;
        t->seq_count++;
        t->seq_idx = (uint16_t)(t->seq_count % len);
        motion_step(t, t->seq_idx, chain.running ? &chain.source[chain.slot].motion : &motion);
        seq_enter(t, period);
    }
}

/* MIDI in: the track a channel plays (0..15): G_ROUTE CH1-4 (0) channels 1..4 their parts (5..16 never get
 * here: midi_event drops them), SEL (1) every channel the selected track */
static uint8_t midi_route;                 /* the G_ROUTE events_block last saw (a change to CH1-4: midi_route_ch14) */
static track_t *midi_track(uint32_t ch) { return ch < NPART && !song.g[G_ROUTE] ? &trk[ch] : TSEL; }

#include "midi_control.c"
#include "midi_clock.c"

/* everything that happens between two rendered blocks */
static void events_block(uint32_t n)
{
    uint32_t i, pr, seq_n = n;
    uint32_t clock_mode = (uint32_t)song.g[G_CLOCK];
    if (midi_clock.mode != clock_mode) {
        seq_stop();
        memset(&midi_clock, 0, sizeof midi_clock);
        midi_clock.mode = (uint8_t)clock_mode;
        midi_beat_samples = 0;
    }
    if (transport_req == 1u) {
        if (clock_mode)
            midi_clock_transport(0xFAu, fm1_ms);
        else
            seq_start();
        transport_req = 0;
    } else if (transport_req == 2u) {
        seq_stop();
        transport_req = 0;
    } else if (transport_req == 3u) {                 /* GLO + PLAY: from the top without stopping (INT clock) */
        if (!clock_mode) {
            seq_stop();
            seq_start();
        }
        transport_req = 0;
    }
    if (midi_route != (uint8_t)song.g[G_ROUTE]) {     /* ROUT changed: CH1-4 lets go of channels 5..16 */
        midi_route = (uint8_t)song.g[G_ROUTE];
        if (!midi_route)
            midi_route_ch14();
    }
    pr = panic_req;
    panic_req = 0;
    if (midi_in_overflow) {                            /* a lost note-off must never leave a held note */
        mi_r = mi_w;
        memset(midi_sel_on, 0, sizeof midi_sel_on);
        memset(midi_ch, 0, sizeof midi_ch);
        memset(midi_owners, 0, sizeof midi_owners);
        memset(mchord, 0, sizeof mchord);
        midi_hint = 0;
        for (i = 0; i < NTRK; i++) {
            trk[i].rh_n = trk[i].rskip_n = 0;
            trk[i].seq_on_n = trk[i].seq_ev_n = trk[i].slide_glide = 0;
        }
        pr |= (1u << NTRK) - 1u;
        RING_PUBLISH();
        midi_in_overflow = 0;
    }
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        if ((pr >> i) & 1u) {
            midi_forget_track(i);
            trk_all_off(t);
            t->nheld = 0;
            t->arp_phys = 0;
            t->arp_note = 0;
        }
        engine_block(t);                              /* engine switch: fade, then switch (voice.c) */
        /* ARP turned off, or HOLD released with no key down: drop the latched chord */
        if ((t->armp && !t->p[P_AMODE]) || (t->aholdp && !t->p[P_AHOLD] && !t->arp_phys)) {
            t->nheld = 0;
            if (!t->p[P_AMODE])
                t->arp_phys = 0;
            if (t->arp_note) {
                trk_note_off(t, t->arp_note);
                t->arp_note = 0;
            }
        }
        t->armp = t->p[P_AMODE];
        t->aholdp = t->p[P_AHOLD];
    }
    keyboard_block();
    while (mi_r != mi_w) {                            /* USB-MIDI (and TRS) in */
        uint32_t at = mi_r % MQ, pkt = midi_in_q[at], status = (pkt >> 8) & 0xFFu;
        uint32_t st = status & 0xF0u, ch = status & 0x0Fu;
        uint32_t d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        uint32_t source = midi_in_source[at] ? midi_in_source[at] : 1u, ms = midi_in_ms[at];
        mi_r++;
        if (status >= 0xF8u) {
            if (clock_mode == source) {
                if (status == 0xF8u)
                    midi_clock_pulse(ms);
                else
                    midi_clock_transport(status, ms);
            }
        } else
            midi_event(st, ch, d1, d2);
    }
    if (clock_mode) {
        seq_n = 0;
        if (song.playing) {
            uint32_t last = midi_clock.have_pulse ? midi_clock.last_ms : midi_clock.start_ms;
            if (fm1_ms - last > 500u) {
                seq_stop();                         /* cable loss must not leave a running held note */
                midi_clock.tempo_valid = 0;
            } else
                seq_n = midi_clock_advance(fm1_ms);
        }
    }
    chain_tick(seq_n);
    for (i = 0; i < NTRK; i++)
        seq_tick(&trk[i], seq_n);
    for (i = 0; i < NPART; i++)
        arp_step(&trk[i], clock_mode ? seq_n : n, clock_mode && song.playing ? seq_n : n);
    beat_pos += clock_mode ? seq_n : n;               /* the beat the ARP LED flashes on (ui_leds) */
    if (beat_pos >= beat_samples()) {
        beat_pos -= beat_samples();
        beat_pos = beat_pos < beat_samples() ? beat_pos : 0u;
        beat_n = (beat_n + 1u) & 3u;
    }
    if (song.playing)
        song.tick++;
}
