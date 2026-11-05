/* puzzles.c - lichess puzzle selection, rating and review. */
#include "puzzles.h"

/* 400*log10(0.85/0.15): the gap at which you score 85% */
#define PUZZLE_EASIER 300

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *THEMES[] = {
    "hangingPiece", "fork", "pin", "skewer", "discoveredAttack", "doubleCheck", "mateIn1", "mateIn2",
    "mateIn3", "backRankMate", "smotheredMate", "trappedPiece", "attraction", "deflection",
    "interference", "intermezzo", "clearance", "capturingDefender", "defensiveMove", "quietMove",
    "advancedPawn", "promotion", "exposedKing", "kingsideAttack", "sacrifice", "xRayAttack",
    "zugzwang", "rookEndgame", "pawnEndgame", "queenEndgame", "bishopEndgame", "knightEndgame",
    "mate", "endgame", "middlegame", "opening", "short", "long", "veryLong", "oneMove",
    "crushing", "advantage", "equality",
};
#define NTHEMES (int)(sizeof THEMES / sizeof THEMES[0])

/* friendly names for the commands and the plan */
static const struct { const char *alias, *themes; } ALIASES[] = {
    {"defence", "defensiveMove"}, {"defense", "defensiveMove"}, {"defensive", "defensiveMove"},
    {"hanging", "hangingPiece"}, {"quiet", "quietMove"}, {"mates", "mateIn1,mateIn2,mateIn3"},
    {"discovered", "discoveredAttack"}, {"backrank", "backRankMate"}, {"trapped", "trappedPiece"},
    {"endgames", "endgame"}, {"rook", "rookEndgame"}, {"pawn", "pawnEndgame"},
};

static int theme_index(const char *name) {
    for (int i = 0; i < NTHEMES; i++)
        if (!strcasecmp(THEMES[i], name)) return i;
    return -1;
}

static long today(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return (long)((t + tm.tm_gmtoff) / 86400);
}

uint64_t puzzles_mask(const char *names, char *err, size_t errlen) {
    uint64_t m = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", names ? names : "");
    if (err && errlen) err[0] = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        if (!strcasecmp(t, "any") || !strcasecmp(t, "all")) continue;
        bool done = false;
        for (size_t i = 0; i < sizeof ALIASES / sizeof ALIASES[0]; i++)
            if (!strcasecmp(t, ALIASES[i].alias)) { m |= puzzles_mask(ALIASES[i].themes, NULL, 0); done = true; }
        if (done) continue;
        int k = theme_index(t);
        if (k >= 0) m |= 1ull << k;
        else if (err && errlen) snprintf(err, errlen, "Unknown theme '%s'.", t);
    }
    return m;
}

void puzzles_theme_text(uint64_t themes, char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < NTHEMES; i++) {
        if (!(themes & (1ull << i))) continue;
        /* the motif names say most; skip length and evaluation tags */
        if (!strcmp(THEMES[i], "short") || !strcmp(THEMES[i], "long") || !strcmp(THEMES[i], "veryLong") ||
            !strcmp(THEMES[i], "oneMove") || !strcmp(THEMES[i], "crushing") || !strcmp(THEMES[i], "advantage") ||
            !strcmp(THEMES[i], "equality"))
            continue;
        if (out[0]) strncat(out, ", ", n - strlen(out) - 1);
        strncat(out, THEMES[i], n - strlen(out) - 1);
    }
}

static int cmp_id(const void *a, const void *b) {
    return strcmp(((const LPuzzle *)a)->id, ((const LPuzzle *)b)->id);
}

static void load_progress(PuzzleSet *s) {
    FILE *f = fopen(s->prog_path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        double r;
        int n, box;
        long due;
        char id[16];
        if (sscanf(line, "rating %lf %d", &r, &n) == 2) { s->rating = r; s->rated = n; continue; }
        if (sscanf(line, "%15s %d %ld", id, &box, &due) != 3) continue;
        LPuzzle key;
        snprintf(key.id, sizeof key.id, "%.7s", id);
        LPuzzle *hit = bsearch(&key, s->p, (size_t)s->n, sizeof *s->p, cmp_id);
        if (hit) { s->box[hit - s->p] = (uint8_t)box; s->due[hit - s->p] = due; }
    }
    fclose(f);
}

static void save_progress(const PuzzleSet *s) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", s->prog_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "rating %.1f %d\n", s->rating, s->rated);
    for (int i = 0; i < s->n; i++)
        if (s->box[i] != 255) fprintf(f, "%s %d %ld\n", s->p[i].id, s->box[i], s->due[i]);
    fclose(f);
    rename(tmp, s->prog_path);
}

bool puzzles_load(PuzzleSet *s, const char *base_dir, char *err, size_t errlen) {
    memset(s, 0, sizeof *s);
    s->rating = 1500; /* lichess's starting point; it moves fast at first */
    char path[1100];
    snprintf(path, sizeof path, "%sdata/puzzles/lichess.tsv", base_dir);
    snprintf(s->prog_path, sizeof s->prog_path, "%sdata/puzzle_progress.tsv", base_dir);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "No lichess puzzles: run analysis/puzzles.py."); return false; }
    char line[1024];
    int alloc = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (s->n == alloc) {
            alloc = alloc ? alloc * 2 : 4096;
            s->p = realloc(s->p, (size_t)alloc * sizeof *s->p);
        }
        LPuzzle *p = &s->p[s->n];
        char *save = NULL;
        char *id = strtok_r(line, "\t\n", &save), *fen = strtok_r(NULL, "\t\n", &save),
             *mv = strtok_r(NULL, "\t\n", &save), *rt = strtok_r(NULL, "\t\n", &save),
             *th = strtok_r(NULL, "\t\n", &save);
        if (!id || !fen || !mv || !rt) continue;
        snprintf(p->id, sizeof p->id, "%s", id);
        snprintf(p->fen, sizeof p->fen, "%s", fen);
        snprintf(p->moves, sizeof p->moves, "%s", mv);
        p->rating = (short)atoi(rt);
        p->themes = th ? puzzles_mask(th, NULL, 0) : 0;
        s->n++;
    }
    fclose(f);
    if (!s->n) { snprintf(err, errlen, "The puzzle file is empty."); return false; }
    qsort(s->p, (size_t)s->n, sizeof *s->p, cmp_id);
    s->box = malloc((size_t)s->n);
    s->due = calloc((size_t)s->n, sizeof *s->due);
    memset(s->box, 255, (size_t)s->n);
    load_progress(s);
    return true;
}

void puzzles_free(PuzzleSet *s) {
    free(s->p);
    free(s->box);
    free(s->due);
    memset(s, 0, sizeof *s);
}

int puzzles_pick(PuzzleSet *s, uint64_t mask) {
    long now = today();
    /* reviews first: missed puzzles that are due */
    for (int i = 0; i < s->n; i++)
        if (s->box[i] != 255 && s->box[i] < 5 && s->due[i] <= now && (!mask || (s->p[i].themes & mask)))
            return i;
    /* then a new one you should solve about 85% of the time - the
     * best-supported difficulty for learning (see docs/improving.md). By the
     * Elo formula that's PUZZLE_EASIER points below your rating. Widen the
     * window until one fits. */
    double target = s->rating - PUZZLE_EASIER;
    for (int width = 100; width <= 1600; width *= 2) {
        int start = rand() % s->n;
        for (int k = 0; k < s->n; k++) {
            int i = (start + k) % s->n;
            if (s->box[i] != 255 || (mask && !(s->p[i].themes & mask))) continue;
            if (fabs(s->p[i].rating - target) <= width) return i;
        }
    }
    return -1;
}

static const int INTERVAL[] = {1, 3, 7, 21, 60};


void puzzles_record(PuzzleSet *s, int idx, bool ok) {
    /* Elo-style rating: fast at first, steadier later */
    double expect = 1.0 / (1.0 + pow(10.0, (s->p[idx].rating - s->rating) / 400.0));
    double k = s->rated < 20 ? 60 : s->rated < 60 ? 32 : 20;
    bool first_try = s->box[idx] == 255;
    if (first_try) {
        s->rating += k * ((ok ? 1.0 : 0.0) - expect);
        s->rated++;
    }
    /* a puzzle solved first time is done; a missed one comes back */
    if (first_try && ok) { s->box[idx] = 5; s->due[idx] = 0; }
    else if (!ok) { s->box[idx] = 0; s->due[idx] = today() + INTERVAL[0]; }
    else {
        s->box[idx] = (uint8_t)(s->box[idx] + 1);
        s->due[idx] = today() + (s->box[idx] < 5 ? INTERVAL[s->box[idx]] : 0);
    }
    save_progress(s);
}
