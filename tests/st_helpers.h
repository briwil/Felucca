/* SPDX-License-Identifier: GPL-3.0-only */
/* CESARI: the tests read a track's notes as the old steps (notes.c pat_step_view) and write them so: a NOTE step's
 * notes start there, a TIE holds the notes ending there one step longer, a REST deletes what starts there */
#ifndef ST_HELPERS_H
#define ST_HELPERS_H
static const step_t *st_of(const track_t *t, uint32_t i)
{
    static step_t v[8];
    static uint32_t k;
    step_t *s = &v[k++ & 7u];
    pat_step_view(t, &t->pat, i % NSTEP, s);
    return s;
}
static void st_set(track_t *t, uint32_t i, step_t s)
{
    uint32_t k;
    if (s.time == ST_TIE) {
        for (k = 0; k < t->pat.n; k++)
            if ((uint32_t)t->pat.note[k].start + t->pat.note[k].len == i)
                t->pat.note[k].len++;
        return;
    }
    if (s.time == ST_NOTE && !s.vel)
        s.vel = 100;
    pat_step_set(&t->pat, i, &s);
    t->seq_active = 1;
}
/* a pattern written as old steps (len of them play): the notes */
static void st_load_pat(track_t *t, const step_t *s, uint32_t len) { pat_from_steps(&t->pat, s, len); t->seq_active = 1; }
/* how many sequenced notes sound now, and whether row r is one of them */
static int seq_sounds(const track_t *t, uint32_t r)
{
    uint32_t i;
    for (i = 0; i < t->seq_on_n; i++)
        if (t->seq_on[i].row == r)
            return 1;
    return 0;
}
#endif
