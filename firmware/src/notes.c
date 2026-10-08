/* SPDX-License-Identifier: GPL-3.0-only */
/* CESARI: the sequence as notes (core.h note_t, pat_t), the Deluge's way: a track's pattern is a list of notes,
 * each on a row (a MIDI note; a drum's: its GM note) starting on a step, nudged earlier or later, with its own
 * length, velocity and chance (a percentage or a loop condition). A kit's drums loop at their own lengths
 * (pat_t.lane_len), every other row at the pattern's LEN.
 *
 * The old formats (projects to FUN8, user presets, the factory patterns, the editor's STEP protocol) hold steps
 * (core.h step_t): pat_from_steps / pat_step_view / pat_step_set turn one into the other. */

/* ---------------------------------------------------------- chance --- */
/* A note's chance: always (0), 10..90 % (1..9), or a loop condition a:b (NOTE_CH_COND + k): it plays on the a-th
 * of every b loops of its row. KNOB order (chance_pos): 10 % .. 90 %, 100 % (always), then 1:2 2:2 1:3 .. 4:4 */
#define NOTE_CONDS 9u
#define NOTE_CH_COND 20u
#define NOTE_CH_POS (10u + NOTE_CONDS)            /* knob positions */
static const uint8_t NOTE_COND[NOTE_CONDS][2] = {{1, 2}, {2, 2}, {1, 3}, {2, 3}, {3, 3}, {1, 4}, {2, 4}, {3, 4}, {4, 4}};

static int chance_ok(uint32_t c) { return c < 10u || (c >= NOTE_CH_COND && c < NOTE_CH_COND + NOTE_CONDS); }
static uint32_t chance_pos(uint32_t c)
{
    return !chance_ok(c) || !c ? 9u : c < 10u ? c - 1u : 10u + c - NOTE_CH_COND;
}
static uint32_t chance_of_pos(uint32_t p)
{
    return p >= NOTE_CH_POS ? NOTE_CH_COND + NOTE_CONDS - 1u : p >= 10u ? NOTE_CH_COND + p - 10u : p >= 9u ? 0u : p + 1u;
}
/* "100" "%", "50" "%", "1:2" "" */
static void chance_label(uint32_t c, char *b, const char **unit)
{
    if (c >= NOTE_CH_COND && c < NOTE_CH_COND + NOTE_CONDS) {
        b[0] = (char)('0' + NOTE_COND[c - NOTE_CH_COND][0]);
        b[1] = ':';
        b[2] = (char)('0' + NOTE_COND[c - NOTE_CH_COND][1]);
        b[3] = 0;
        *unit = "";
        return;
    }
    fmt_int(b, c && c < 10u ? (int32_t)c * 10 : 100);
    *unit = "%";
}
/* plays this time? loop: how many times its row has played round before (0 = the first) */
static int chance_roll(uint32_t c, uint32_t loop, uint32_t r)
{
    if (!c || !chance_ok(c))
        return 1;
    if (c < 10u)
        return r % 100u < c * 10u;
    c -= NOTE_CH_COND;
    return loop % NOTE_COND[c][1] == NOTE_COND[c][0] - 1u;
}
/* a step's chance (core.h step_chance, 0..100) as a note's */
static uint32_t chance_of_pct(uint32_t pct)
{
    return pct >= 95u ? 0u : pct < 10u ? 1u : (pct + 5u) / 10u;
}

/* ------------------------------------------------------------ notes --- */
static void pat_clear(pat_t *p) { memset(p, 0, sizeof *p); }
static int32_t pat_find(const pat_t *p, uint32_t row, uint32_t start)
{
    uint32_t i;
    for (i = 0; i < p->n && i < NNOTE; i++)
        if (p->note[i].row == row && p->note[i].start == start)
            return (int32_t)i;
    return -1;
}
/* a note at (row, start): the one there takes the new length and velocity (its chance and nudge stay); -1: full */
static int32_t pat_add(pat_t *p, uint32_t row, uint32_t start, uint32_t len, uint32_t vel)
{
    int32_t i = pat_find(p, row, start);
    note_t *n;
    if (i < 0) {
        if (p->n >= NNOTE)
            return -1;
        i = p->n++;
        n = &p->note[i];
        memset(n, 0, sizeof *n);
        n->row = (uint8_t)(row & 127u);
        n->start = (uint8_t)(start % NSTEP);
    } else
        n = &p->note[i];
    n->len = (uint8_t)(len < 1u ? 1u : len > NSTEP ? NSTEP : len);
    n->vel = (uint8_t)(vel < 1u ? 1u : vel > 127u ? 127u : vel);
    return i;
}
static void pat_del(pat_t *p, uint32_t i)
{
    if (i >= p->n)
        return;
    memmove(&p->note[i], &p->note[i + 1u], (p->n - i - 1u) * sizeof p->note[0]);
    p->n--;
}
/* every note starting at step s (all rows) gone */
static void pat_del_step(pat_t *p, uint32_t s)
{
    uint32_t i = 0;
    while (i < p->n)
        if (p->note[i].start == s)
            pat_del(p, i);
        else
            i++;
}
static uint32_t pat_sig_of(const pat_t *p)
{
    uint32_t h = 2166136261u, i;
    const uint8_t *b = (const uint8_t *)p->note;
    for (i = 0; i < (uint32_t)p->n * sizeof p->note[0]; i++)
        h = (h ^ b[i]) * 16777619u;
    for (i = 0; i < KIT_LANES; i++)
        h = (h ^ p->lane_len[i]) * 16777619u;
    return (h ^ p->n) * 16777619u;
}
/* a note's fields inside their ranges (a loaded pattern) */
static void note_bound(note_t *n)
{
    n->row &= 127u;
    n->start %= NSTEP;
    n->len = (uint8_t)(n->len < 1u ? 1u : n->len > NSTEP ? NSTEP : n->len);
    n->vel = (uint8_t)(n->vel < 1u ? 1u : n->vel > 127u ? 127u : n->vel);
    if (!chance_ok(n->chance))
        n->chance = 0;
    if (n->nudge < -NUDGE_MAX || n->nudge > NUDGE_MAX)
        n->nudge = 0;
}
static void pat_bound(pat_t *p)
{
    uint32_t i;
    if (p->n > NNOTE)
        p->n = NNOTE;
    for (i = 0; i < p->n; i++)
        note_bound(&p->note[i]);
    for (i = 0; i < KIT_LANES; i++)
        if (p->lane_len[i] > NSTEP)
            p->lane_len[i] = 0;
}

/* -------------------------------------------------------- the rows --- */
/* the kit the track's rows are drums of (the one asked for), 0 = a synth track: rows are notes */
static const kit_if_t *note_kit(const track_t *t) { return track_kit(t); }
/* the kit's drum a row plays, -1 = none (or not a kit) */
static int32_t note_lane(const kit_if_t *k, uint32_t row) { return k ? k->lane_of(row) : -1; }
/* the row of a kit's drum l (its own note) */
static uint32_t lane_row(const kit_if_t *k, uint32_t l) { return k && k->notes && l < k->nlanes ? k->notes[l] : 36u; }
/* the grid's row r (top down): its drum */
static uint32_t grid_lane_of(const kit_if_t *k, uint32_t r)
{
    if (!k || !k->nlanes)
        return 0;
    r %= k->nlanes;
    return k->order ? k->order[r] : r;
}
static uint32_t grid_row_of(const kit_if_t *k, uint32_t lane)   /* .. and back */
{
    uint32_t r;
    for (r = 0; k && r < k->nlanes; r++)
        if (grid_lane_of(k, r) == lane)
            return r;
    return 0;
}
/* the steps row `row` loops at: a kit's drum its own length, else the pattern's LEN */
static uint32_t row_len_of(const track_t *t, const pat_t *p, uint32_t row)
{
    int32_t l = note_lane(note_kit(t), row);
    uint32_t len = t->p[P_SLEN] > 0 ? (uint32_t)t->p[P_SLEN] : 1u;
    if (l >= 0 && l < (int32_t)KIT_LANES && p->lane_len[l])
        len = p->lane_len[l];
    return len > NSTEP ? NSTEP : len;
}

/* --------------------------------------------- the old formats: steps --- */
/* steps (len of them play) -> notes: a NOTE step's notes start there and hold through the TIE steps after it;
 * its lane hits are their GM drums; an accent is velocity 127; slides and ratchets are not kept */
static void pat_from_steps(pat_t *p, const step_t *s, uint32_t len)
{
    uint32_t i, k, l;
    pat_clear(p);
    len = len < 1u ? 1u : len > NSTEP ? NSTEP : len;
    for (i = 0; i < NSTEP; i++) {
        const step_t *st = &s[i];
        uint32_t ties = 0, vel, pct, ch;
        if (st->time != ST_NOTE || (!st->n && !st->hit))
            continue;
        pct = step_chance(st);
        if (!pct)
            continue;                                   /* (a silent step) */
        ch = chance_of_pct(pct);
        while (i + 1u + ties < len && s[i + 1u + ties].time == ST_TIE)
            ties++;
        vel = (st->flags & SF_ACCENT) ? 127u : st->vel ? st->vel : 96u;
        for (k = 0; k < st->n && k < 4u; k++) {
            int32_t j = pat_add(p, st->note[k], i, 1u + ties, vel);
            if (j >= 0)
                p->note[j].chance = (uint8_t)ch;
        }
        for (l = 0; l < NLANE; l++)
            if ((st->hit >> l) & 1u) {
                int32_t j = pat_add(p, DRUM_LANE_NOTE[l], i, 1u, (st->acc >> l) & 1u ? 127u : vel);
                if (j >= 0)
                    p->note[j].chance = (uint8_t)ch;
            }
    }
}
/* step s of the pattern as the old step format (the editor, user presets): its notes (4 at most; a drum's lane
 * notes as hits on a drum track), a TIE where an earlier note still holds, the loudest velocity, 127 an accent */
static void pat_step_view_k(const kit_if_t *kit, const pat_t *p, uint32_t s, step_t *st)
{
    uint32_t i, vel = 0, drum = kit != 0, ch = 100u;
    memset(st, 0, sizeof *st);
    st->time = ST_REST;
    for (i = 0; i < p->n; i++) {
        const note_t *n = &p->note[i];
        if (n->start == s) {
            uint32_t l = drum_lane(n->row);
            if (drum && DRUM_LANE_NOTE[l] == n->row) {
                st->hit |= (uint8_t)(1u << l);
                if (n->vel >= 127u)
                    st->acc |= (uint8_t)(1u << l);
            } else if (st->n < 4u)
                st->note[st->n++] = n->row;
            if (n->vel > vel)
                vel = n->vel;
            if (n->chance && n->chance < 10u)
                ch = n->chance * 10u;
            st->time = ST_NOTE;
        } else if (st->time == ST_REST && n->len > 1u && (uint32_t)((s + NSTEP - n->start) % NSTEP) < n->len)
            st->time = ST_TIE;
    }
    if (st->time == ST_NOTE) {
        st->vel = (uint8_t)(vel ? vel : 96u);
        if (vel >= 127u && !st->hit)
            st->flags |= SF_ACCENT;
        step_set_chance(st, ch);
    } else if (st->time == ST_TIE) {
        st->vel = 0;
    }
}
static void pat_step_view(const track_t *t, const pat_t *p, uint32_t s, step_t *st)
{
    pat_step_view_k(note_kit(t), p, s, st);
}
/* step s set from the old step format: the notes starting there are replaced by its notes and hits (a TIE or a
 * REST: none start there) */
static void pat_step_set(pat_t *p, uint32_t s, const step_t *st)
{
    uint32_t k, l, vel, ch;
    pat_del_step(p, s);
    if (st->time != ST_NOTE)
        return;
    vel = (st->flags & SF_ACCENT) ? 127u : st->vel ? st->vel : 96u;
    ch = chance_of_pct(step_chance(st));
    for (k = 0; k < st->n && k < 4u; k++) {
        int32_t j = pat_add(p, st->note[k], s, 1u, vel);
        if (j >= 0)
            p->note[j].chance = (uint8_t)ch;
    }
    for (l = 0; l < NLANE; l++)
        if ((st->hit >> l) & 1u) {
            int32_t j = pat_add(p, DRUM_LANE_NOTE[l], s, 1u, (st->acc >> l) & 1u ? 127u : vel);
            if (j >= 0)
                p->note[j].chance = (uint8_t)ch;
        }
}
/* step s at a glance (the footer's bars): 0 nothing, 1 a note holds over it, 2 a note starts there, 3 a loud one */
static uint32_t pat_step_kind(const pat_t *p, uint32_t s)
{
    uint32_t i, k = 0;
    for (i = 0; i < p->n; i++) {
        const note_t *n = &p->note[i];
        if (n->start == s)
            k = n->vel >= 120u ? 3u : k > 2u ? k : 2u;
        else if (!k && n->len > 1u && (uint32_t)((s + NSTEP - n->start) % NSTEP) < n->len)
            k = 1u;
    }
    return k;
}
/* any note starting at step s */
static int pat_step_on(const pat_t *p, uint32_t s)
{
    uint32_t i;
    for (i = 0; i < p->n; i++)
        if (p->note[i].start == s)
            return 1;
    return 0;
}
