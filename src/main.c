/* main.c - window, keyboard and game flow. Moves are typed in standard
 * notation; lines starting with '/' are commands. */
#include <SDL3/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "app.h"
#include "json.h"

static void say(App *a, int kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(App *a, int kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof a->msg, fmt, ap);
    va_end(ap);
    a->msg_kind = kind;
}

static void new_game(App *a, const char *fen, int human) {
    opp_cancel(a->opp);
    if (!game_init(&a->game, fen)) {
        game_init(&a->game, NULL);
        say(a, MSG_ERROR, "That FEN didn't parse; starting from the usual position.");
    }
    a->human = human;
    a->flipped = human == BLACK;
    a->animating = false;
    a->saved = false;
}

static bool save_pgn(App *a, char *path_out, size_t n) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%sgames", a->base_dir);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char stamp[32], date[16];
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M%S", &tm);
    strftime(date, sizeof date, "%Y.%m.%d", &tm);
    snprintf(path_out, n, "%s/%s.pgn", dir, stamp);
    FILE *f = fopen(path_out, "w");
    if (!f) return false;
    char opp[96];
    opp_name(a->opp, a->level, opp, sizeof opp);
    const char *you = "Player";
    fprintf(f, "[Event \"Notation Trainer\"]\n[Site \"local\"]\n[Date \"%s\"]\n", date);
    fprintf(f, "[White \"%s\"]\n[Black \"%s\"]\n", a->human == WHITE ? you : opp, a->human == WHITE ? opp : you);
    fprintf(f, "[Result \"%s\"]\n", game_result(&a->game));
    if (strcmp(a->game.start_fen, STARTPOS_FEN) != 0)
        fprintf(f, "[SetUp \"1\"]\n[FEN \"%s\"]\n", a->game.start_fen);
    GameStatus st = game_status(&a->game);
    if (st != ST_ONGOING) fprintf(f, "[Termination \"%s\"]\n", status_text(st));
    static char movetext[MAX_PLY * 12];
    game_movetext(&a->game, movetext, sizeof movetext);
    /* wrap at 80 columns */
    fputc('\n', f);
    int col = 0;
    for (char *tok = strtok(movetext, " "); tok; tok = strtok(NULL, " ")) {
        int l = (int)strlen(tok);
        if (col && col + 1 + l > 80) { fputc('\n', f); col = 0; }
        else if (col) { fputc(' ', f); col++; }
        fputs(tok, f);
        col += l;
    }
    fputs("\n\n", f);
    fclose(f);
    return true;
}

static Drill *cur_drill(App *a);
static void drill_finish(App *a, bool ok, const char *extra);
static void train_item(App *a);

static void after_move(App *a, Move m) {
    a->animating = true;
    a->anim_move = m;
    a->anim_start = a->now;
    GameStatus st = game_status(&a->game);
    Drill *d = cur_drill(a);
    if (d && a->dstate == DS_PLAY && st != ST_ONGOING) {
        const char *res = game_result(&a->game);
        bool won = strcmp(res, a->human == WHITE ? "1-0" : "0-1") == 0;
        drill_finish(a, won, won ? "Converted. " : "Not won this time. ");
        return;
    }
    if (st != ST_ONGOING && !a->saved) {
        char path[1200];
        a->saved = save_pgn(a, path, sizeof path);
        bool won = (strcmp(game_result(&a->game), "1-0") == 0) == (a->human == WHITE) &&
                   strcmp(game_result(&a->game), "1/2-1/2") != 0;
        say(a, won ? MSG_GOOD : MSG_NOTE, "%s. %s Saved to games/. %s",
            status_text(st), won ? "Well played!" : "", a->training ? "Enter to continue." : "/new for another.");
        if (a->training) train_item(a);
    }
}

/* ------------------------------------------------------------ drills */

static Drill *cur_drill(App *a) {
    if (a->drill < 0) return NULL;
    return a->lichess ? &a->lp_drill : &a->drills.d[a->drill];
}

/* the drill's start position; a pre-move (the opponent's) is played first */
static void drill_setup(App *a, Drill *d, int side) {
    new_game(a, d->fen, side);
    a->saved = true; /* drills aren't saved as games */
    if (!d->pre[0]) return;
    Move m;
    char why[256];
    if (san_parse(&a->game.pos, d->pre, false, &m, why, sizeof why) == SAN_ERR) return;
    game_push(&a->game, m);
    a->animating = true;
    a->anim_move = m;
    a->anim_start = a->now;
}

static bool solution_move(App *a, int step, Move *m) {
    char why[256];
    Drill *d = cur_drill(a);
    return step < d->nsol && san_parse(&a->game.pos, d->sol[step], false, m, why, sizeof why) != SAN_ERR;
}

static void drill_record(App *a, bool ok) {
    if (a->drecorded) return;
    a->drecorded = true;
    if (a->lichess) puzzles_record(&a->lp, a->lp_idx, ok);
    else drills_record(&a->drills, a->drill, ok);
    a->dsess_n++;
    if (ok) a->dsess_ok++;
}

static void train_item(App *a);
static bool train_step_full(App *a);
static void train_end_step(App *a);
static void leave_modes(App *a);

static void drill_finish(App *a, bool ok, const char *extra) {
    Drill *d = cur_drill(a);
    if (!a->drecorded) train_item(a);
    drill_record(a, ok);
    a->dstate = DS_DONE;
    char rating[64] = "";
    if (a->lichess) snprintf(rating, sizeof rating, "  ·  puzzle rating %.0f", a->lp.rating);
    say(a, ok ? MSG_GOOD : MSG_ERROR, "%s%s%s%s  ·  Enter for the next.",
        ok ? "Correct! " : "Missed. ", extra ? extra : "", d->info[0] ? d->info : "", rating);
}

static void drill_end_session(App *a) {
    int n = a->dsess_n, ok = a->dsess_ok;
    if (a->training) { train_end_step(a); return; }
    a->drill = -1;
    a->banner[0] = 0;
    new_game(a, NULL, a->pref_side);
    say(a, MSG_GOOD, "Drills finished: %d of %d right first time. Back to free play.", ok, n);
}

static void lichess_next(App *a);

static void drill_next(App *a) {
    if (a->lichess) { lichess_next(a); return; }
    int idx = drills_next(&a->drills);
    if (idx < 0) { drill_end_session(a); return; }
    a->drill = idx;
    Drill *d = &a->drills.d[idx];
    Position p;
    pos_from_fen(&p, d->fen);
    int side = d->pre[0] ? !p.side : p.side;
    if (!strcmp(d->kind, "line")) side = (d->nsol - 1) % 2 == 0 ? WHITE : BLACK;
    drill_setup(a, d, side);
    a->dstep = 0;
    a->dfailed = a->dshow = a->drecorded = false;
    const char *label = !strcmp(d->kind, "tactic") ? "Puzzle from your game"
                      : !strcmp(d->kind, "punish") ? "Punish the blunder"
                      : !strcmp(d->kind, "line") ? "Opening line" : "Endgame";
    snprintf(a->banner, sizeof a->banner, "%s  ·  %d left%s\n%s", label,
             a->drills.qn - a->drills.qpos + 1, d->seen ? "  ·  review" : "  ·  new", d->title);
    if (!strcmp(d->kind, "endgame")) a->dstate = DS_PLAY;
    else if (!strcmp(d->kind, "line") && side == BLACK) { a->dstate = DS_AUTO; a->dnext = a->now + 600; }
    else a->dstate = DS_ASK;
    say(a, MSG_INFO, "%s to move.", a->game.pos.side == WHITE ? "White" : "Black");
}

/* ---------------------------------------------------- lichess puzzles */

static bool lichess_build(App *a, int idx) {
    LPuzzle *p = &a->lp.p[idx];
    Drill *d = &a->lp_drill;
    memset(d, 0, sizeof *d);
    snprintf(d->kind, sizeof d->kind, "lichess");
    snprintf(d->fen, sizeof d->fen, "%s", p->fen);
    Position pos;
    if (!pos_from_fen(&pos, p->fen)) return false;
    char buf[128], *save = NULL;
    snprintf(buf, sizeof buf, "%s", p->moves);
    int k = 0;
    for (char *u = strtok_r(buf, " ", &save); u; u = strtok_r(NULL, " ", &save), k++) {
        Move m;
        if (!move_from_uci(&pos, u, &m)) return false;
        char san[16];
        move_to_san(&pos, m, san, sizeof san);
        if (k == 0) snprintf(d->pre, sizeof d->pre, "%.11s", san);
        else if (d->nsol < DRILL_MAX_SOL) snprintf(d->sol[d->nsol++], sizeof d->sol[0], "%.11s", san);
        make_move(&pos, m);
    }
    if (!d->pre[0] || !d->nsol) return false;
    char th[200];
    puzzles_theme_text(p->themes, th, sizeof th);
    snprintf(d->title, sizeof d->title, "Your opponent just played %s. Find the best move.", d->pre);
    snprintf(d->info, sizeof d->info, "%s  ·  puzzle rated %d  ·  lichess.org/training/%s", th, p->rating, p->id);
    snprintf(d->link, sizeof d->link, "https://lichess.org/training/%s", p->id);
    return true;
}

static void lichess_next(App *a) {
    for (int tries = 0; tries < 20; tries++) {
        int idx = a->lichess ? puzzles_pick(&a->lp, a->lp_mask) : -1;
        if (idx < 0) break;
        if (!lichess_build(a, idx)) { a->lp.box[idx] = 5; continue; } /* skip a broken one */
        a->lp_idx = idx;
        a->drill = 0;
        Drill *d = &a->lp_drill;
        Position p;
        pos_from_fen(&p, d->fen);
        drill_setup(a, d, !p.side);
        a->dstep = 0;
        a->dfailed = a->dshow = a->drecorded = false;
        a->dstate = DS_ASK;
        bool review = a->lp.box[idx] != 255;
        snprintf(a->banner, sizeof a->banner, "Lichess puzzle%s%s  ·  your puzzle rating %.0f%s\n%s",
                 a->lp_theme[0] ? "  ·  " : "", a->lp_theme, a->lp.rating, review ? "  ·  review" : "", d->title);
        say(a, MSG_INFO, "%s to move.", a->game.pos.side == WHITE ? "White" : "Black");
        return;
    }
    a->lichess = false;
    drill_end_session(a);
}

static void lichess_session(App *a, const char *themes) {
    char err[300];
    leave_modes(a);
    if (!a->lp.n && !puzzles_load(&a->lp, a->base_dir, err, sizeof err)) { say(a, MSG_ERROR, "%s", err); return; }
    a->lp_mask = puzzles_mask(themes, err, sizeof err);
    if (err[0]) { say(a, MSG_ERROR, "%s Try e.g. /puzzles fork, /puzzles defence,quiet.", err); return; }
    snprintf(a->lp_theme, sizeof a->lp_theme, "%s", themes ? themes : "");
    a->lichess = true;
    a->lp_rating_start = a->lp.rating;
    a->dsess_n = a->dsess_ok = 0;
    a->blind = false;
    lichess_next(a);
}

static void drill_session(App *a, const char *kind) {
    char err[300];
    leave_modes(a);
    drills_free(&a->drills);
    if (!drills_load(&a->drills, a->base_dir, err, sizeof err)) { say(a, MSG_ERROR, "%s", err); return; }
    if (drills_queue(&a->drills, kind) == 0) {
        say(a, MSG_INFO, "Nothing due%s%s. Come back tomorrow, or try /drill all.", kind ? " for " : "", kind ? kind : "");
        return;
    }
    a->dsess_n = a->dsess_ok = 0;
    a->blind = false;
    drill_next(a);
}

/* show the answer: back to the start and play the solution through */
static void drill_show(App *a, const char *why) {
    Drill *d = cur_drill(a);
    a->dfailed = true;
    if (!a->drecorded) train_item(a);
    drill_record(a, false);
    char line[300] = "";
    for (int i = 0; i < d->nsol; i++) {
        strncat(line, d->sol[i], sizeof line - strlen(line) - 2);
        strncat(line, " ", sizeof line - strlen(line) - 1);
    }
    drill_setup(a, d, a->human);
    a->dstep = 0;
    a->dshow = true;
    a->dstate = DS_AUTO;
    a->dnext = a->now + 900;
    say(a, MSG_ERROR, "%sThe answer: %s  ·  %s  ·  Enter for the next drill.", why ? why : "", line, d->info);
}

static void drill_user_move(App *a, Move m, const char *typed) {
    Drill *d = cur_drill(a);
    Move want;
    bool is_line = !strcmp(d->kind, "line");
    if (solution_move(a, a->dstep, &want) && move_eq(m, want)) {
        game_push(&a->game, m);
        after_move(a, m);
        a->dstep++;
        if (a->dstep >= d->nsol) { drill_finish(a, !a->dfailed, NULL); return; }
        a->dstate = DS_AUTO;
        a->dnext = a->now + 450;
        if (!a->dfailed) a->msg[0] = 0;
        return;
    }
    if (is_line) {
        a->dfailed = true;
        say(a, MSG_ERROR, "%s isn't your repertoire move. Here you play %s - type it to go on.", typed, d->sol[a->dstep]);
        return;
    }
    /* a puzzle: an equally good move counts */
    int ucp, bcp;
    if (opp_eval_after(a->opp, &a->game.pos, m, 300000, &ucp) &&
        opp_eval_after(a->opp, &a->game.pos, want, 300000, &bcp)) {
        bool ok = bcp >= 9000 ? ucp >= 9000 : (ucp >= bcp - 60 || ucp >= 600);
        if (ok) {
            game_push(&a->game, m);
            after_move(a, m);
            char extra[160];
            snprintf(extra, sizeof extra, "%s works too (the line was %s...). ", typed, d->sol[a->dstep]);
            drill_finish(a, !a->dfailed, extra);
            return;
        }
    }
    char why[160];
    snprintf(why, sizeof why, "Not %s. ", typed);
    drill_show(a, why);
}

/* automatic moves: the opponent's replies, or replaying an answer */
static void drill_tick(App *a) {
    if (a->drill < 0 || a->dstate != DS_AUTO || a->now < a->dnext || a->animating) return;
    Drill *d = cur_drill(a);
    Move m;
    if (!solution_move(a, a->dstep, &m)) { drill_finish(a, false, "(bad drill line) "); return; }
    game_push(&a->game, m);
    after_move(a, m);
    a->dstep++;
    if (a->dstep >= d->nsol) {
        if (a->dshow) a->dstate = DS_DONE;
        else drill_finish(a, !a->dfailed, NULL);
    } else if (a->dshow) {
        a->dnext = a->now + 900;
    } else {
        a->dstate = DS_ASK;
    }
}

/* ------------------------------------------- checks-and-captures scan */

static const char *SCAN_WHAT[] = {"check and capture", "capture", "check"};
static const char *SCAN_NOT[] = {"a check or a capture", "a capture", "a check"};

static bool scan_load(App *a) {
    if (a->scan_list) return true;
    char path[1200];
    snprintf(path, sizeof path, "%sdata/analysis/drills/scan.tsv", a->base_dir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char *line = NULL;
    size_t cap = 0;
    int alloc = 0;
    while (getline(&line, &cap, f) > 0) {
        if (line[0] == '#') continue;
        if (a->scan_count == alloc) {
            alloc = alloc ? alloc * 2 : 256;
            a->scan_list = realloc(a->scan_list, (size_t)alloc * sizeof *a->scan_list);
        }
        ScanPos *sp = &a->scan_list[a->scan_count];
        char *save = NULL, *t;
        t = strtok_r(line, "\t\n", &save); if (!t) continue; snprintf(sp->fen_before, sizeof sp->fen_before, "%s", t);
        t = strtok_r(NULL, "\t\n", &save); if (!t) continue; snprintf(sp->last_move, sizeof sp->last_move, "%s", t);
        t = strtok_r(NULL, "\t\n", &save); snprintf(sp->link, sizeof sp->link, "%s", t ? t : "");
        t = strtok_r(NULL, "\t\n", &save); snprintf(sp->note, sizeof sp->note, "%s", t ? t : "");
        a->scan_count++;
    }
    free(line);
    fclose(f);
    /* shuffle, so each session starts somewhere new */
    for (int i = a->scan_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        ScanPos tmp = a->scan_list[i]; a->scan_list[i] = a->scan_list[j]; a->scan_list[j] = tmp;
    }
    return a->scan_count > 0;
}

static bool gives_check(const Position *p, Move m) {
    Position q = *p;
    make_move(&q, m);
    return in_check(&q);
}

/* the moves to find for scan_pos, by mode; promotions count once (as =Q) */
static void scan_targets(App *a) {
    Move ms[MAX_MOVES];
    int n = gen_legal(&a->scan_pos, ms);
    a->scan_n = 0;
    a->scan_wrong = 0;
    for (int i = 0; i < n && a->scan_n < SCAN_MAX; i++) {
        if (ms[i].promo && ms[i].promo != QUEEN) continue;
        bool cap = ms[i].flags & MF_CAPTURE, chk = gives_check(&a->scan_pos, ms[i]);
        bool want = a->scan_mode == 0 ? (cap || chk) : a->scan_mode == 1 ? cap : chk;
        if (want) { a->scan_found[a->scan_n] = false; a->scan_moves[a->scan_n++] = ms[i]; }
    }
}

static void scan_prompt(App *a) {
    const char *who = a->scan_phase == 0 ? "your" : "your OPPONENT'S";
    const char *col = a->scan_pos.side == WHITE ? "White" : "Black";
    snprintf(a->banner, sizeof a->banner, "Checks & captures  ·  position %d%s\n"
             "Type every %s for %s (%s), one per Enter. Empty Enter when you have them all.",
             a->scan_next, a->scan_phase ? "  ·  threats" : "", SCAN_WHAT[a->scan_mode], col, who);
}

static void scan_begin_phase(App *a) {
    a->scan = SC_ASK;
    scan_targets(a);
    a->scan_start = a->now;
    scan_prompt(a);
}

static void scan_next_position(App *a) {
    for (int tries = 0; tries < a->scan_count; tries++) {
        ScanPos *sp = &a->scan_list[a->scan_next++ % a->scan_count];
        Position p;
        if (!pos_from_fen(&p, sp->fen_before)) continue;
        Move m;
        char why[256];
        if (san_parse(&p, sp->last_move, false, &m, why, sizeof why) == SAN_ERR) continue;
        int me = !p.side;
        new_game(a, sp->fen_before, me);
        a->saved = true;
        game_push(&a->game, m);
        a->animating = false;
        a->scan_pos = a->game.pos;
        a->scan_phase = 0;
        scan_begin_phase(a);
        say(a, MSG_INFO, "%s just played %s.", me == WHITE ? "Black" : "White", a->game.san[0]);
        return;
    }
    a->scan = SC_OFF;
    say(a, MSG_ERROR, "No usable positions in scan.tsv.");
}

static void scan_log(App *a, int found) {
    char path[1200];
    snprintf(path, sizeof path, "%sdata/scan_log.tsv", a->base_dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    time_t t = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", localtime(&t));
    fprintf(f, "%s\t%s\t%s\t%d\t%d\t%d\t%.1f\n", stamp, SCAN_WHAT[a->scan_mode],
            a->scan_phase ? "opponent" : "own", found, a->scan_n, a->scan_wrong, a->scan_secs);
    fclose(f);
}

static void scan_finish_phase(App *a) {
    int found = 0;
    for (int i = 0; i < a->scan_n; i++) found += a->scan_found[i];
    a->scan_secs = (double)(a->now - a->scan_start) / 1000.0;
    a->scan = SC_RESULT;
    a->scan_sess_found += found;
    a->scan_sess_total += a->scan_n;
    a->scan_sess_phases++;
    a->scan_sess_secs += a->scan_secs;
    scan_log(a, found);
    char missed[400] = "";
    for (int i = 0; i < a->scan_n; i++)
        if (!a->scan_found[i]) {
            char s[16];
            move_to_san(&a->scan_pos, a->scan_moves[i], s, sizeof s);
            strncat(missed, " ", sizeof missed - strlen(missed) - 1);
            strncat(missed, s, sizeof missed - strlen(missed) - 1);
        }
    bool all = found == a->scan_n;
    say(a, all ? MSG_GOOD : MSG_ERROR, "%d of %d in %.0f s%s%s%s.  Session: %d of %d.  Enter to go on.",
        found, a->scan_n, a->scan_secs, all ? " - all of them!" : ". Missed:", missed,
        a->scan_wrong ? "" : "", a->scan_sess_found, a->scan_sess_total);
}

static void scan_continue(App *a) {
    /* after your own moves, the same scan for the opponent (unless you're
     * in check, where "their move" isn't a legal position) */
    if (a->scan_phase == 0 && !in_check(&a->game.pos)) {
        a->scan_pos = a->game.pos;
        a->scan_pos.side = !a->scan_pos.side;
        a->scan_pos.ep = -1;
        a->scan_phase = 1;
        scan_begin_phase(a);
        say(a, MSG_INFO, "Now the threats: what could your opponent take or check with, if it were their move?");
        return;
    }
    train_item(a);
    if (train_step_full(a)) return;
    scan_next_position(a);
}

static void scan_start_session(App *a, const char *arg) {
    if (!scan_load(a)) { say(a, MSG_ERROR, "No scan positions: run analysis/report.py to make data/analysis/drills/scan.tsv."); return; }
    leave_modes(a);
    a->blind = false;
    a->scan_mode = !strncmp(arg, "cap", 3) ? 1 : !strncmp(arg, "che", 3) ? 2 : 0;
    a->scan_sess_found = a->scan_sess_total = a->scan_sess_phases = 0;
    a->scan_sess_secs = 0;
    scan_next_position(a);
}

static void scan_stop(App *a) {
    a->scan = SC_OFF;
    a->banner[0] = 0;
    new_game(a, NULL, a->pref_side);
    if (a->scan_sess_phases)
        say(a, MSG_INFO, "Scan session: %d of %d found (%.0f%%) over %d boards, %.0f s each on average.",
            a->scan_sess_found, a->scan_sess_total,
            a->scan_sess_total ? 100.0 * a->scan_sess_found / a->scan_sess_total : 0,
            a->scan_sess_phases, a->scan_sess_secs / a->scan_sess_phases);
}

static void scan_input(App *a, const char *line) {
    Move m;
    char why[256];
    SanResult r = san_parse(&a->scan_pos, line, false, &m, why, sizeof why);
    if (r == SAN_ERR) {
        a->scan_wrong++;
        say(a, MSG_ERROR, "%s - %s", line, why);
        return;
    }
    if (m.promo) m.promo = QUEEN;
    char san[16];
    move_to_san(&a->scan_pos, m, san, sizeof san);
    for (int i = 0; i < a->scan_n; i++)
        if (move_eq(a->scan_moves[i], m)) {
            if (a->scan_found[i]) { say(a, MSG_NOTE, "%s - you already have that one.", san); return; }
            a->scan_found[i] = true;
            int k = 0;
            for (int j = 0; j < a->scan_n; j++) k += a->scan_found[j];
            if (r == SAN_OK_NOTE) say(a, MSG_NOTE, "✓ (%d found)  %s", k, why);
            else say(a, MSG_GOOD, "%s  ✓   (%d found)", san, k);
            return;
        }
    a->scan_wrong++;
    say(a, MSG_ERROR, "%s is legal, but it isn't %s.", san, SCAN_NOT[a->scan_mode]);
}


/* -------------------------------------------------------- mode handling */

static void scan_stop(App *a);

/* stop whatever drill or game is running; the training run survives */
static void leave_modes(App *a) {
    opp_cancel(a->opp);
    a->drill = -1;
    a->scan = SC_OFF;
    a->threat = TH_OFF;
    a->sq_drill = false;
    a->lichess = false;
    a->review = RV_OFF;
    if (a->online == OL_OVER || a->online == OL_SEEKING) {
        ls_close(a->ol_seek); a->ol_seek = NULL;
        ls_close(a->ol_game); a->ol_game = NULL;
        ls_close(a->ol_events); a->ol_events = NULL;
        a->online = OL_OFF;
    }
    a->narrows = 0;
    a->banner[0] = 0;
    a->dstate = 0;
}

/* -------------------------------------------------------- threat spotting */

static bool threat_load(App *a) {
    if (a->th_list) return true;
    char path[1200];
    snprintf(path, sizeof path, "%sdata/analysis/drills/threats.tsv", a->base_dir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char *line = NULL;
    size_t cap = 0;
    int alloc = 0;
    while (getline(&line, &cap, f) > 0) {
        if (line[0] == '#') continue;
        if (a->th_count == alloc) {
            alloc = alloc ? alloc * 2 : 256;
            a->th_list = realloc(a->th_list, (size_t)alloc * sizeof *a->th_list);
        }
        ThreatPos *t = &a->th_list[a->th_count];
        char *save = NULL, *f1 = strtok_r(line, "\t\n", &save), *f2 = strtok_r(NULL, "\t\n", &save),
             *f3 = strtok_r(NULL, "\t\n", &save), *f4 = strtok_r(NULL, "\t\n", &save),
             *f5 = strtok_r(NULL, "\t\n", &save);
        if (!f1 || !f2 || !f3) continue;
        snprintf(t->fen_before, sizeof t->fen_before, "%s", f1);
        snprintf(t->last_move, sizeof t->last_move, "%s", f2);
        snprintf(t->answers, sizeof t->answers, "%s", f3);
        snprintf(t->played, sizeof t->played, "%s", f4 ? f4 : "");
        snprintf(t->note, sizeof t->note, "%s", f5 ? f5 : "");
        a->th_count++;
    }
    free(line);
    fclose(f);
    for (int i = a->th_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        ThreatPos tmp = a->th_list[i]; a->th_list[i] = a->th_list[j]; a->th_list[j] = tmp;
    }
    return a->th_count > 0;
}

static void threat_next(App *a) {
    for (int tries = 0; tries < a->th_count; tries++) {
        ThreatPos *t = &a->th_list[a->th_next++ % a->th_count];
        Position p;
        Move m;
        char why[256];
        if (!pos_from_fen(&p, t->fen_before) || san_parse(&p, t->last_move, false, &m, why, sizeof why) == SAN_ERR)
            continue;
        int me = !p.side;
        new_game(a, t->fen_before, me);
        a->saved = true;
        game_push(&a->game, m);
        a->animating = false;
        a->th_pos = a->game.pos;
        a->th_pos.side = !a->th_pos.side;
        a->th_pos.ep = -1;
        a->th_none = !strcmp(t->answers, "none");
        a->th_nans = 0;
        if (!a->th_none) {
            char buf[100], *save = NULL;
            snprintf(buf, sizeof buf, "%s", t->answers);
            for (char *s = strtok_r(buf, " ", &save); s && a->th_nans < 8; s = strtok_r(NULL, " ", &save))
                if (san_parse(&a->th_pos, s, false, &a->th_ans[a->th_nans], why, sizeof why) != SAN_ERR) a->th_nans++;
            if (a->th_nans == 0) continue;
        }
        a->threat = TH_ASK;
        a->narrows = 0;
        a->th_start = a->now;
        const char *them = me == WHITE ? "Black" : "White";
        snprintf(a->banner, sizeof a->banner, "Threat spotting  ·  position %d\n"
                 "%s just played %s. What does it threaten? Type the move %s wants to play next - or none.",
                 a->th_next, them, a->game.san[0], them);
        a->msg[0] = 0;
        return;
    }
    a->threat = TH_OFF;
    say(a, MSG_ERROR, "No usable positions in threats.tsv.");
}

static void threat_log(App *a, bool ok, const char *guess) {
    char path[1200];
    snprintf(path, sizeof path, "%sdata/threat_log.tsv", a->base_dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    time_t t = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", localtime(&t));
    ThreatPos *tp = &a->th_list[(a->th_next - 1) % a->th_count];
    fprintf(f, "%s\t%s\t%s\t%s\t%d\t%.1f\n", stamp, tp->fen_before, tp->answers, guess, ok,
            (double)(a->now - a->th_start) / 1000.0);
    fclose(f);
}

static void train_item(App *a);

static void threat_input(App *a, const char *line) {
    ThreatPos *tp = &a->th_list[(a->th_next - 1) % a->th_count];
    bool says_none = !strcasecmp(line, "none") || !strcasecmp(line, "nothing") || !strcasecmp(line, "no") ||
                     !strcmp(line, "-");
    Move g = {0};
    char why[256];
    if (!says_none && san_parse(&a->th_pos, line, false, &g, why, sizeof why) == SAN_ERR) {
        /* the parser speaks to the side to move; here that's the opponent */
        const char *them = a->th_pos.side == WHITE ? "White" : "Black";
        char fixed[300];
        if (!strncmp(why, "You have no ", 12)) snprintf(fixed, sizeof fixed, "%s has no %s", them, why + 12);
        else if (!strncmp(why, "You're in check", 15)) snprintf(fixed, sizeof fixed, "%s would be in check", them);
        else snprintf(fixed, sizeof fixed, "%s", why);
        say(a, MSG_ERROR, "%s - %s (typing %s's move)", line, fixed, them);
        return;
    }
    bool ok = false;
    if (says_none) ok = a->th_none;
    else for (int i = 0; i < a->th_nans; i++) if (move_eq(g, a->th_ans[i])) ok = true;
    a->threat = TH_RESULT;
    a->th_sess_n++;
    a->th_sess_ok += ok;
    threat_log(a, ok, line);
    train_item(a);
    /* arrows: the threats, plus a wrong guess in orange */
    a->narrows = 0;
    for (int i = 0; i < a->th_nans && a->narrows < ARROWS_MAX; i++) {
        a->arrows[a->narrows] = a->th_ans[i];
        a->arrow_col[a->narrows++] = ok ? 0 : 1;
    }
    if (!ok && !says_none && a->narrows < ARROWS_MAX) { a->arrows[a->narrows] = g; a->arrow_col[a->narrows++] = 2; }
    char ans[160] = "";
    for (int i = 0; i < a->th_nans; i++) {
        char s[16];
        move_to_san(&a->th_pos, a->th_ans[i], s, sizeof s);
        strncat(ans, i ? " or " : "", sizeof ans - strlen(ans) - 1);
        strncat(ans, s, sizeof ans - strlen(ans) - 1);
    }
    if (ok && a->th_none) say(a, MSG_GOOD, "Right - nothing serious is threatened. %s  ·  Enter for the next.", tp->note);
    else if (ok) say(a, MSG_GOOD, "Yes: %s. %s  ·  Enter for the next.", ans, tp->note);
    else if (a->th_none) say(a, MSG_ERROR, "No real threat here - %s doesn't win anything. %s  ·  Enter for the next.", line, tp->note);
    else say(a, MSG_ERROR, "The threat was %s (red). %s  ·  Enter for the next.", ans, tp->note);
}

static void threat_start(App *a) {
    if (!threat_load(a)) { say(a, MSG_ERROR, "No threat positions yet: run analysis/threats.py."); return; }
    leave_modes(a);
    a->th_sess_n = a->th_sess_ok = 0;
    a->blind = false;
    threat_next(a);
}

/* ------------------------------------------------------------ training */

static void sq_start(App *a);
static void train_end_step(App *a);
static void review_start(App *a, const char *arg);
static void drill_session(App *a, const char *kind);
static void scan_start_session(App *a, const char *arg);

static const char *DEFAULT_PLAN =
    "# Default plan: used until your coach writes data/plan.txt\n"
    "note Most of your lost points are middlegame blunders: pieces left hanging and "
    "captures missed. Today is about seeing the whole board before you move.\n"
    "coords off\n"
    "squares 1\n"
    "scan 5\n"
    "threats 8\n"
    "drill 6 puzzles\n"
    "drill 4 punish\n"
    "drill 3 lines\n";

static void plan_parse(App *a, const char *text) {
    a->plan_n = 0;
    a->plan_note[0] = 0;
    char buf[8192];
    snprintf(buf, sizeof buf, "%s", text);
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ') line++;
        if (!*line || *line == '#') continue;
        if (!strncmp(line, "note ", 5)) {
            if (a->plan_note[0]) strncat(a->plan_note, " ", sizeof a->plan_note - strlen(a->plan_note) - 1);
            strncat(a->plan_note, line + 5, sizeof a->plan_note - strlen(a->plan_note) - 1);
            continue;
        }
        if (!strncmp(line, "coords ", 7)) { a->coords = strncmp(line + 7, "off", 3) != 0; continue; }
        if (!strncmp(line, "blind ", 6)) { a->blind = strncmp(line + 6, "off", 3) != 0; continue; }
        if (a->plan_n >= PLAN_MAX) break;
        TrainStep *s = &a->plan[a->plan_n];
        memset(s, 0, sizeof *s);
        int n = sscanf(line, "%15s %d %31s", s->kind, &s->count, s->arg);
        if (n < 2 || s->count <= 0) continue;
        if (strcmp(s->kind, "squares") && strcmp(s->kind, "scan") && strcmp(s->kind, "threats") &&
            strcmp(s->kind, "drill") && strcmp(s->kind, "play") && strcmp(s->kind, "puzzles") && strcmp(s->kind, "review"))
            continue;
        a->plan_n++;
    }
}

static void plan_load(App *a) {
    const char *places[] = {"data/plan.txt", "plans/default.txt"};
    for (int i = 0; i < 2; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s%s", a->base_dir, places[i]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        static char text[8192];
        size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose(f);
        plan_parse(a, text);
        snprintf(a->plan_source, sizeof a->plan_source, "%s", places[i]);
        if (a->plan_n) return;
    }
    plan_parse(a, DEFAULT_PLAN);
    snprintf(a->plan_source, sizeof a->plan_source, "built-in default");
}

static void step_label(const TrainStep *s, char *out, size_t n) {
    if (!strcmp(s->kind, "squares")) snprintf(out, n, "Square naming: %d round%s of 30 s", s->count, s->count > 1 ? "s" : "");
    else if (!strcmp(s->kind, "scan")) snprintf(out, n, "Checks & captures: %d board%s%s%s", s->count, s->count > 1 ? "s" : "",
                                               s->arg[0] ? ", " : "", s->arg);
    else if (!strcmp(s->kind, "threats")) snprintf(out, n, "Threat spotting: %d position%s", s->count, s->count > 1 ? "s" : "");
    else if (!strcmp(s->kind, "drill")) {
        const char *w = !strncmp(s->arg, "puz", 3) || !strncmp(s->arg, "tac", 3) ? "Puzzles from your games"
                      : !strncmp(s->arg, "pun", 3) ? "Punish the blunder"
                      : !strncmp(s->arg, "line", 4) || !strncmp(s->arg, "open", 4) ? "Your opening lines"
                      : !strncmp(s->arg, "end", 3) ? "Endgames you let slip" : "Drills";
        snprintf(out, n, "%s: %d", w, s->count);
    } else if (!strcmp(s->kind, "review")) snprintf(out, n, "Review %d of your games yourself, then see the engine%s%s",
                                                    s->count, s->arg[0] ? ": " : "", s->arg);
    else if (!strcmp(s->kind, "puzzles")) snprintf(out, n, "Lichess puzzles: %d%s%s", s->count,
                                                     s->arg[0] ? ", " : "", s->arg);
    else if (!strcmp(s->kind, "play")) snprintf(out, n, "Play %d game%s against Stockfish%s%s", s->count,
                                                  s->count > 1 ? "s" : "", s->arg[0] ? " " : "", s->arg);
    else snprintf(out, n, "%s", s->kind);
}

static void train_log_step(App *a, const TrainStep *s) {
    char path[1200];
    snprintf(path, sizeof path, "%sdata/train_log.tsv", a->base_dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    time_t t = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", localtime(&t));
    fprintf(f, "%s\t%s %s\t%d\t%d\t%s\n", stamp, s->kind, s->arg, s->count, a->tdone, s->summary);
    fclose(f);
}

static void train_show_next(App *a, const char *done_line) {
    char label[160];
    step_label(&a->plan[a->tstep], label, sizeof label);
    snprintf(a->banner, sizeof a->banner, "Training  ·  step %d of %d\n%s%sNext: %s.",
             a->tstep + 1, a->plan_n, done_line ? done_line : "", done_line ? "\n" : "", label);
    a->twait = TW_BEGIN;
    say(a, MSG_INFO, "Press Enter to start.");
}

static void train_begin_step(App *a) {
    TrainStep *s = &a->plan[a->tstep];
    a->tdone = 0;
    a->twait = TW_NONE;
    if (!strcmp(s->kind, "squares")) { a->sq_sess_rounds = a->sq_sess_score = 0; sq_start(a); }
    else if (!strcmp(s->kind, "scan")) scan_start_session(a, s->arg);
    else if (!strcmp(s->kind, "threats")) threat_start(a);
    else if (!strcmp(s->kind, "drill")) drill_session(a, s->arg[0] ? s->arg : NULL);
    else if (!strcmp(s->kind, "puzzles")) lichess_session(a, s->arg);
    else if (!strcmp(s->kind, "review")) review_start(a, s->arg);
    else if (!strcmp(s->kind, "play")) {
        leave_modes(a);
        int lv = atoi(s->arg + (strncmp(s->arg, "level=", 6) ? 0 : 6));
        if (lv >= LEVEL_MIN && lv <= LEVEL_MAX) a->level = lv;
        new_game(a, NULL, rand() % 2 ? WHITE : BLACK);
        say(a, MSG_INFO, "Play it out. Before every move: what did their last move attack?");
    }
    /* a step with nothing to do (no drills due, no positions) is skipped */
    bool running = a->sq_drill || a->scan || a->threat || a->review || a->drill >= 0 || !strcmp(s->kind, "play");
    if (!running) {
        snprintf(s->summary, sizeof s->summary, "skipped (%s)", a->msg);
        train_end_step(a);
    }
}

static void train_end_step(App *a) {
    TrainStep *s = &a->plan[a->tstep];
    if (!s->summary[0]) {
        if (!strcmp(s->kind, "squares"))
            snprintf(s->summary, sizeof s->summary, "%d squares in %d round%s", a->sq_sess_score, a->sq_sess_rounds,
                     a->sq_sess_rounds == 1 ? "" : "s");
        else if (!strcmp(s->kind, "scan"))
            snprintf(s->summary, sizeof s->summary, "found %d of %d checks & captures (%.0f%%), %.0f s a board",
                     a->scan_sess_found, a->scan_sess_total,
                     a->scan_sess_total ? 100.0 * a->scan_sess_found / a->scan_sess_total : 0,
                     a->scan_sess_phases ? a->scan_sess_secs / a->scan_sess_phases : 0);
        else if (!strcmp(s->kind, "threats"))
            snprintf(s->summary, sizeof s->summary, "spotted %d of %d threats", a->th_sess_ok, a->th_sess_n);
        else if (!strcmp(s->kind, "drill"))
            snprintf(s->summary, sizeof s->summary, "%d of %d right first time", a->dsess_ok, a->dsess_n);
        else if (!strcmp(s->kind, "review"))
            snprintf(s->summary, sizeof s->summary, "found %d of %d mistakes yourself", a->rv_found, a->rv_total);
        else if (!strcmp(s->kind, "puzzles"))
            snprintf(s->summary, sizeof s->summary, "%d of %d solved, puzzle rating %.0f -> %.0f", a->dsess_ok,
                     a->dsess_n, a->lp_rating_start, a->lp.rating);
        else
            snprintf(s->summary, sizeof s->summary, "%d game%s played", a->tdone, a->tdone == 1 ? "" : "s");
    }
    train_log_step(a, s);
    char label[160], done[512];
    step_label(s, label, sizeof label);
    snprintf(done, sizeof done, "Done - %s: %s.", label, s->summary);
    leave_modes(a);
    a->tstep++;
    if (a->tstep >= a->plan_n) {
        a->twait = TW_FINISHED;
        int len = snprintf(a->banner, sizeof a->banner, "Training complete\n");
        for (int i = 0; i < a->plan_n && len < (int)sizeof a->banner; i++) {
            step_label(&a->plan[i], label, sizeof label);
            len += snprintf(a->banner + len, sizeof a->banner - (size_t)len, "%s: %s\n", label, a->plan[i].summary);
        }
        new_game(a, NULL, a->pref_side);
        say(a, MSG_GOOD, "Well done. Results are saved in data/train_log.tsv for your coach. Enter for free play.");
        return;
    }
    train_show_next(a, done);
}

/* called when one item of the current step is finished */
static void train_item(App *a) {
    if (a->training && a->twait == TW_NONE) a->tdone++;
}

/* at a "next item" point: true if the step's quota is met and the run moved on */
static bool train_step_full(App *a) {
    if (!a->training || a->twait != TW_NONE || a->tstep >= a->plan_n) return false;
    if (a->tdone < a->plan[a->tstep].count) return false;
    train_end_step(a);
    return true;
}

static void train_start(App *a) {
    leave_modes(a);
    plan_load(a);
    a->training = true;
    a->tstep = 0;
    for (int i = 0; i < a->plan_n; i++) a->plan[i].summary[0] = 0;
    int len = snprintf(a->banner, sizeof a->banner, "Today's training (%s)\n%s\n", a->plan_source, a->plan_note);
    for (int i = 0; i < a->plan_n && len < (int)sizeof a->banner; i++) {
        char label[160];
        step_label(&a->plan[i], label, sizeof label);
        len += snprintf(a->banner + len, sizeof a->banner - (size_t)len, "%d. %s\n", i + 1, label);
    }
    a->twait = TW_BEGIN;
    new_game(a, NULL, a->pref_side);
    say(a, MSG_INFO, "Press Enter to begin. /skip skips a step, /stop ends the run.");
}

static void train_stop(App *a) {
    leave_modes(a);
    a->training = false;
    a->twait = TW_NONE;
    new_game(a, NULL, a->pref_side);
    say(a, MSG_INFO, "Training stopped. /train to start again.");
}


/* ---------------------------------------------------------- self-review */

static const char *RV_TAGS[] = {"hung", "threat", "capture", "calc", "plan", "time", "other"};

static ReviewGame *rv_game(App *a) { return &a->rv.g[a->rv_cur]; }

static bool rv_my_ply(App *a, int ply) { return (ply % 2 == 0) == rv_game(a)->me_white; }

/* your winning chances before ply i */
static double rv_wp(App *a, int i) {
    ReviewGame *g = rv_game(a);
    return review_wp(g->me_white ? g->eval[i] : -g->eval[i]);
}

static void rv_arrows(App *a) {
    ReviewGame *g = rv_game(a);
    a->narrows = 0;
    int i = a->rv_ply;
    if (i >= g->n || !rv_my_ply(a, i)) return;
    char why[256];
    for (int k = 0; k < a->rv_nmarks; k++)
        if (a->rv_mark[k].ply == i) { a->arrows[a->narrows] = a->rv_mark[k].m; a->arrow_col[a->narrows++] = 2; }
    if (a->review == RV_SHOWN && a->rv_mistake[i]) {
        Move m;
        if (san_parse(&a->game.pos, g->san[i], false, &m, why, sizeof why) != SAN_ERR) {
            a->arrows[a->narrows] = m; a->arrow_col[a->narrows++] = 1;
        }
        if (strcmp(g->best[i], "-") && san_parse(&a->game.pos, g->best[i], false, &m, why, sizeof why) != SAN_ERR) {
            a->arrows[a->narrows] = m; a->arrow_col[a->narrows++] = 0;
        }
    }
}

static void rv_goto(App *a, int ply) {
    ReviewGame *g = rv_game(a);
    if (ply < 0) ply = 0;
    if (ply > g->n) ply = g->n;
    int me = g->me_white ? WHITE : BLACK;
    game_init(&a->game, NULL);
    a->human = me;
    a->saved = true;
    char why[256];
    for (int i = 0; i < ply; i++) {
        Move m;
        if (san_parse(&a->game.pos, g->san[i], false, &m, why, sizeof why) == SAN_ERR) { ply = i; break; }
        game_push(&a->game, m);
        if (i == ply - 1) { a->animating = true; a->anim_move = m; a->anim_start = a->now; }
    }
    a->rv_ply = ply;
    rv_arrows(a);
}

static void rv_banner(App *a) {
    ReviewGame *g = rv_game(a);
    double sc = review_score(g);
    const char *res = sc == 1 ? "won" : sc == 0 ? "lost" : "drew";
    int len = snprintf(a->banner, sizeof a->banner, "Self-review  ·  %s %s  ·  you %s as %s vs %s\n", g->date, g->speed,
                       res, g->me_white ? "White" : "Black", g->me_white ? g->black : g->white);
    if (a->review == RV_SELF)
        snprintf(a->banner + len, sizeof a->banner - (size_t)len,
                 "Engine hidden. Step through with the arrow keys (or Enter). At one of your moves you "
                 "think was wrong, type the better move and why, e.g. Nd2 threat. Why: hung, threat, "
                 "capture, calc, plan, time. Up to 3. Then /engine.");
}

static void review_open(App *a, int idx) {
    leave_modes(a);
    a->rv_cur = idx;
    a->review = RV_SELF;
    a->rv_nmarks = a->rv_found = a->rv_total = 0;
    memset(a->rv_mark, 0, sizeof a->rv_mark);
    memset(a->rv_mistake, 0, sizeof a->rv_mistake);
    a->blind = false;
    a->flipped = !rv_game(a)->me_white;
    rv_goto(a, 0);
    a->flipped = !rv_game(a)->me_white;
    rv_banner(a);
    say(a, MSG_INFO, "%s", rv_game(a)->opening);
}

static void review_start(App *a, const char *arg) {
    char err[300];
    if (!a->rv.n && !reviews_load(&a->rv, a->base_dir, err, sizeof err)) { say(a, MSG_ERROR, "%s", err); return; }
    int pick = -1;
    if (!strcmp(arg, "last")) pick = 0;
    for (int i = 0; i < a->rv.n && pick < 0; i++) {
        ReviewGame *g = &a->rv.g[i];
        if (g->reviewed) continue;
        double sc = review_score(g);
        if (!strcmp(arg, "win") && sc != 1) continue;
        if (!strcmp(arg, "loss") && sc != 0) continue;
        pick = i;
    }
    if (pick < 0) { say(a, MSG_INFO, "No unreviewed games%s%s. Try /review last.", arg[0] ? " of that kind: " : "", arg); return; }
    review_open(a, pick);
}

static void review_mark(App *a, const char *line) {
    ReviewGame *g = rv_game(a);
    int i = a->rv_ply;
    if (i >= g->n) { say(a, MSG_ERROR, "That's the end of the game. Go back to one of your moves."); return; }
    if (!rv_my_ply(a, i)) { say(a, MSG_ERROR, "It's your opponent's move here - step to one of yours (the position before you moved)."); return; }
    char mv[32] = "", tag[32] = "other";
    sscanf(line, "%31s %31s", mv, tag);
    bool tag_ok = false;
    for (size_t k = 0; k < sizeof RV_TAGS / sizeof RV_TAGS[0]; k++) if (!strcmp(tag, RV_TAGS[k])) tag_ok = true;
    if (!tag_ok) { say(a, MSG_ERROR, "Why '%s'? Use one of: hung, threat, capture, calc, plan, time, other.", tag); return; }
    Move m;
    char why[256];
    if (san_parse(&a->game.pos, mv, false, &m, why, sizeof why) == SAN_ERR) { say(a, MSG_ERROR, "%s - %s", mv, why); return; }
    char san[16];
    move_to_san(&a->game.pos, m, san, sizeof san);
    char a1[16], b1[16];
    snprintf(a1, sizeof a1, "%s", san); snprintf(b1, sizeof b1, "%s", g->san[i]);
    a1[strcspn(a1, "+#")] = 0; b1[strcspn(b1, "+#")] = 0;
    if (!strcmp(a1, b1)) { say(a, MSG_ERROR, "%s is what you played. Type the move you think you should have played instead.", san); return; }
    int slot = -1;
    for (int k = 0; k < a->rv_nmarks; k++) if (a->rv_mark[k].ply == i) slot = k;
    if (slot < 0) {
        if (a->rv_nmarks >= RV_MARKS) { say(a, MSG_ERROR, "That's 3 marks already - /engine to compare, or /unmark."); return; }
        slot = a->rv_nmarks++;
    }
    a->rv_mark[slot].ply = i;
    a->rv_mark[slot].m = m;
    snprintf(a->rv_mark[slot].tag, sizeof a->rv_mark[0].tag, "%s", tag);
    rv_arrows(a);
    say(a, MSG_GOOD, "Marked move %d: you played %s; better %s (%s). %d of %d marks - /engine when you're done.",
        i / 2 + 1, g->san[i], san, tag, a->rv_nmarks, RV_MARKS);
}

static void review_log(App *a) {
    FILE *f = fopen(a->rv.log_path, "a");
    if (!f) return;
    ReviewGame *g = rv_game(a);
    time_t t = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", localtime(&t));
    fprintf(f, "%s\t%s\t%s\t%d\t%d\t%d\t", stamp, g->id, g->result, a->rv_nmarks, a->rv_found, a->rv_total);
    for (int k = 0; k < a->rv_nmarks; k++) {
        char u[8];
        move_to_uci(a->rv_mark[k].m, u);
        fprintf(f, "%s%d:%s:%s", k ? ";" : "", a->rv_mark[k].ply, u, a->rv_mark[k].tag);
    }
    fprintf(f, "\n");
    fclose(f);
    g->reviewed = true;
}

static void review_reveal(App *a) {
    ReviewGame *g = rv_game(a);
    a->review = RV_SHOWN;
    a->rv_total = a->rv_found = 0;
    for (int i = 0; i < g->n; i++) {
        a->rv_mistake[i] = rv_my_ply(a, i) && rv_wp(a, i) - rv_wp(a, i + 1) >= 0.2;
        a->rv_total += a->rv_mistake[i];
    }
    char out[1200] = "";
    size_t len = 0;
    #define OUT(...) do { if (len < sizeof out) len += (size_t)snprintf(out + len, sizeof out - len, __VA_ARGS__); } while (0)
    char why[256];
    for (int k = 0; k < a->rv_nmarks; k++) {
        ReviewMark *mk = &a->rv_mark[k];
        int i = mk->ply;
        rv_goto(a, i);
        char mine[16];
        move_to_san(&a->game.pos, mk->m, mine, sizeof mine);
        double played = rv_wp(a, i + 1), best = rv_wp(a, i);
        if (a->rv_mistake[i]) { a->rv_found++; OUT("✓ Move %d %s was a mistake (%.0f%% → %.0f%%). ", i / 2 + 1, g->san[i], 100 * best, 100 * played); }
        else OUT("· Move %d %s was fine by the engine (%.0f%% → %.0f%%). ", i / 2 + 1, g->san[i], 100 * best, 100 * played);
        char b[16], m2[16];
        snprintf(b, sizeof b, "%s", g->best[i]); snprintf(m2, sizeof m2, "%s", mine);
        b[strcspn(b, "+#")] = 0; m2[strcspn(m2, "+#")] = 0;
        int cp;
        if (!strcmp(b, m2)) OUT("Your %s is the engine's move.\n", mine);
        else if (opp_eval_after(a->opp, &a->game.pos, mk->m, 200000, &cp))
            OUT("Your %s: %.0f%%; engine's %s: %.0f%%.\n", mine, 100 * review_wp(cp), g->best[i], 100 * best);
        else OUT("Engine preferred %s.\n", g->best[i]);
    }
    int missed = 0, first_missed = -1;
    for (int i = 0; i < g->n; i++) {
        if (!a->rv_mistake[i]) continue;
        bool marked = false;
        for (int k = 0; k < a->rv_nmarks; k++) if (a->rv_mark[k].ply == i) marked = true;
        if (marked) continue;
        if (first_missed < 0) first_missed = i;
        if (missed++ < 4)
            OUT("✗ Not spotted: move %d %s (%.0f%% → %.0f%%) - best %s.\n", i / 2 + 1, g->san[i],
                100 * rv_wp(a, i), 100 * rv_wp(a, i + 1), g->best[i]);
    }
    if (!a->rv_total) OUT("The engine found no big mistakes of yours in this game.\n");
    #undef OUT
    (void)why;
    int hl = snprintf(a->banner, sizeof a->banner, "Self-review  ·  engine verdict\n");
    snprintf(a->banner + hl, sizeof a->banner - (size_t)hl, "%s", out);
    review_log(a);
    train_item(a);
    rv_goto(a, first_missed >= 0 ? first_missed : a->rv_nmarks ? a->rv_mark[0].ply : a->rv_ply);
    say(a, a->rv_found == a->rv_total && a->rv_total ? MSG_GOOD : MSG_INFO,
        "You found %d of the %d mistakes the engine sees. Arrows: red played, green best, orange yours. "
        "%s", a->rv_found, a->rv_total, a->training ? "Enter to continue." : "/review for another game.");
}


/* ------------------------------------------------------ live lichess */

static bool ol_load_token(App *a) {
    const char *env = getenv("LICHESS_TOKEN");
    if (env && *env) { snprintf(a->ol_token, sizeof a->ol_token, "%s", env); return true; }
    char path[1200];
    snprintf(path, sizeof path, "%sdata/lichess_token", a->base_dir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[128] = "";
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    buf[strcspn(buf, " \t\r\n")] = 0;
    snprintf(a->ol_token, sizeof a->ol_token, "%s", buf);
    return a->ol_token[0] != 0;
}

static void ol_save_token(App *a, const char *tok) {
    char dir[1100], path[1200];
    snprintf(dir, sizeof dir, "%sdata", a->base_dir);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/lichess_token", dir);
    FILE *f = fopen(path, "w");
    if (!f) { say(a, MSG_ERROR, "Couldn't save the token to %s.", path); return; }
    fchmod(fileno(f), 0600);
    fprintf(f, "%s\n", tok);
    fclose(f);
    snprintf(a->ol_token, sizeof a->ol_token, "%s", tok);
    say(a, MSG_GOOD, "Token saved (data/lichess_token, readable only by you). Type /lichess to find a game.");
}

static void ol_close_all(App *a) {
    ls_close(a->ol_seek); a->ol_seek = NULL;
    ls_close(a->ol_game); a->ol_game = NULL;
    ls_close(a->ol_events); a->ol_events = NULL;
}

static void ol_rebuild(App *a, const char *moves, bool animate) {
    game_init(&a->game, NULL);
    a->saved = true;
    char buf[sizeof a->ol_moves], *save = NULL;
    snprintf(buf, sizeof buf, "%s", moves);
    Move m = {0};
    bool any = false;
    for (char *u = strtok_r(buf, " ", &save); u; u = strtok_r(NULL, " ", &save)) {
        if (!move_from_uci(&a->game.pos, u, &m)) break;
        game_push(&a->game, m);
        any = true;
    }
    if (any && animate) { a->animating = true; a->anim_move = m; a->anim_start = a->now; }
}

static const char *ol_status_text(const char *st) {
    if (!strcmp(st, "mate")) return "Checkmate";
    if (!strcmp(st, "resign")) return "Resignation";
    if (!strcmp(st, "outoftime")) return "Time ran out";
    if (!strcmp(st, "timeout")) return "Opponent left the game";
    if (!strcmp(st, "draw")) return "Draw";
    if (!strcmp(st, "stalemate")) return "Stalemate";
    if (!strcmp(st, "aborted")) return "Game aborted";
    if (!strcmp(st, "noStart")) return "Game didn't start";
    return st;
}

static void ol_apply_state(App *a, JNode *st) {
    const char *moves = json_str(st, "moves", "");
    if (strcmp(moves, a->ol_moves)) {
        bool theirs = false;
        snprintf(a->ol_moves, sizeof a->ol_moves, "%s", moves);
        ol_rebuild(a, a->ol_moves, true);
        theirs = a->game.nply > 0 && a->game.pos.side == a->human;
        if (theirs) a->msg[0] = 0;
    }
    a->ol_time[WHITE] = (long)json_num(st, "wtime", (double)a->ol_time[WHITE]);
    a->ol_time[BLACK] = (long)json_num(st, "btime", (double)a->ol_time[BLACK]);
    a->ol_clock_at = a->now;
    const char *status = json_str(st, "status", "started");
    snprintf(a->ol_status, sizeof a->ol_status, "%s", status);
    if (strcmp(status, "started") && strcmp(status, "created")) {
        const char *winner = json_str(st, "winner", "");
        const char *me = a->human == WHITE ? "white" : "black";
        const char *verdict = !winner[0] ? (strcmp(status, "aborted") && strcmp(status, "noStart") ? "Draw." : "")
                            : !strcmp(winner, me) ? "You won!" : "You lost.";
        a->online = OL_OVER;
        ol_close_all(a);
        say(a, !strcmp(winner, me) ? MSG_GOOD : MSG_NOTE,
            "%s. %s  It's on lichess.org/%s - make update brings it into your analysis. /lichess for another.",
            ol_status_text(status), verdict, a->ol_id);
        snprintf(a->banner, sizeof a->banner, "Lichess  ·  rated %d+%d  ·  finished\n%s vs %s", OL_MINUTES, OL_INCREMENT,
                 a->ol_name[WHITE], a->ol_name[BLACK]);
    }
}

static void ol_player(JNode *p, char *out, size_t n) {
    if (!p) { snprintf(out, n, "?"); return; }
    double ai = json_num(p, "aiLevel", 0);
    if (ai > 0) { snprintf(out, n, "lichess AI level %.0f", ai); return; }
    const char *name = json_str(p, "name", json_str(p, "id", "Anonymous"));
    double r = json_num(p, "rating", 0);
    if (r > 0) snprintf(out, n, "%s (%.0f%s)", name, r, json_bool(p, "provisional", false) ? "?" : "");
    else snprintf(out, n, "%s", name);
}

static void ol_start_game(App *a, const char *id, const char *color) {
    ls_close(a->ol_seek);
    a->ol_seek = NULL;
    leave_modes(a);
    snprintf(a->ol_id, sizeof a->ol_id, "%s", id);
    a->online = OL_PLAYING;
    a->human = !strcmp(color, "black") ? BLACK : WHITE;
    a->flipped = a->human == BLACK;
    a->ol_moves[0] = 0;
    a->ol_time[0] = a->ol_time[1] = OL_MINUTES * 60 * 1000L;
    a->ol_clock_at = a->now;
    snprintf(a->ol_status, sizeof a->ol_status, "started");
    game_init(&a->game, NULL);
    a->saved = true;
    char path[96];
    snprintf(path, sizeof path, "/api/board/game/stream/%s", id);
    a->ol_game = ls_open(path, NULL, a->ol_token);
    snprintf(a->banner, sizeof a->banner, "Lichess  ·  rated %d+%d  ·  you have %s\nBefore every move: what does their last move want? "
             "/resign, /draw, /abort (first moves only).", OL_MINUTES, OL_INCREMENT, a->human == WHITE ? "White" : "Black");
    say(a, MSG_GOOD, "Game on! lichess.org/%s", id);
}

static void ol_start(App *a) {
    if (a->online == OL_PLAYING) { say(a, MSG_ERROR, "You're already in a lichess game."); return; }
    if (!ol_load_token(a)) {
        const char *url = "https://lichess.org/account/oauth/token/create?scopes[]=board:play&description=Notation+Trainer";
        SDL_SetClipboardText(url);
        say(a, MSG_INFO, "To play on lichess you need a token. Open this page (copied to your clipboard), create the "
            "token, copy it, then type /token and paste it (Ctrl+V):  %s", url);
        return;
    }
    if (a->training) train_stop(a);
    leave_modes(a);
    long http = 0;
    char *body = lichess_get("/api/account", a->ol_token, &http, 15);
    JNode *acct = json_parse(body);
    if (!body || http != 200 || !acct) {
        say(a, MSG_ERROR, http == 401 ? "lichess rejected the token - make a new one and use /token."
                                      : "Couldn't reach lichess (HTTP %ld). Check your connection and try again.", http);
        free(body); json_free(acct);
        return;
    }
    snprintf(a->ol_user, sizeof a->ol_user, "%s", json_str(acct, "username", "you"));
    double r = json_num(acct, "perfs.rapid.rating", 0);
    json_free(acct);
    free(body);
    a->ol_events = ls_open("/api/stream/event", NULL, a->ol_token);
    char seek[160];
    snprintf(seek, sizeof seek, "rated=true&time=%d&increment=%d&variant=standard&color=random", OL_MINUTES, OL_INCREMENT);
    a->ol_seek = ls_open("/api/board/seek", seek, a->ol_token);
    a->online = OL_SEEKING;
    a->ol_since = a->now;
    new_game(a, NULL, a->pref_side);
    snprintf(a->banner, sizeof a->banner, "Lichess  ·  %s%s%.0f\nLooking for a rated %d+%d game. It usually takes under a minute. /stop cancels.",
             a->ol_user, r > 0 ? "  ·  rapid " : "", r, OL_MINUTES, OL_INCREMENT);
    a->msg[0] = 0;
}

static void ol_stop(App *a) {
    ol_close_all(a);
    a->online = OL_OFF;
    a->banner[0] = 0;
    new_game(a, NULL, a->pref_side);
    say(a, MSG_INFO, "Stopped looking for a lichess game.");
}

/* called every loop: read whatever the streams have delivered */
static bool ol_poll(App *a) {
    if (a->online == OL_OFF || a->online == OL_OVER) return false;
    bool changed = false;
    char *line;
    while ((line = ls_next_line(a->ol_events))) {
        JNode *j = json_parse(line);
        const char *type = json_str(j, "type", "");
        if (!strcmp(type, "gameStart") && a->online == OL_SEEKING) {
            const char *id = json_str(j, "game.gameId", json_str(j, "game.id", ""));
            /* only standard games at the trainer's time control get picked up */
            if (id[0]) ol_start_game(a, id, json_str(j, "game.color", "white"));
        }
        json_free(j);
        free(line);
        changed = true;
    }
    while ((line = ls_next_line(a->ol_seek))) {
        JNode *j = json_parse(line);
        const char *err = json_str(j, "error", NULL);
        if (err) say(a, MSG_ERROR, "lichess: %s", err);
        json_free(j);
        free(line);
        changed = true;
    }
    long http;
    char err[256];
    if (a->online == OL_SEEKING && ls_finished(a->ol_seek, &http, err, sizeof err)) {
        ls_close(a->ol_seek);
        a->ol_seek = NULL;
        if (http != 200) {
            ol_close_all(a);
            a->online = OL_OFF;
            if (a->msg_kind != MSG_ERROR) say(a, MSG_ERROR, "The seek ended (HTTP %ld%s%s). /lichess to try again.", http, err[0] ? ": " : "", err);
        }
        changed = true;
    }
    while (a->ol_game && (line = ls_next_line(a->ol_game))) {
        JNode *j = json_parse(line);
        const char *type = json_str(j, "type", "");
        if (!strcmp(type, "gameFull")) {
            ol_player(json_get(j, "white"), a->ol_name[WHITE], sizeof a->ol_name[0]);
            ol_player(json_get(j, "black"), a->ol_name[BLACK], sizeof a->ol_name[0]);
            ol_apply_state(a, json_get(j, "state"));
        } else if (!strcmp(type, "gameState")) {
            ol_apply_state(a, j);
        } else if (!strcmp(type, "opponentGone")) {
            if (json_bool(j, "gone", false))
                say(a, MSG_NOTE, "Your opponent left. If they don't return in %.0f s, type /claim to win.",
                    json_num(j, "claimWinInSeconds", 0));
        }
        json_free(j);
        free(line);
        changed = true;
        if (a->online != OL_PLAYING) return true;
    }
    if (a->online == OL_PLAYING && ls_finished(a->ol_game, &http, err, sizeof err)) {
        /* dropped connection: reopen; the stream resends the full game */
        ls_close(a->ol_game);
        char path[96];
        snprintf(path, sizeof path, "/api/board/game/stream/%s", a->ol_id);
        a->ol_game = ls_open(path, NULL, a->ol_token);
        changed = true;
    }
    if (a->online != OL_OFF && a->ol_events && ls_finished(a->ol_events, NULL, NULL, 0)) {
        ls_close(a->ol_events);
        a->ol_events = ls_open("/api/stream/event", NULL, a->ol_token);
    }
    if (lichess_error(err, sizeof err)) {
        say(a, MSG_ERROR, "%s", err);
        if (a->online == OL_PLAYING) ol_rebuild(a, a->ol_moves, false); /* undo an optimistic move */
        changed = true;
    }
    return changed;
}

static void ol_user_move(App *a, Move m) {
    char uci[8], path[96];
    move_to_uci(m, uci);
    game_push(&a->game, m);
    a->animating = true;
    a->anim_move = m;
    a->anim_start = a->now;
    snprintf(path, sizeof path, "/api/board/game/%s/move/%s", a->ol_id, uci);
    lichess_post(path, "", a->ol_token);
}

static bool ol_command(App *a, const char *cmd, const char *arg) {
    (void)arg;
    char path[96];
    if (!strcmp(cmd, "resign")) {
        snprintf(path, sizeof path, "/api/board/game/%s/resign", a->ol_id);
        lichess_post(path, "", a->ol_token);
        say(a, MSG_NOTE, "Resigning...");
    } else if (!strcmp(cmd, "draw")) {
        snprintf(path, sizeof path, "/api/board/game/%s/draw/yes", a->ol_id);
        lichess_post(path, "", a->ol_token);
        say(a, MSG_INFO, "Draw offered (or accepted, if they offered one).");
    } else if (!strcmp(cmd, "abort")) {
        snprintf(path, sizeof path, "/api/board/game/%s/abort", a->ol_id);
        lichess_post(path, "", a->ol_token);
        say(a, MSG_INFO, "Aborting...");
    } else if (!strcmp(cmd, "claim")) {
        snprintf(path, sizeof path, "/api/board/game/%s/claim-victory", a->ol_id);
        lichess_post(path, "", a->ol_token);
    } else {
        return false;
    }
    return true;
}

static const char *HELP =
    "Moves: standard notation - e4, Nf3, exd5, Nbd7, R1e2, O-O, e8=Q. Check signs optional "
    "(required in /strict). Up/Down recall earlier input.  "
    "/new [white|black] · /flip · /undo · /level 1-10 · /blind (hide pieces) · /peek (3 s look) · "
    "/coords (board labels) · /squares (30 s square-naming drill) · "
    "/drill [puzzles|punish|lines|endgames] · /answer · /next · /play (leave drills) · "
    "/scan [captures|checks] (list every check and capture) · /threats (what does their move threaten?) · "
    "/train (today's guided session) · /skip · /stop · /puzzles [themes] (lichess, at your level) · "
    "/review [win|loss|last] (analyse a game yourself, then /engine) · "
    "/lichess (a rated 15+10 game on lichess; /token to set up) · "
    "/strict · /pgn (save) · /fen (copy) · /resign · /quit";

static void next_square(App *a) {
    int t;
    do t = rand() % 64; while (t == a->sq_target);
    a->sq_target = t;
}

static void sq_start(App *a) {
    leave_modes(a);
    a->sq_drill = true;
    a->sq_score = a->sq_misses = 0;
    a->sq_end = a->now + SQ_DRILL_MS;
    a->sq_flash = 0;
    next_square(a);
    say(a, MSG_INFO, "Name each highlighted square - just type it, no Enter. Esc stops.");
}

static void end_square_drill(App *a, bool finished) {
    a->sq_drill = false;
    if (!finished) { say(a, MSG_INFO, "Square drill stopped."); return; }
    bool record = a->sq_score > a->sq_best;
    if (record) a->sq_best = a->sq_score;
    if (a->training && a->twait == TW_NONE) {
        train_item(a);
        a->sq_sess_rounds++;
        a->sq_sess_score += a->sq_score;
        char res[120];
        snprintf(res, sizeof res, "%d right, %d wrong.%s", a->sq_score, a->sq_misses, record ? " New best!" : "");
        if (train_step_full(a)) { say(a, MSG_GOOD, "%s Press Enter to start the next step.", res); return; }
        a->twait = TW_AGAIN;
        say(a, MSG_GOOD, "%s Enter for the next round.", res);
        return;
    }
    say(a, record ? MSG_GOOD : MSG_INFO, "Squares: %d right, %d wrong in 30 s.%s Best: %d. /squares again, /flip to drill from Black's side.",
        a->sq_score, a->sq_misses, record ? " New best!" : "", a->sq_best);
}

/* in the drill, two typed characters are an answer */
static void square_answer(App *a) {
    char name[3];
    sq_name(a->sq_target, name);
    if (a->input[0] == name[0] && a->input[1] == name[1]) {
        a->sq_score++;
        a->msg[0] = 0;
        next_square(a);
    } else {
        a->sq_misses++;
        a->sq_flash = a->now;
        say(a, MSG_ERROR, "%s - that was %s.", a->input, name);
        next_square(a);
    }
    a->input_len = 0;
    a->input[0] = 0;
}

static bool command(App *a, const char *line) {
    char cmd[32] = "", arg[64] = "";
    sscanf(line, "/%31s %63s", cmd, arg);
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "q")) return false;
    if (a->online == OL_PLAYING) {
        /* fair play: during a live game only game commands work - no engine, drills or hints */
        if (ol_command(a, cmd, arg)) return true;
        if (strcmp(cmd, "flip") && strcmp(cmd, "coords") && strcmp(cmd, "help") && strcmp(cmd, "h") &&
            strcmp(cmd, "?") && strcmp(cmd, "strict")) {
            say(a, MSG_ERROR, "Not during a lichess game. Game commands: /resign /draw /abort /claim /flip /coords.");
            return true;
        }
    }
    if (!strcmp(cmd, "lichess") || !strcmp(cmd, "online")) { ol_start(a); return true; }
    if (!strcmp(cmd, "token")) {
        if (!arg[0]) { say(a, MSG_ERROR, "Paste the token after /token, e.g. /token lip_abc123 (Ctrl+V pastes)."); return true; }
        ol_save_token(a, arg);
        return true;
    }
    if (!strcmp(cmd, "stop") && a->online == OL_SEEKING) { ol_stop(a); return true; }
    if (!strcmp(cmd, "new") || !strcmp(cmd, "n")) {
        if (a->training) train_stop(a);
        leave_modes(a);
        int side = !strcmp(arg, "black") || !strcmp(arg, "b") ? BLACK
                   : !strcmp(arg, "white") || !strcmp(arg, "w") ? WHITE : a->pref_side;
        a->pref_side = side;
        new_game(a, NULL, side);
        say(a, MSG_INFO, "New game - you have %s.", side == WHITE ? "White" : "Black");
    } else if (!strcmp(cmd, "flip")) {
        a->flipped = !a->flipped;
        a->msg[0] = 0;
    } else if (!strcmp(cmd, "undo") || !strcmp(cmd, "u")) {
        if (a->drill >= 0) { say(a, MSG_ERROR, "No take-backs in drills. /answer to see the solution."); return true; }
        opp_cancel(a->opp);
        if (a->game.nply == 0) { say(a, MSG_ERROR, "Nothing to take back."); return true; }
        game_pop(&a->game);
        while (a->game.nply > 0 && a->game.pos.side != a->human) game_pop(&a->game);
        a->animating = false;
        a->saved = false;
        say(a, MSG_INFO, "Taken back.");
    } else if (!strcmp(cmd, "level") || !strcmp(cmd, "l")) {
        int lv = atoi(arg);
        if (lv < LEVEL_MIN || lv > LEVEL_MAX) { say(a, MSG_ERROR, "Level is 1 to 10, e.g. /level 4."); return true; }
        a->level = lv;
        char n[96];
        opp_name(a->opp, lv, n, sizeof n);
        say(a, MSG_INFO, "Opponent: %s.", n);
    } else if (!strcmp(cmd, "blind")) {
        a->blind = !a->blind;
        say(a, MSG_INFO, a->blind ? "Blindfold on. /peek shows the pieces for 3 seconds." : "Blindfold off.");
    } else if (!strcmp(cmd, "train") || !strcmp(cmd, "t")) {
        train_start(a);
    } else if (!strcmp(cmd, "skip")) {
        if (!a->training || a->twait == TW_FINISHED) { say(a, MSG_ERROR, "Not in a training run. /train starts one."); return true; }
        if (a->twait == TW_BEGIN && a->tstep < a->plan_n) {
            snprintf(a->plan[a->tstep].summary, sizeof a->plan[0].summary, "skipped");
            a->tdone = 0;
        } else if (a->tdone == 0) {
            snprintf(a->plan[a->tstep].summary, sizeof a->plan[0].summary, "skipped");
        }
        a->twait = TW_NONE;
        train_end_step(a);
    } else if (!strcmp(cmd, "stop")) {
        if (a->training) train_stop(a);
        else { leave_modes(a); new_game(a, NULL, a->pref_side); say(a, MSG_INFO, "Stopped."); }
    } else if (!strcmp(cmd, "review") || !strcmp(cmd, "rv")) {
        review_start(a, arg);
    } else if (!strcmp(cmd, "engine") || !strcmp(cmd, "reveal")) {
        if (a->review != RV_SELF) { say(a, MSG_ERROR, "Start a self-review first: /review."); return true; }
        review_reveal(a);
    } else if (!strcmp(cmd, "unmark")) {
        if (a->review == RV_SELF && a->rv_nmarks) { a->rv_nmarks--; rv_arrows(a); say(a, MSG_INFO, "Last mark removed."); }
    } else if (!strcmp(cmd, "puzzles") || !strcmp(cmd, "pz")) {
        lichess_session(a, arg);
    } else if (!strcmp(cmd, "threats") || !strcmp(cmd, "threat")) {
        threat_start(a);
    } else if (!strcmp(cmd, "scan") || !strcmp(cmd, "cc")) {
        scan_start_session(a, arg);
    } else if (!strcmp(cmd, "done")) {
        if (a->scan == SC_ASK) scan_finish_phase(a);
    } else if (!strcmp(cmd, "drill") || !strcmp(cmd, "d")) {
        drill_session(a, arg[0] ? arg : NULL);
    } else if (!strcmp(cmd, "next")) {
        if (a->drill < 0) { say(a, MSG_ERROR, "Not in a drill. /drill starts one."); return true; }
        drill_next(a);
    } else if (!strcmp(cmd, "answer") || !strcmp(cmd, "giveup")) {
        Drill *d = cur_drill(a);
        if (!d || a->dstate == DS_DONE) { say(a, MSG_ERROR, "No drill to give up."); return true; }
        if (!strcmp(d->kind, "endgame")) { opp_cancel(a->opp); drill_finish(a, false, "Given up. "); }
        else drill_show(a, NULL);
    } else if (!strcmp(cmd, "play")) {
        if (a->scan) scan_stop(a);
        if (a->training) { train_stop(a); return true; }
        leave_modes(a);
        new_game(a, NULL, a->pref_side);
        say(a, MSG_INFO, "Free play.");
    } else if (!strcmp(cmd, "coords")) {
        a->coords = !a->coords;
        say(a, MSG_INFO, a->coords ? "Coordinates on." : "Coordinates off. /squares drills naming squares.");
    } else if (!strcmp(cmd, "squares") || !strcmp(cmd, "sq")) {
        sq_start(a);
    } else if (!strcmp(cmd, "peek")) {
        a->peek_until = a->now + 3000;
        a->msg[0] = 0;
    } else if (!strcmp(cmd, "strict")) {
        a->strict = !a->strict;
        say(a, MSG_INFO, a->strict ? "Strict: moves must be exact, including + and #." : "Strict off.");
    } else if (!strcmp(cmd, "pgn")) {
        char path[1200];
        if (save_pgn(a, path, sizeof path)) say(a, MSG_GOOD, "Saved %s", path);
        else say(a, MSG_ERROR, "Couldn't save the game.");
    } else if (!strcmp(cmd, "fen")) {
        char fen[100];
        pos_to_fen(&a->game.pos, fen, sizeof fen);
        SDL_SetClipboardText(fen);
        say(a, MSG_INFO, "%s  (copied)", fen);
    } else if (!strcmp(cmd, "resign")) {
        if (game_status(&a->game) != ST_ONGOING) return true;
        opp_cancel(a->opp);
        a->game.forced = ST_RESIGNED;
        a->game.resigned_side = a->human;
        char path[1200];
        a->saved = save_pgn(a, path, sizeof path);
        say(a, MSG_NOTE, "You resigned. Saved to games/. /new for another.");
    } else if (!strcmp(cmd, "help") || !strcmp(cmd, "h") || !strcmp(cmd, "?")) {
        say(a, MSG_INFO, "%s", HELP);
    } else {
        say(a, MSG_ERROR, "Unknown command /%s. Try /help.", cmd);
    }
    return true;
}

static bool submit(App *a) {
    char line[64];
    snprintf(line, sizeof line, "%s", a->input);
    a->input[0] = 0;
    a->input_len = 0;
    if (!line[0]) {
        if (a->twait == TW_BEGIN) train_begin_step(a);
        else if (a->twait == TW_AGAIN) { a->twait = TW_NONE; sq_start(a); }
        else if (a->twait == TW_FINISHED) { a->training = false; a->twait = TW_NONE; a->banner[0] = 0; say(a, MSG_INFO, "Free play."); }
        else if (a->review == RV_SELF) rv_goto(a, a->rv_ply + 1);
        else if (a->review == RV_SHOWN) { if (!train_step_full(a)) rv_goto(a, a->rv_ply + 1); }
        else if (a->scan == SC_ASK) scan_finish_phase(a);
        else if (a->scan == SC_RESULT) scan_continue(a);
        else if (a->threat == TH_RESULT) { if (!train_step_full(a)) threat_next(a); }
        else if (a->drill >= 0 && a->dstate == DS_DONE) { if (!train_step_full(a)) drill_next(a); }
        else if (a->training && a->tstep < a->plan_n && !strcmp(a->plan[a->tstep].kind, "play") &&
                 game_status(&a->game) != ST_ONGOING) {
            if (!train_step_full(a)) { new_game(a, NULL, rand() % 2 ? WHITE : BLACK); a->saved = false; say(a, MSG_INFO, "Next game."); }
        }
        return true;
    }
    if (a->nhist == 0 || strcmp(a->history[(a->nhist - 1) % HISTORY], line) != 0)
        snprintf(a->history[a->nhist++ % HISTORY], 64, "%s", line);
    a->hist_pos = a->nhist;

    if (line[0] == '/') return command(a, line);
    if (a->twait == TW_BEGIN || a->twait == TW_AGAIN) { say(a, MSG_INFO, "Press Enter to start."); return true; }
    if (a->scan == SC_ASK) { scan_input(a, line); return true; }
    if (a->threat == TH_ASK) { threat_input(a, line); return true; }
    if (a->review == RV_SELF) { review_mark(a, line); return true; }
    if (a->review == RV_SHOWN) { say(a, MSG_INFO, "Arrow keys to look around; /review for another game."); return true; }
    if (a->threat == TH_RESULT) { say(a, MSG_INFO, "Press Enter for the next one."); return true; }
    if (a->scan == SC_RESULT) { say(a, MSG_INFO, "Press Enter for the next one, or /play to stop."); return true; }
    if (a->drill >= 0 && a->dstate == DS_DONE) { say(a, MSG_INFO, "Press Enter for the next drill."); return true; }
    if (a->drill >= 0 && a->dstate == DS_AUTO) { say(a, MSG_INFO, "Wait a moment..."); return true; }
    if (a->online == OL_SEEKING) { say(a, MSG_INFO, "Still looking for an opponent... /stop cancels."); return true; }
    if (a->online == OL_PLAYING && game_status(&a->game) != ST_ONGOING) { say(a, MSG_INFO, "Waiting for lichess..."); return true; }
    if (game_status(&a->game) != ST_ONGOING) { say(a, MSG_ERROR, "The game is over - /new to start another."); return true; }
    if (a->game.pos.side != a->human) { say(a, MSG_ERROR, "Not your move yet."); return true; }

    Move m;
    char why[256];
    SanResult r = san_parse(&a->game.pos, line, a->strict, &m, why, sizeof why);
    if (r == SAN_ERR) { say(a, MSG_ERROR, "%s - %s", line, why); return true; }
    if (r == SAN_OK_NOTE) say(a, MSG_NOTE, "%s", why);
    else a->msg[0] = 0;
    if (a->drill >= 0 && a->dstate == DS_ASK) { drill_user_move(a, m, line); return true; }
    if (a->online == OL_PLAYING) { ol_user_move(a, m); return true; }
    game_push(&a->game, m);
    after_move(a, m);
    return true;
}

static void base_dir(char *out, size_t n) {
    const char *b = SDL_GetBasePath();
    snprintf(out, n, "%s", b ? b : "./");
}

static void usage(void) {
    fprintf(stderr,
            "usage: chess [train] [--black] [--level N] [--fen FEN] [--moves \"e4 e5 ...\"]\n"
            "             [--screenshot out.png [--size WxH] [--blind]]\n");
}

int main(int argc, char **argv) {
    static App app;
    App *a = &app;
    a->level = 3;
    a->human = WHITE;
    a->coords = true;
    a->drill = -1;
    srand(getenv("CHESS_SEED") ? (unsigned)atoi(getenv("CHESS_SEED")) : (unsigned)time(NULL));
    const char *fen = NULL, *moves = NULL, *shot = NULL;
    int shot_w = 1280, shot_h = 800;
    bool shot_blind = false, start_training = false, start_online = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--black")) a->human = BLACK;
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) a->level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fen") && i + 1 < argc) fen = argv[++i];
        else if (!strcmp(argv[i], "--moves") && i + 1 < argc) moves = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &shot_w, &shot_h);
        else if (!strcmp(argv[i], "--blind")) shot_blind = true;
        else if (!strcmp(argv[i], "--no-coords")) a->coords = false;
        else if (!strcmp(argv[i], "--train") || !strcmp(argv[i], "train")) start_training = true;
        else if (!strcmp(argv[i], "--lichess") || !strcmp(argv[i], "lichess")) start_online = true;
        else { usage(); return 2; }
    }
    a->pref_side = a->human;
    if (a->level < LEVEL_MIN) a->level = LEVEL_MIN;
    if (a->level > LEVEL_MAX) a->level = LEVEL_MAX;

    base_dir(a->base_dir, sizeof a->base_dir);
    if (!render_init(a->base_dir)) fprintf(stderr, "warning: some piece images are missing\n");
    a->opp = opp_create(a->base_dir);
    new_game(a, fen, a->human);
    if (moves) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", moves);
        for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
            if (strchr(tok, '.')) continue;
            Move m;
            char why[256];
            if (san_parse(&a->game.pos, tok, false, &m, why, sizeof why) == SAN_ERR) {
                fprintf(stderr, "--moves: %s: %s\n", tok, why);
                return 2;
            }
            game_push(&a->game, m);
        }
    }

    if (start_training) train_start(a);

    if (shot) { /* render one frame to a PNG, no window */
        a->blind = shot_blind;
        if (getenv("SHOT_SQUARES")) { a->sq_drill = true; a->sq_target = 28; a->sq_score = 7; a->sq_end = 18000; }
        a->now = 0;
        a->msg[0] = 0;
        if (getenv("SHOT_INPUT")) { snprintf(a->input, sizeof a->input, "%s", getenv("SHOT_INPUT")); a->input_len = (int)strlen(a->input); }
        if (getenv("SHOT_TRY")) {
            Move m; char why[256];
            SanResult r = san_parse(&a->game.pos, getenv("SHOT_TRY"), a->strict, &m, why, sizeof why);
            if (r == SAN_ERR) say(a, MSG_ERROR, "%s - %s", getenv("SHOT_TRY"), why);
            else if (r == SAN_OK_NOTE) say(a, MSG_NOTE, "%s", why);
        }
        if (getenv("SHOT_SCRIPT")) {
            /* test harness: ';'-separated inputs, time advanced after each */
            char buf[4096];
            snprintf(buf, sizeof buf, "%s", getenv("SHOT_SCRIPT"));
            a->now = 100000;
            char *save = NULL;
            for (char *tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
                snprintf(a->input, sizeof a->input, "%s", strcmp(tok, ".") ? tok : ""); /* "." = Enter */
                a->input_len = (int)strlen(a->input);
                submit(a);
                for (int k = 0; k < 8; k++) { a->now += 1000; a->animating = false; drill_tick(a); }
                for (int k = 0; a->online && k < 30; k++) { ol_poll(a); usleep(50000); a->now += 50; }
                char fen[100];
                pos_to_fen(&a->game.pos, fen, sizeof fen);
                printf("> %-10s | %s\n  msg: %s\n", tok, fen, a->msg);
            }
            if (a->banner[0]) printf("banner: %s\n", a->banner);
        }
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, shot_w, shot_h);
        cairo_t *cr = cairo_create(s);
        render_frame(a, cr, shot_w, shot_h, 1.0);
        cairo_destroy(cr);
        int rc = cairo_surface_write_to_png(s, shot) == CAIRO_STATUS_SUCCESS ? 0 : 1;
        cairo_surface_destroy(s);
        opp_destroy(a->opp);
        render_shutdown();
        return rc;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win;
    SDL_Renderer *ren;
    if (!SDL_CreateWindowAndRenderer("Notation Trainer", 1280, 800,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 900, 600);
    SDL_SetRenderVSync(ren, 1);
    SDL_StartTextInput(win);

    SDL_Texture *tex = NULL;
    cairo_surface_t *surf = NULL;
    int tw = 0, th = 0;
    bool running = true, dirty = true;
    uint64_t last_blink = 0;
    GameStatus last_status = ST_ONGOING;
    char opp_line[96];
    opp_name(a->opp, a->level, opp_line, sizeof opp_line);
    if (!a->training) say(a, MSG_INFO, "Opponent: %s. Type your move and press Enter. /help for commands.", opp_line);
    if (start_online) { a->now = SDL_GetTicks(); ol_start(a); }

    while (running) {
        a->now = SDL_GetTicks();
        bool busy = a->animating || opp_busy(a->opp) || a->now < a->peek_until || a->sq_drill || a->online ||
                    (a->drill >= 0 && a->dstate == DS_AUTO);
        if (a->sq_drill && a->now >= a->sq_end) { end_square_drill(a, true); }
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, busy ? 8 : 60)) {
            do {
                switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_EXPOSED: dirty = true; break;
                case SDL_EVENT_TEXT_INPUT:
                    for (const char *c = e.text.text; *c; c++)
                        if (*c >= 32 && *c < 127 && a->input_len < (int)sizeof a->input - 1) {
                            a->input[a->input_len++] = *c;
                            a->input[a->input_len] = 0;
                            if (a->sq_drill && a->input[0] != '/' && a->input_len == 2) square_answer(a);
                        }
                    dirty = true;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    dirty = true;
                    switch (e.key.key) {
                    case SDLK_RETURN: case SDLK_KP_ENTER: running = submit(a); break;
                    case SDLK_BACKSPACE:
                        if (e.key.mod & SDL_KMOD_CTRL) a->input_len = 0;
                        else if (a->input_len) a->input_len--;
                        a->input[a->input_len] = 0;
                        break;
                    case SDLK_V:
                        if (e.key.mod & SDL_KMOD_CTRL) {
                            char *clip = SDL_GetClipboardText();
                            for (const char *c = clip ? clip : ""; *c; c++)
                                if (*c > 32 && *c < 127 && a->input_len < (int)sizeof a->input - 1) a->input[a->input_len++] = *c;
                                else if (*c == ' ' && a->input_len && a->input_len < (int)sizeof a->input - 1) a->input[a->input_len++] = ' ';
                            a->input[a->input_len] = 0;
                            SDL_free(clip);
                        }
                        break;
                    case SDLK_LEFT: if (a->review) rv_goto(a, a->rv_ply - 1); break;
                    case SDLK_RIGHT: if (a->review) rv_goto(a, a->rv_ply + 1); break;
                    case SDLK_HOME: if (a->review) rv_goto(a, 0); break;
                    case SDLK_END: if (a->review) rv_goto(a, rv_game(a)->n); break;
                    case SDLK_ESCAPE:
                        if (a->sq_drill && a->input_len == 0) end_square_drill(a, false);
                        a->input_len = 0; a->input[0] = 0;
                        break;
                    case SDLK_UP:
                        if (a->hist_pos > 0 && a->hist_pos > a->nhist - HISTORY) {
                            a->hist_pos--;
                            snprintf(a->input, sizeof a->input, "%s", a->history[a->hist_pos % HISTORY]);
                            a->input_len = (int)strlen(a->input);
                        }
                        break;
                    case SDLK_DOWN:
                        if (a->hist_pos < a->nhist) a->hist_pos++;
                        if (a->hist_pos == a->nhist) a->input[0] = 0;
                        else snprintf(a->input, sizeof a->input, "%s", a->history[a->hist_pos % HISTORY]);
                        a->input_len = (int)strlen(a->input);
                        break;
                    }
                    break;
                }
            } while (SDL_PollEvent(&e));
        }
        a->now = SDL_GetTicks();

        if (a->animating && a->now - a->anim_start >= ANIM_MS) { a->animating = false; dirty = true; }

        Move m;
        if (opp_poll(a->opp, &m)) {
            game_push(&a->game, m);
            after_move(a, m);
            dirty = true;
        }
        GameStatus st = game_status(&a->game);
        drill_tick(a);
        st = game_status(&a->game);
        if (ol_poll(a)) dirty = true;
        bool engine_plays = (a->drill < 0 || a->dstate == DS_PLAY) && !a->scan && !a->threat && !a->review && !a->online &&
                            !a->sq_drill && (a->twait == TW_NONE || !a->training);
        if (engine_plays && st == ST_ONGOING && a->game.pos.side != a->human && !opp_busy(a->opp) && !a->animating) {
            opp_go(a->opp, &a->game, a->drill >= 0 ? LEVEL_MAX : a->level);
            dirty = true;
        }
        if (st != last_status) { dirty = true; last_status = st; }
        if (a->now / 530 != last_blink) { last_blink = a->now / 530; dirty = true; }
        if (a->animating || a->sq_drill || a->online == OL_SEEKING || a->online == OL_PLAYING ||
            (a->peek_until && a->now < a->peek_until + 50)) dirty = true;
        if (!dirty) continue;
        dirty = false;

        int pw, ph, ww, wh;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        SDL_GetWindowSize(win, &ww, &wh);
        if (pw != tw || ph != th || !tex) {
            if (tex) SDL_DestroyTexture(tex);
            if (surf) cairo_surface_destroy(surf);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pw, ph);
            surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
            tw = pw;
            th = ph;
        }
        double scale = ww > 0 ? (double)pw / ww : 1;
        cairo_t *cr = cairo_create(surf);
        cairo_scale(cr, scale, scale);
        render_frame(a, cr, pw / scale, ph / scale, scale);
        cairo_destroy(cr);
        cairo_surface_flush(surf);
        SDL_UpdateTexture(tex, NULL, cairo_image_surface_get_data(surf), cairo_image_surface_get_stride(surf));
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }

    ol_close_all(a);
    opp_destroy(a->opp);
    if (tex) SDL_DestroyTexture(tex);
    if (surf) cairo_surface_destroy(surf);
    render_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
