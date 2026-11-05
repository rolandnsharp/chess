/* app.h - state shared by the window loop and the renderer. */
#ifndef APP_H
#define APP_H

#include <cairo.h>
#include <stdint.h>

#include "chess.h"
#include "drill.h"
#include "puzzles.h"
#include "review.h"
#include "lichess.h"
#include "engine.h"

enum { MSG_INFO, MSG_NOTE, MSG_ERROR, MSG_GOOD };
enum { DS_ASK = 1, DS_AUTO, DS_DONE, DS_PLAY };
enum { SC_OFF = 0, SC_ASK, SC_RESULT };

enum { TH_OFF = 0, TH_ASK, TH_RESULT };
enum { TW_NONE = 0, TW_BEGIN, TW_AGAIN, TW_FINISHED };
enum { RV_OFF = 0, RV_SELF, RV_SHOWN };
enum { OL_OFF = 0, OL_SEEKING, OL_PLAYING, OL_OVER };

/* the only live time control offered: slow enough to check every move */
#define OL_MINUTES 15
#define OL_INCREMENT 10

typedef struct {
    int ply;       /* index of your move that you think was wrong */
    Move m;        /* what you'd play instead */
    char tag[12];  /* why: hung, threat, capture, calc, plan, time, other */
} ReviewMark;
#define RV_MARKS 3

#define SCAN_MAX 64
#define ARROWS_MAX 16
#define PLAN_MAX 24

typedef struct {
    char fen_before[100], last_move[12], answers[100], played[16], note[200];
} ThreatPos;

typedef struct {
    char kind[16]; /* squares, scan, threats, drill, play */
    char arg[32];
    int count;
    char summary[160];
} TrainStep;
typedef struct {
    char fen_before[100], last_move[12], link[200], note[120];
} ScanPos;

#define HISTORY 64

typedef struct {
    Game game;
    Opponent *opp;
    int human;   /* WHITE or BLACK */
    int pref_side; /* the side you chose for free play */
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
    char banner[1200];  /* drill prompt or training plan, shown in the panel */

    /* checks-and-captures scan */
    int scan;              /* SC_* */
    int scan_mode;         /* 0 checks and captures, 1 captures, 2 checks */
    int scan_phase;        /* 0: your moves, 1: your opponent's */
    Position scan_pos;     /* the position being scanned, scan side to move */
    Move scan_moves[SCAN_MAX];
    bool scan_found[SCAN_MAX];
    int scan_n, scan_wrong;
    uint64_t scan_start;
    double scan_secs;
    ScanPos *scan_list;
    int scan_count, scan_next;
    int scan_sess_found, scan_sess_total, scan_sess_phases;
    double scan_sess_secs;

    /* lichess puzzles: drill flow on a scratch Drill */
    PuzzleSet lp;
    bool lichess;
    int lp_idx;
    uint64_t lp_mask;
    char lp_theme[64];
    Drill lp_drill;
    double lp_rating_start;

    /* live lichess game */
    int online;            /* OL_* */
    LStream *ol_events, *ol_seek, *ol_game;
    char ol_token[128], ol_user[48], ol_id[24];
    char ol_name[2][64];
    long ol_time[2];       /* ms left at ol_clock_at */
    uint64_t ol_clock_at, ol_since;
    char ol_moves[MAX_PLY * 6]; /* moves as the server last sent them */
    char ol_status[24];

    /* self-review */
    int review;            /* RV_* */
    ReviewSet rv;
    int rv_cur, rv_ply;
    ReviewMark rv_mark[RV_MARKS];
    int rv_nmarks, rv_found, rv_total;
    bool rv_mistake[RV_MAX_PLY];

    /* threat spotting */
    int threat;            /* TH_* */
    ThreatPos *th_list;
    int th_count, th_next;
    Position th_pos;       /* the opponent to move, as if given a free move */
    Move th_ans[8];
    int th_nans;
    bool th_none;          /* the answer is "no threat" */
    uint64_t th_start;
    int th_sess_n, th_sess_ok;

    /* generic arrows (threat answers); colours: 0 green, 1 red, 2 orange */
    Move arrows[ARROWS_MAX];
    int arrow_col[ARROWS_MAX], narrows;

    /* training run: a plan of steps, written by the coach */
    bool training;
    TrainStep plan[PLAN_MAX];
    int plan_n, tstep, tdone, twait;
    char plan_note[800], plan_source[64];
    int sq_sess_rounds, sq_sess_score;

    int snd_nply;          /* board as last heard, to sound each new move once */
    uint64_t snd_hash;

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
