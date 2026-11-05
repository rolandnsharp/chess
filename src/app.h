/* app.h - state shared by the window loop and the renderer. */
#ifndef APP_H
#define APP_H

#include <cairo.h>
#include <stdint.h>

#include "chess.h"
#include "drill.h"
#include "engine.h"

enum { MSG_INFO, MSG_NOTE, MSG_ERROR, MSG_GOOD };
enum { DS_ASK = 1, DS_AUTO, DS_DONE, DS_PLAY };

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

    bool coords; /* board labels a-h, 1-8 */

    /* square-naming drill: name the highlighted square, 30 seconds */
    bool sq_drill;
    int sq_target, sq_score, sq_misses, sq_best;
    uint64_t sq_end, sq_flash; /* drill end; time of last wrong answer */

    /* drills */
    DrillSet drills;
    int drill;          /* current drill, or -1 in free play */
    int dstate;         /* DS_* */
    int dstep;          /* next solution move */
    bool dfailed;       /* got this drill wrong */
    bool dshow;         /* replaying the answer */
    bool drecorded;
    uint64_t dnext;     /* when the next automatic move is played */
    int dsess_n, dsess_ok;
    char banner[512];   /* drill prompt, shown in the panel */

    bool saved; /* finished game written to games/ */
    char base_dir[1024];
    uint64_t now;
} App;

#define ANIM_MS 180
#define SQ_DRILL_MS 30000

bool render_init(const char *base_dir);
void render_frame(App *a, cairo_t *cr, double w, double h, double scale);
void render_shutdown(void);

#endif
