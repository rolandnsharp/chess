/* review.h - your analysed games (data/analysis/drills/games.tsv) for
 * self-review: you mark your mistakes first, then see the engine's. */
#ifndef REVIEW_H
#define REVIEW_H

#include <stdbool.h>
#include <stddef.h>

#define RV_MAX_PLY 400

typedef struct {
    char id[24], date[20], speed[12], white[40], black[40], result[8];
    char opening[96], link[128];
    bool me_white;
    int n;                       /* plies */
    char san[RV_MAX_PLY][10];
    int eval[RV_MAX_PLY + 1];    /* White-relative centipawns before ply i (eval[n] = final) */
    char best[RV_MAX_PLY][10];   /* engine's choice at ply i, "-" if none */
    bool reviewed;               /* in data/review_log.tsv already */
} ReviewGame;

typedef struct {
    ReviewGame *g;
    int n;                       /* newest first */
    char log_path[1100];
} ReviewSet;

bool reviews_load(ReviewSet *s, const char *base_dir, char *err, size_t errlen);
void reviews_free(ReviewSet *s);
double review_wp(int cp);        /* win probability from centipawns (lichess curve) */
/* your score in a game: 1 win, 0.5 draw, 0 loss */
double review_score(const ReviewGame *g);

#endif
