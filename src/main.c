/* main.c - window, keyboard and game flow. Moves are typed in standard
 * notation; lines starting with '/' are commands. */
#include <SDL3/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"

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
        say(a, won ? MSG_GOOD : MSG_NOTE, "%s. %s Saved to games/. /new for another.",
            status_text(st), won ? "Well played!" : "");
    }
}

/* ------------------------------------------------------------ drills */

static Drill *cur_drill(App *a) { return a->drill >= 0 ? &a->drills.d[a->drill] : NULL; }

static bool solution_move(App *a, int step, Move *m) {
    char why[256];
    Drill *d = cur_drill(a);
    return step < d->nsol && san_parse(&a->game.pos, d->sol[step], false, m, why, sizeof why) != SAN_ERR;
}

static void drill_record(App *a, bool ok) {
    if (a->drecorded) return;
    a->drecorded = true;
    drills_record(&a->drills, a->drill, ok);
    a->dsess_n++;
    if (ok) a->dsess_ok++;
}

static void drill_finish(App *a, bool ok, const char *extra) {
    Drill *d = cur_drill(a);
    drill_record(a, ok);
    a->dstate = DS_DONE;
    say(a, ok ? MSG_GOOD : MSG_ERROR, "%s%s%s  ·  Enter for the next drill.",
        ok ? "Correct! " : "Missed. ", extra ? extra : "", d->info[0] ? d->info : "");
}

static void drill_end_session(App *a) {
    int n = a->dsess_n, ok = a->dsess_ok;
    a->drill = -1;
    a->banner[0] = 0;
    new_game(a, NULL, a->human);
    say(a, MSG_GOOD, "Drills finished: %d of %d right first time. Back to free play.", ok, n);
}

static void drill_next(App *a) {
    int idx = drills_next(&a->drills);
    if (idx < 0) { drill_end_session(a); return; }
    a->drill = idx;
    Drill *d = &a->drills.d[idx];
    Position p;
    pos_from_fen(&p, d->fen);
    int side = p.side;
    if (!strcmp(d->kind, "line")) side = (d->nsol - 1) % 2 == 0 ? WHITE : BLACK;
    new_game(a, d->fen, side);
    a->saved = true; /* drills aren't saved as games */
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

static void drill_session(App *a, const char *kind) {
    char err[300];
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
    drill_record(a, false);
    char line[300] = "";
    for (int i = 0; i < d->nsol; i++) {
        strncat(line, d->sol[i], sizeof line - strlen(line) - 2);
        strncat(line, " ", sizeof line - strlen(line) - 1);
    }
    new_game(a, d->fen, a->human);
    a->saved = true;
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
    scan_next_position(a);
}

static void scan_start_session(App *a, const char *arg) {
    if (!scan_load(a)) { say(a, MSG_ERROR, "No scan positions: run analysis/report.py to make data/analysis/drills/scan.tsv."); return; }
    opp_cancel(a->opp);
    a->drill = -1;
    a->blind = false;
    a->scan_mode = !strncmp(arg, "cap", 3) ? 1 : !strncmp(arg, "che", 3) ? 2 : 0;
    a->scan_sess_found = a->scan_sess_total = a->scan_sess_phases = 0;
    a->scan_sess_secs = 0;
    scan_next_position(a);
}

static void scan_stop(App *a) {
    a->scan = SC_OFF;
    a->banner[0] = 0;
    new_game(a, NULL, a->human);
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

static const char *HELP =
    "Moves: standard notation - e4, Nf3, exd5, Nbd7, R1e2, O-O, e8=Q. Check signs optional "
    "(required in /strict). Up/Down recall earlier input.  "
    "/new [white|black] · /flip · /undo · /level 1-10 · /blind (hide pieces) · /peek (3 s look) · "
    "/coords (board labels) · /squares (30 s square-naming drill) · "
    "/drill [puzzles|punish|lines|endgames] · /answer · /next · /play (leave drills) · "
    "/scan [captures|checks] (list every check and capture) · "
    "/strict · /pgn (save) · /fen (copy) · /resign · /quit";

static void next_square(App *a) {
    int t;
    do t = rand() % 64; while (t == a->sq_target);
    a->sq_target = t;
}

static void end_square_drill(App *a, bool finished) {
    a->sq_drill = false;
    if (!finished) { say(a, MSG_INFO, "Square drill stopped."); return; }
    bool record = a->sq_score > a->sq_best;
    if (record) a->sq_best = a->sq_score;
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
    if (!strcmp(cmd, "new") || !strcmp(cmd, "n")) {
        a->drill = -1;
        a->banner[0] = 0;
        a->scan = SC_OFF;
        int side = !strcmp(arg, "black") || !strcmp(arg, "b") ? BLACK
                   : !strcmp(arg, "white") || !strcmp(arg, "w") ? WHITE : a->human;
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
    } else if (!strcmp(cmd, "scan") || !strcmp(cmd, "cc")) {
        scan_start_session(a, arg);
    } else if (!strcmp(cmd, "done")) {
        if (a->scan == SC_ASK) scan_finish_phase(a);
    } else if (!strcmp(cmd, "drill") || !strcmp(cmd, "d")) {
        if (a->scan) scan_stop(a);
        drill_session(a, arg[0] ? arg : NULL);
    } else if (!strcmp(cmd, "next")) {
        if (a->drill < 0) { say(a, MSG_ERROR, "Not in a drill. /drill starts one."); return true; }
        drill_next(a);
    } else if (!strcmp(cmd, "answer") || !strcmp(cmd, "giveup")) {
        Drill *d = cur_drill(a);
        if (!d || a->dstate == DS_DONE) { say(a, MSG_ERROR, "No drill to give up."); return true; }
        if (!strcmp(d->kind, "endgame")) { opp_cancel(a->opp); drill_finish(a, false, "Given up. "); }
        else drill_show(a, NULL);
    } else if (!strcmp(cmd, "play") && a->scan) {
        scan_stop(a);
    } else if (!strcmp(cmd, "play")) {
        if (a->drill >= 0) { opp_cancel(a->opp); a->drill = -1; a->banner[0] = 0; new_game(a, NULL, a->human); }
        say(a, MSG_INFO, "Free play.");
    } else if (!strcmp(cmd, "coords")) {
        a->coords = !a->coords;
        say(a, MSG_INFO, a->coords ? "Coordinates on." : "Coordinates off. /squares drills naming squares.");
    } else if (!strcmp(cmd, "squares") || !strcmp(cmd, "sq")) {
        a->sq_drill = true;
        a->sq_score = a->sq_misses = 0;
        a->sq_end = a->now + SQ_DRILL_MS;
        a->sq_flash = 0;
        next_square(a);
        say(a, MSG_INFO, "Name each highlighted square - just type it, no Enter. Esc stops.");
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
        if (a->scan == SC_ASK) scan_finish_phase(a);
        else if (a->scan == SC_RESULT) scan_continue(a);
        else if (a->drill >= 0 && a->dstate == DS_DONE) drill_next(a);
        return true;
    }
    if (a->nhist == 0 || strcmp(a->history[(a->nhist - 1) % HISTORY], line) != 0)
        snprintf(a->history[a->nhist++ % HISTORY], 64, "%s", line);
    a->hist_pos = a->nhist;

    if (line[0] == '/') return command(a, line);
    if (a->scan == SC_ASK) { scan_input(a, line); return true; }
    if (a->scan == SC_RESULT) { say(a, MSG_INFO, "Press Enter for the next one, or /play to stop."); return true; }
    if (a->drill >= 0 && a->dstate == DS_DONE) { say(a, MSG_INFO, "Press Enter for the next drill."); return true; }
    if (a->drill >= 0 && a->dstate == DS_AUTO) { say(a, MSG_INFO, "Wait a moment..."); return true; }
    if (game_status(&a->game) != ST_ONGOING) { say(a, MSG_ERROR, "The game is over - /new to start another."); return true; }
    if (a->game.pos.side != a->human) { say(a, MSG_ERROR, "Not your move yet."); return true; }

    Move m;
    char why[256];
    SanResult r = san_parse(&a->game.pos, line, a->strict, &m, why, sizeof why);
    if (r == SAN_ERR) { say(a, MSG_ERROR, "%s - %s", line, why); return true; }
    if (r == SAN_OK_NOTE) say(a, MSG_NOTE, "%s", why);
    else a->msg[0] = 0;
    if (a->drill >= 0 && a->dstate == DS_ASK) { drill_user_move(a, m, line); return true; }
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
            "usage: chess [--black] [--level N] [--fen FEN] [--moves \"e4 e5 ...\"]\n"
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
    bool shot_blind = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--black")) a->human = BLACK;
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) a->level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fen") && i + 1 < argc) fen = argv[++i];
        else if (!strcmp(argv[i], "--moves") && i + 1 < argc) moves = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &shot_w, &shot_h);
        else if (!strcmp(argv[i], "--blind")) shot_blind = true;
        else if (!strcmp(argv[i], "--no-coords")) a->coords = false;
        else { usage(); return 2; }
    }
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
    say(a, MSG_INFO, "Opponent: %s. Type your move and press Enter. /help for commands.", opp_line);

    while (running) {
        a->now = SDL_GetTicks();
        bool busy = a->animating || opp_busy(a->opp) || a->now < a->peek_until || a->sq_drill ||
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
        bool engine_plays = (a->drill < 0 || a->dstate == DS_PLAY) && !a->scan;
        if (engine_plays && st == ST_ONGOING && a->game.pos.side != a->human && !opp_busy(a->opp) && !a->animating) {
            opp_go(a->opp, &a->game, a->drill >= 0 ? LEVEL_MAX : a->level);
            dirty = true;
        }
        if (st != last_status) { dirty = true; last_status = st; }
        if (a->now / 530 != last_blink) { last_blink = a->now / 530; dirty = true; }
        if (a->animating || a->sq_drill || (a->peek_until && a->now < a->peek_until + 50)) dirty = true;
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

    opp_destroy(a->opp);
    if (tex) SDL_DestroyTexture(tex);
    if (surf) cairo_surface_destroy(surf);
    render_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
