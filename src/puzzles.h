/* puzzles.h - lichess puzzles (data/puzzles/lichess.tsv): pick by theme near
 * the player's puzzle rating, track a rating, and bring back missed ones. */
#ifndef PUZZLES_H
#define PUZZLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char id[8];
    char fen[92];
    char moves[100];  /* UCI, space separated; the first is the opponent's */
    short rating;
    uint64_t themes;  /* bit per entry in puzzle_theme_names() */
} LPuzzle;

typedef struct {
    LPuzzle *p;
    int n;
    uint8_t *box;     /* 255 = never tried; 0 = missed; higher = solved on review */
    long *due;
    double rating;
    int rated;
    char prog_path[1100];
} PuzzleSet;

bool puzzles_load(PuzzleSet *s, const char *base_dir, char *err, size_t errlen);
void puzzles_free(PuzzleSet *s);
/* comma-separated theme names or aliases -> mask (0 = any); bad names in err */
uint64_t puzzles_mask(const char *names, char *err, size_t errlen);
int puzzles_pick(PuzzleSet *s, uint64_t mask); /* index, or -1 */
void puzzles_record(PuzzleSet *s, int idx, bool ok);
void puzzles_theme_text(uint64_t themes, char *out, size_t n);

#endif
