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

static void after_move(App *a, Move m) {
    a->animating = true;
    a->anim_move = m;
    a->anim_start = a->now;
    GameStatus st = game_status(&a->game);
    if (st != ST_ONGOING && !a->saved) {
        char path[1200];
        a->saved = save_pgn(a, path, sizeof path);
        bool won = (strcmp(game_result(&a->game), "1-0") == 0) == (a->human == WHITE) &&
                   strcmp(game_result(&a->game), "1/2-1/2") != 0;
        say(a, won ? MSG_GOOD : MSG_NOTE, "%s. %s Saved to games/. /new for another.",
            status_text(st), won ? "Well played!" : "");
    }
}

static const char *HELP =
    "Moves: standard notation - e4, Nf3, exd5, Nbd7, R1e2, O-O, e8=Q. Check signs optional "
    "(required in /strict). Up/Down recall earlier input.  "
    "/new [white|black] · /flip · /undo · /level 1-10 · /blind (hide pieces) · /peek (3 s look) · "
    "/coords (board labels) · /squares (30 s square-naming drill) · "
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
        int side = !strcmp(arg, "black") || !strcmp(arg, "b") ? BLACK
                   : !strcmp(arg, "white") || !strcmp(arg, "w") ? WHITE : a->human;
        new_game(a, NULL, side);
        say(a, MSG_INFO, "New game - you have %s.", side == WHITE ? "White" : "Black");
    } else if (!strcmp(cmd, "flip")) {
        a->flipped = !a->flipped;
        a->msg[0] = 0;
    } else if (!strcmp(cmd, "undo") || !strcmp(cmd, "u")) {
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
    if (!line[0]) return true;
    if (a->nhist == 0 || strcmp(a->history[(a->nhist - 1) % HISTORY], line) != 0)
        snprintf(a->history[a->nhist++ % HISTORY], 64, "%s", line);
    a->hist_pos = a->nhist;

    if (line[0] == '/') return command(a, line);
    if (game_status(&a->game) != ST_ONGOING) { say(a, MSG_ERROR, "The game is over - /new to start another."); return true; }
    if (a->game.pos.side != a->human) { say(a, MSG_ERROR, "Not your move yet."); return true; }

    Move m;
    char why[256];
    SanResult r = san_parse(&a->game.pos, line, a->strict, &m, why, sizeof why);
    if (r == SAN_ERR) { say(a, MSG_ERROR, "%s - %s", line, why); return true; }
    if (r == SAN_OK_NOTE) say(a, MSG_NOTE, "%s", why);
    else a->msg[0] = 0;
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
    srand((unsigned)time(NULL));
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
        bool busy = a->animating || opp_busy(a->opp) || a->now < a->peek_until || a->sq_drill;
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
        if (st == ST_ONGOING && a->game.pos.side != a->human && !opp_busy(a->opp) && !a->animating) {
            opp_go(a->opp, &a->game, a->level);
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
