/* app.h - state shared by the window loop and the renderer. */
#ifndef APP_H
#define APP_H

#include <cairo.h>
#include <stdint.h>

#include "chess.h"
#include "engine.h"

enum { MSG_INFO, MSG_NOTE, MSG_ERROR, MSG_GOOD };

#define HISTORY 64

typedef struct {
    Game game;
    Opponent *opp;
    int human;   /* WHITE or BLACK */
    int level;   /* LEVEL_MIN..LEVEL_MAX */
    bool flipped, blind, strict;
    uint64_t peek_until; /* show pieces in blind mode until this time */

    char input[64];
    int input_len;
    char history[HISTORY][64];
    int nhist, hist_pos;

    char msg[512];
    int msg_kind;

    bool animating;
    Move anim_move;
    uint64_t anim_start;

    bool saved; /* finished game written to games/ */
    char base_dir[1024];
    uint64_t now;
} App;

#define ANIM_MS 180

bool render_init(const char *base_dir);
void render_frame(App *a, cairo_t *cr, double w, double h, double scale);
void render_shutdown(void);

#endif
