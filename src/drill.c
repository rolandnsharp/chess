/* drill.c - loading, scheduling and recording drills. */
#include "drill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* days until the next review, by box */
static const int INTERVAL[] = {0, 1, 3, 7, 21, 60};
#define NBOX (int)(sizeof INTERVAL / sizeof INTERVAL[0])

static long today(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return (long)((t + tm.tm_gmtoff) / 86400);
}

static uint64_t fnv(const char *a, const char *b, const char *c) {
    uint64_t h = 1469598103934665603ull;
    const char *parts[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        for (const char *p = parts[i]; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ull; }
        h ^= 0xff; h *= 1099511628211ull;
    }
    return h;
}

static char *field(char **s) {
    char *start = *s;
    if (!start) return "";
    char *tab = strchr(start, '\t');
    if (tab) { *tab = 0; *s = tab + 1; }
    else { *s = NULL; char *nl = strchr(start, '\n'); if (nl) *nl = 0; }
    return start;
}

static void load_progress(DrillSet *s) {
    FILE *f = fopen(s->prog_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned long long key;
        int box, seen, correct;
        long due;
        if (sscanf(line, "%llx %d %ld %d %d", &key, &box, &due, &seen, &correct) != 5) continue;
        for (int i = 0; i < s->n; i++)
            if (s->d[i].key == key) {
                s->d[i].box = box; s->d[i].due = due; s->d[i].seen = seen; s->d[i].correct = correct;
            }
    }
    fclose(f);
}

static void save_progress(const DrillSet *s) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", s->prog_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < s->n; i++)
        if (s->d[i].seen)
            fprintf(f, "%016llx %d %ld %d %d\n", (unsigned long long)s->d[i].key, s->d[i].box,
                    s->d[i].due, s->d[i].seen, s->d[i].correct);
    fclose(f);
    rename(tmp, s->prog_path);
}

bool drills_load(DrillSet *s, const char *base_dir, char *err, size_t errlen) {
    memset(s, 0, sizeof *s);
    char path[1100];
    snprintf(path, sizeof path, "%sdata/analysis/drills/drills.tsv", base_dir);
    snprintf(s->prog_path, sizeof s->prog_path, "%sdata/progress.tsv", base_dir);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "No drills yet: run analysis/report.py to make %s", path); return false; }
    char *line = NULL;
    size_t cap = 0;
    int alloc = 0;
    while (getline(&line, &cap, f) > 0) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (s->n == alloc) {
            alloc = alloc ? alloc * 2 : 256;
            s->d = realloc(s->d, (size_t)alloc * sizeof *s->d);
        }
        Drill *d = &s->d[s->n];
        memset(d, 0, sizeof *d);
        char *p = line;
        snprintf(d->kind, sizeof d->kind, "%s", field(&p));
        snprintf(d->fen, sizeof d->fen, "%s", field(&p));
        char *sol = field(&p);
        snprintf(d->title, sizeof d->title, "%s", field(&p));
        snprintf(d->link, sizeof d->link, "%s", field(&p));
        snprintf(d->info, sizeof d->info, "%s", field(&p));
        d->key = fnv(d->kind, d->fen, sol);
        char *save = NULL;
        for (char *t = strtok_r(sol, " ", &save); t && d->nsol < DRILL_MAX_SOL; t = strtok_r(NULL, " ", &save))
            snprintf(d->sol[d->nsol++], sizeof d->sol[0], "%s", t);
        if (!d->fen[0] || (strcmp(d->kind, "endgame") != 0 && d->nsol == 0)) continue;
        s->n++;
    }
    free(line);
    fclose(f);
    s->queue = malloc(sizeof(int) * (size_t)(s->n * 2 + 1));
    load_progress(s);
    if (s->n == 0) { snprintf(err, errlen, "The drill file is empty."); return false; }
    return true;
}

void drills_free(DrillSet *s) {
    free(s->d);
    free(s->queue);
    memset(s, 0, sizeof *s);
}

static bool kind_match(const Drill *d, const char *kind) {
    if (!kind || !*kind || !strcmp(kind, "all")) return true;
    /* accept plurals and short names */
    if (!strncmp(kind, "tactic", 6) || !strncmp(kind, "puzzle", 6)) return !strcmp(d->kind, "tactic");
    if (!strncmp(kind, "punish", 6)) return !strcmp(d->kind, "punish");
    if (!strncmp(kind, "line", 4) || !strncmp(kind, "open", 4) || !strncmp(kind, "rep", 3)) return !strcmp(d->kind, "line");
    if (!strncmp(kind, "end", 3)) return !strcmp(d->kind, "endgame");
    return false;
}

static bool is_due(const Drill *d, long now) { return !d->seen || d->due <= now; }

int drills_due_count(const DrillSet *s, const char *kind) {
    long now = today();
    int c = 0;
    for (int i = 0; i < s->n; i++)
        if (kind_match(&s->d[i], kind) && is_due(&s->d[i], now)) c++;
    return c;
}

int drills_queue(DrillSet *s, const char *kind) {
    long now = today();
    s->qn = s->qpos = 0;
    /* failed before, then due reviews, then new ones in file order (worst first) */
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < s->n; i++) {
            Drill *d = &s->d[i];
            if (!kind_match(d, kind) || !is_due(d, now)) continue;
            int which = d->seen && d->box == 0 ? 0 : d->seen ? 1 : 2;
            if (which == pass) s->queue[s->qn++] = i;
        }
    return s->qn;
}

int drills_next(DrillSet *s) {
    return s->qpos < s->qn ? s->queue[s->qpos++] : -1;
}

void drills_record(DrillSet *s, int idx, bool ok) {
    Drill *d = &s->d[idx];
    d->seen++;
    if (ok) {
        d->correct++;
        if (d->box < NBOX - 1) d->box++;
    } else {
        d->box = 0;
        if (s->qn < s->n * 2) s->queue[s->qn++] = idx; /* again later this session */
    }
    d->due = today() + INTERVAL[d->box];
    save_progress(s);
}
