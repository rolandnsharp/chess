/* drill.h - drills from data/analysis/drills/drills.tsv, scheduled with
 * Leitner boxes. Progress is kept in data/progress.tsv. */
#ifndef DRILL_H
#define DRILL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DRILL_MAX_SOL 32

typedef struct {
    char kind[12]; /* tactic, punish, line, endgame */
    char fen[100];
    char sol[DRILL_MAX_SOL][12];
    int nsol;
    char title[300], link[200], info[300];
    uint64_t key;
    int box;  /* 0 = new or failed; higher = known longer */
    long due; /* day number */
    int seen, correct;
} Drill;

typedef struct {
    Drill *d;
    int n;
    int *queue, qn, qpos;
    char prog_path[1100];
} DrillSet;

bool drills_load(DrillSet *s, const char *base_dir, char *err, size_t errlen);
void drills_free(DrillSet *s);
/* Build a session queue. kind NULL or "all" = everything. Returns size. */
int drills_queue(DrillSet *s, const char *kind);
int drills_next(DrillSet *s); /* index into s->d, or -1 when finished */
void drills_record(DrillSet *s, int idx, bool ok);
int drills_due_count(const DrillSet *s, const char *kind);

#endif
