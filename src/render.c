/* render.c - draws the board and side panel with cairo and pango. Pieces
 * are SVGs rendered once per size with librsvg and then cached. */
#include "app.h"

#include <librsvg/rsvg.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

typedef struct { double r, g, b, a; } Rgba;
#define HEX(h, al) ((Rgba){((h) >> 16 & 255) / 255.0, ((h) >> 8 & 255) / 255.0, ((h) & 255) / 255.0, (al)})

static const Rgba C_BG_TOP = HEX(0x24221f, 1), C_BG_BOT = HEX(0x161513, 1);
static const Rgba C_LIGHT = HEX(0xeed8b4, 1), C_DARK = HEX(0xb08560, 1);
static const Rgba C_FRAME = HEX(0x3b3026, 1);
static const Rgba C_LAST = HEX(0xf6d55c, 0.42);
static const Rgba C_CARD = HEX(0x2a2723, 1), C_CARD_EDGE = HEX(0x3a362f, 1);
static const Rgba C_TEXT = HEX(0xefe9df, 1), C_MUTED = HEX(0x9c9384, 1);
static const Rgba C_ACCENT = HEX(0xe3b04b, 1), C_ERROR = HEX(0xf07a68, 1), C_GOOD = HEX(0x93c58a, 1);
static const Rgba C_INPUT = HEX(0x1c1b18, 1);

static RsvgHandle *svg[2][7];
static cairo_surface_t *cache[2][7];
static int cache_px;

static void set(cairo_t *cr, Rgba c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

bool render_init(const char *base_dir) {
    static const char *names = "PNBRQK";
    bool ok = true;
    for (int c = 0; c < 2; c++)
        for (int t = PAWN; t <= KING; t++) {
            char path[1200];
            snprintf(path, sizeof path, "%sassets/pieces/cburnett/%c%c.svg", base_dir, c ? 'b' : 'w', names[t - 1]);
            GError *err = NULL;
            svg[c][t] = rsvg_handle_new_from_file(path, &err);
            if (!svg[c][t]) { fprintf(stderr, "can't load %s: %s\n", path, err ? err->message : "?"); ok = false; }
            if (err) g_error_free(err);
        }
    return ok;
}

void render_shutdown(void) {
    for (int c = 0; c < 2; c++)
        for (int t = 0; t < 7; t++) {
            if (svg[c][t]) g_object_unref(svg[c][t]);
            if (cache[c][t]) cairo_surface_destroy(cache[c][t]);
        }
}

static cairo_surface_t *piece_surface(int color, int type, int px) {
    if (px != cache_px) {
        for (int c = 0; c < 2; c++)
            for (int t = 0; t < 7; t++)
                if (cache[c][t]) { cairo_surface_destroy(cache[c][t]); cache[c][t] = NULL; }
        cache_px = px;
    }
    if (!cache[color][type] && svg[color][type]) {
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        cairo_t *cr = cairo_create(s);
        RsvgRectangle vp = {0, 0, px, px};
        rsvg_handle_render_document(svg[color][type], cr, &vp, NULL);
        cairo_destroy(cr);
        cache[color][type] = s;
    }
    return cache[color][type];
}

/* Draws a piece in a box of `size` logical units at x,y. */
static void draw_piece(cairo_t *cr, uint8_t pc, double x, double y, double size, double scale, double alpha) {
    int px = (int)lround(size * scale);
    cairo_surface_t *s = piece_surface(PCOLOR(pc), PTYPE(pc), px);
    if (!s) return;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, size / px, size / px);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

enum { LEFT, RIGHT, CENTER };

/* Draws text; returns its width. y is the top of the text. */
static double text(cairo_t *cr, const char *s, const char *font, double x, double y, Rgba c, int align, double max_w) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_text(l, s, -1);
    if (max_w > 0) {
        pango_layout_set_width(l, (int)(max_w * PANGO_SCALE));
        pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    }
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    double dx = align == RIGHT ? -w : align == CENTER ? -w / 2.0 : 0;
    set(cr, c);
    cairo_move_to(cr, x + dx, y);
    pango_cairo_show_layout(cr, l);
    pango_font_description_free(fd);
    g_object_unref(l);
    return w;
}

static double text_height(cairo_t *cr, const char *s, const char *font, double max_w) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_text(l, s, -1);
    if (max_w > 0) { pango_layout_set_width(l, (int)(max_w * PANGO_SCALE)); pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR); }
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    pango_font_description_free(fd);
    g_object_unref(l);
    return h;
}

static double ease(double t) { return t < 0 ? 0 : t > 1 ? 1 : 1 - pow(1 - t, 3); }

/* screen position of a square's top-left corner */
static void sq_xy(const App *a, int sq, double bx, double by, double s, double *x, double *y) {
    int f = FILE_OF(sq), r = RANK_OF(sq);
    if (a->flipped) { f = 7 - f; r = 7 - r; }
    *x = bx + f * s;
    *y = by + (7 - r) * s;
}

static void draw_board(App *a, cairo_t *cr, double bx, double by, double size, double scale) {
    double s = size / 8;
    const Position *p = &a->game.pos;

    /* frame with a soft shadow */
    for (int i = 6; i >= 1; i--) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.05);
        rounded(cr, bx - 10 - i, by - 10 - i + 4, size + 20 + 2 * i, size + 20 + 2 * i, 12 + i);
        cairo_fill(cr);
    }
    set(cr, C_FRAME);
    rounded(cr, bx - 10, by - 10, size + 20, size + 20, 12);
    cairo_fill(cr);

    for (int sq = 0; sq < 64; sq++) {
        double x, y;
        sq_xy(a, sq, bx, by, s, &x, &y);
        bool light = (FILE_OF(sq) + RANK_OF(sq)) & 1;
        set(cr, light ? C_LIGHT : C_DARK);
        cairo_rectangle(cr, x, y, s, s);
        cairo_fill(cr);
    }
    /* gentle sheen across the whole board */
    cairo_pattern_t *sheen = cairo_pattern_create_linear(bx, by, bx + size, by + size);
    cairo_pattern_add_color_stop_rgba(sheen, 0, 1, 1, 1, 0.07);
    cairo_pattern_add_color_stop_rgba(sheen, 1, 0, 0, 0, 0.08);
    cairo_set_source(cr, sheen);
    cairo_rectangle(cr, bx, by, size, size);
    cairo_fill(cr);
    cairo_pattern_destroy(sheen);

    if (a->sq_drill) {
        double x, y;
        sq_xy(a, a->sq_target, bx, by, s, &x, &y);
        bool wrong = a->sq_flash && a->now - a->sq_flash < 350;
        if (wrong) set(cr, HEX(0xf07a68, 0.75));
        else set(cr, HEX(0xe3b04b, 0.55));
        cairo_rectangle(cr, x, y, s, s);
        cairo_fill(cr);
        set(cr, wrong ? C_ERROR : HEX(0xfff1c9, 0.95));
        cairo_set_line_width(cr, s * 0.06);
        cairo_rectangle(cr, x + s * 0.03, y + s * 0.03, s * 0.94, s * 0.94);
        cairo_stroke(cr);
        return; /* no pieces, no labels: just the square */
    }

    /* last move */
    if (a->game.nply > 0) {
        Move m = a->game.moves[a->game.nply - 1];
        int sqs[2] = {m.from, m.to};
        for (int i = 0; i < 2; i++) {
            double x, y;
            sq_xy(a, sqs[i], bx, by, s, &x, &y);
            set(cr, C_LAST);
            cairo_rectangle(cr, x, y, s, s);
            cairo_fill(cr);
        }
    }
    /* king in check */
    if (in_check(p)) {
        double x, y;
        sq_xy(a, p->king[p->side], bx, by, s, &x, &y);
        cairo_pattern_t *g = cairo_pattern_create_radial(x + s / 2, y + s / 2, 0, x + s / 2, y + s / 2, s * 0.72);
        cairo_pattern_add_color_stop_rgba(g, 0, 1, 0.15, 0.1, 0.95);
        cairo_pattern_add_color_stop_rgba(g, 0.45, 0.9, 0.1, 0.05, 0.55);
        cairo_pattern_add_color_stop_rgba(g, 1, 0.6, 0, 0, 0);
        cairo_set_source(cr, g);
        cairo_rectangle(cr, x, y, s, s);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    /* coordinates, tucked into the edge squares */
    char font[64];
    snprintf(font, sizeof font, "DejaVu Sans Bold %.1fpx", s * 0.17);
    for (int i = 0; i < 8 && a->coords; i++) {
        int f = a->flipped ? 7 - i : i;
        int r = a->flipped ? i : 7 - i;
        char lab[2] = {(char)('a' + f), 0};
        bool light_bottom = (f + (a->flipped ? 7 : 0)) & 1;
        text(cr, lab, font, bx + i * s + s - s * 0.06, by + size - s * 0.25, light_bottom ? C_DARK : C_LIGHT, RIGHT, 0);
        char num[2] = {(char)('1' + r), 0};
        bool light_left = ((a->flipped ? 7 : 0) + r) & 1;
        text(cr, num, font, bx + s * 0.06, by + i * s + s * 0.04, light_left ? C_DARK : C_LIGHT, LEFT, 0);
    }

    /* pieces */
    bool hide = a->blind && a->now >= a->peek_until;
    if (a->scan) hide = false;
    if (hide) {
        snprintf(font, sizeof font, "DejaVu Serif Italic %.1fpx", s * 0.28);
        cairo_set_source_rgba(cr, 0.2, 0.15, 0.1, 0.35);
        text(cr, "blindfold", font, bx + size / 2, by + size / 2 - s * 0.2, (Rgba){0.25, 0.18, 0.12, 0.45}, CENTER, 0);
        return;
    }
    double t = a->animating ? ease((double)(a->now - a->anim_start) / ANIM_MS) : 1;
    Move am = a->anim_move;
    for (int sq = 0; sq < 64; sq++) {
        uint8_t pc = p->sq[sq];
        if (!pc || (a->animating && sq == am.to)) continue;
        double x, y;
        sq_xy(a, sq, bx, by, s, &x, &y);
        draw_piece(cr, pc, x, y, s, scale, 1);
    }
    if (a->animating) {
        double fx, fy, tx, ty;
        sq_xy(a, am.from, bx, by, s, &fx, &fy);
        sq_xy(a, am.to, bx, by, s, &tx, &ty);
        uint8_t pc = p->sq[am.to];
        if (pc) draw_piece(cr, pc, fx + (tx - fx) * t, fy + (ty - fy) * t, s, scale, 1);
    }
}

static void arrow(App *a, cairo_t *cr, Move m, double bx, double by, double s, Rgba c) {
    double fx, fy, tx, ty;
    sq_xy(a, m.from, bx, by, s, &fx, &fy);
    sq_xy(a, m.to, bx, by, s, &tx, &ty);
    fx += s / 2; fy += s / 2; tx += s / 2; ty += s / 2;
    double ang = atan2(ty - fy, tx - fx), len = hypot(tx - fx, ty - fy);
    double head = s * 0.38, w = s * 0.13;
    cairo_save(cr);
    cairo_translate(cr, fx, fy);
    cairo_rotate(cr, ang);
    cairo_move_to(cr, s * 0.18, -w / 2);
    cairo_line_to(cr, len - head, -w / 2);
    cairo_line_to(cr, len - head, -head * 0.55);
    cairo_line_to(cr, len, 0);
    cairo_line_to(cr, len - head, head * 0.55);
    cairo_line_to(cr, len - head, w / 2);
    cairo_line_to(cr, s * 0.18, w / 2);
    cairo_close_path(cr);
    set(cr, c);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_scan_arrows(App *a, cairo_t *cr, double bx, double by, double size) {
    if (!a->scan) return;
    double s = size / 8;
    /* found first, so a missed arrow on the same line stays visible */
    for (int i = 0; i < a->scan_n; i++)
        if (a->scan_found[i]) arrow(a, cr, a->scan_moves[i], bx, by, s, HEX(0x5fb05a, 0.72));
    if (a->scan == SC_RESULT)
        for (int i = 0; i < a->scan_n; i++)
            if (!a->scan_found[i]) arrow(a, cr, a->scan_moves[i], bx, by, s, HEX(0xe05545, 0.85));
}

/* the material this side is up, as small pieces plus a +N (works for any
 * start position, unlike counting captures) */
static void draw_player(App *a, cairo_t *cr, int color, const char *name, double x, double y, double size, double scale) {
    static const int val[7] = {0, 1, 3, 3, 5, 9, 0};
    int count[2][7] = {{0}};
    for (int s = 0; s < 64; s++) {
        uint8_t pc = a->game.pos.sq[s];
        if (pc) count[PCOLOR(pc)][PTYPE(pc)]++;
    }
    int mat[2] = {0, 0};
    for (int c = 0; c < 2; c++)
        for (int t = PAWN; t <= QUEEN; t++) mat[c] += count[c][t] * val[t];

    bool to_move = a->game.pos.side == color && game_status(&a->game) == ST_ONGOING;
    cairo_arc(cr, x + 6, y + 13, 5, 0, 2 * M_PI);
    if (to_move) set(cr, C_ACCENT);
    else cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_fill(cr);
    double w = text(cr, name, "DejaVu Sans Bold 15px", x + 20, y + 4, to_move ? C_TEXT : C_MUTED, LEFT, 0);
    double cx = x + 20 + w + 14;
    double ps = size / 8 * 0.36;
    int them = !color;
    if (a->blind) return;
    for (int t = QUEEN; t >= PAWN; t--) {
        int missing = count[color][t] - count[them][t];
        for (int i = 0; i < missing && i < 8; i++) {
            draw_piece(cr, PIECE(them, t), cx, y + 13 - ps / 2, ps, scale, 0.9);
            cx += ps * 0.55;
        }
        if (missing > 0) cx += ps * 0.4;
    }
    int diff = mat[color] - mat[them];
    if (diff > 0) {
        char b[16];
        snprintf(b, sizeof b, "+%d", diff);
        text(cr, b, "DejaVu Sans 13px", cx + 4, y + 6, C_MUTED, LEFT, 0);
    }
}

static void draw_panel(App *a, cairo_t *cr, double x, double y, double w, double h) {
    /* card */
    set(cr, C_CARD);
    rounded(cr, x, y, w, h, 14);
    cairo_fill_preserve(cr);
    set(cr, C_CARD_EDGE);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    double pad = 22, cx = x + pad, cw = w - 2 * pad;
    double cy = y + pad;
    text(cr, "Notation Trainer", "DejaVu Serif Bold 24px", cx, cy, C_TEXT, LEFT, 0);
    cy += 34;
    char opp[96], line[200];
    opp_name(a->opp, a->level, opp, sizeof opp);
    snprintf(line, sizeof line, "You play %s against %s%s%s", a->human == WHITE ? "White" : "Black", opp,
             a->blind ? "  ·  blindfold" : "", a->strict ? "  ·  strict" : "");
    if (a->banner[0]) {
        /* drill: first line is the label, the rest the prompt */
        char head[160];
        const char *nl = strchr(a->banner, '\n');
        size_t hl = nl ? (size_t)(nl - a->banner) : strlen(a->banner);
        snprintf(head, sizeof head, "%.*s", (int)(hl < sizeof head - 1 ? hl : sizeof head - 1), a->banner);
        text(cr, head, "DejaVu Sans Bold 13px", cx, cy, C_ACCENT, LEFT, cw);
        cy += text_height(cr, head, "DejaVu Sans Bold 13px", cw) + 6;
        if (nl) {
            text(cr, nl + 1, "DejaVu Sans 15px", cx, cy, C_TEXT, LEFT, cw);
            cy += text_height(cr, nl + 1, "DejaVu Sans 15px", cw);
        }
    } else {
        text(cr, line, "DejaVu Sans 13px", cx, cy, C_MUTED, LEFT, cw);
        cy += text_height(cr, line, "DejaVu Sans 13px", cw);
    }
    cy += 14;
    cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
    cairo_rectangle(cr, cx, cy, cw, 1);
    cairo_fill(cr);
    cy += 14;

    /* bottom-up: footer help, input, message, status. Moves fill the rest. */
    double by = y + h - pad;
    const char *help = "/new  /flip  /undo  /level 1-10  /resign  /help\n/blind  /peek  /coords  /squares  /strict  /pgn  /fen";
    double hh = text_height(cr, help, "DejaVu Sans Mono 11px", cw);
    by -= hh;
    text(cr, help, "DejaVu Sans Mono 11px", cx, by, C_MUTED, LEFT, cw);
    by -= 14;

    double ih = 52;
    by -= ih;
    set(cr, C_INPUT);
    rounded(cr, cx, by, cw, ih, 10);
    cairo_fill_preserve(cr);
    bool your_turn = a->game.pos.side == a->human && game_status(&a->game) == ST_ONGOING &&
                     (a->drill < 0 || a->dstate == DS_ASK || a->dstate == DS_PLAY);
    if (your_turn) set(cr, HEX(0xe3b04b, 0.65));
    else cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    double tx = cx + 16, ty = by + 13;
    if (a->input_len == 0) {
        const char *ph = a->scan == SC_ASK ? "e.g. Nxe5   ·   empty Enter = done" :
                         a->scan == SC_RESULT ? "Enter for the next" :
                         a->sq_drill ? "Type the square, e.g. e4" : your_turn ? "Your move - e.g. Nf3, exd5, O-O" : "Type a /command";
        text(cr, ph, "DejaVu Sans 17px", tx + 6, ty + 2, HEX(0x6a6358, 1), LEFT, 0);
        if ((a->now / 530) % 2 == 0) { set(cr, C_ACCENT); cairo_rectangle(cr, tx, ty + 2, 2, 22); cairo_fill(cr); }
    } else {
        double tw = text(cr, a->input, "DejaVu Sans Mono 19px", tx, ty, C_TEXT, LEFT, 0);
        if ((a->now / 530) % 2 == 0) { set(cr, C_ACCENT); cairo_rectangle(cr, tx + tw + 2, ty + 2, 2, 22); cairo_fill(cr); }
    }
    by -= 12;

    if (a->msg[0]) {
        Rgba mc = a->msg_kind == MSG_ERROR ? C_ERROR : a->msg_kind == MSG_NOTE ? C_ACCENT
                  : a->msg_kind == MSG_GOOD ? C_GOOD : C_MUTED;
        double mh = text_height(cr, a->msg, "DejaVu Sans 14px", cw);
        by -= mh;
        text(cr, a->msg, "DejaVu Sans 14px", cx, by, mc, LEFT, cw);
        by -= 10;
    }

    GameStatus st = game_status(&a->game);
    char status[160];
    if (a->sq_drill) {
        uint64_t left = a->sq_end > a->now ? a->sq_end - a->now : 0;
        snprintf(status, sizeof status, "Name the square  ·  %d right  ·  %llu s left", a->sq_score,
                 (unsigned long long)((left + 999) / 1000));
    } else if (a->scan == SC_ASK) {
        int k = 0;
        for (int i = 0; i < a->scan_n; i++) k += a->scan_found[i];
        snprintf(status, sizeof status, "%s  ·  %d found  ·  %llu s",
                 a->scan_phase ? "Their moves" : "Your moves", k,
                 (unsigned long long)((a->now - a->scan_start) / 1000));
    } else if (a->scan == SC_RESULT) {
        snprintf(status, sizeof status, "Green found  ·  red missed");
    } else if (a->drill >= 0 && a->dstate == DS_DONE) {
        snprintf(status, sizeof status, "Press Enter for the next drill");
    } else if (a->drill >= 0 && a->dstate == DS_AUTO) {
        snprintf(status, sizeof status, a->dshow ? "Showing the answer…" : "Opponent replies…");
    } else if (a->drill >= 0 && a->dstate == DS_ASK) {
        snprintf(status, sizeof status, "Your move  ·  %s", a->game.pos.side == WHITE ? "White" : "Black");
    } else if (st != ST_ONGOING) {
        const char *res = game_result(&a->game);
        snprintf(status, sizeof status, "%s  ·  %s", status_text(st), res);
    } else if (opp_busy(a->opp)) {
        snprintf(status, sizeof status, "%s is thinking…", opp_is_stockfish(a->opp) ? "Stockfish" : "Engine");
    } else {
        snprintf(status, sizeof status, "%s to move%s", a->game.pos.side == WHITE ? "White" : "Black",
                 in_check(&a->game.pos) ? "  ·  check!" : "");
    }
    by -= 22;
    text(cr, status, "DejaVu Sans Bold 16px", cx, by, st != ST_ONGOING ? C_ACCENT : C_TEXT, LEFT, 0);
    by -= 14;

    /* move list: rows of number, white, black; newest at the bottom */
    double row = 27, top = cy, bottom = by;
    int nrows_fit = (int)((bottom - top) / row);
    const Game *g = &a->game;
    bool black_first = g->start.side == BLACK;
    int total_cells = g->nply + (black_first ? 1 : 0);
    int nrows = g->nply ? (total_cells + 1) / 2 : 0;
    int first = nrows > nrows_fit ? nrows - nrows_fit : 0;
    if (g->nply == 0) {
        text(cr, a->blind ? "Moves appear here. The pieces don't." : "Moves appear here.",
             "DejaVu Sans Italic 14px", cx, top + 4, HEX(0x6a6358, 1), LEFT, 0);
    }
    double colw = (cw - 52) / 2;
    for (int r = first; r < nrows; r++) {
        double ry = top + (r - first) * row;
        if (r % 2 == 0) {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.025);
            cairo_rectangle(cr, cx - 6, ry, cw + 12, row);
            cairo_fill(cr);
        }
        char num[16];
        snprintf(num, sizeof num, "%d.", g->start.fullmove + r);
        text(cr, num, "DejaVu Sans Mono 15px", cx + 36, ry + 4, C_MUTED, RIGHT, 0);
        for (int c = 0; c < 2; c++) {
            int ply = r * 2 + c - (black_first ? 1 : 0);
            if (ply < 0 || ply >= g->nply) continue;
            double mx = cx + 52 + c * colw;
            bool last = ply == g->nply - 1;
            if (last) {
                set(cr, HEX(0xe3b04b, 0.18));
                rounded(cr, mx - 6, ry + 2, colw - 10, row - 4, 6);
                cairo_fill(cr);
            }
            text(cr, g->san[ply], "DejaVu Sans Mono Bold 15px", mx, ry + 4, last ? C_ACCENT : C_TEXT, LEFT, 0);
        }
    }
}

void render_frame(App *a, cairo_t *cr, double w, double h, double scale) {
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgb(bg, 0, C_BG_TOP.r, C_BG_TOP.g, C_BG_TOP.b);
    cairo_pattern_add_color_stop_rgb(bg, 1, C_BG_BOT.r, C_BG_BOT.g, C_BG_BOT.b);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    double m = 28, label = 34, panel_min = 380;
    double size = fmin(h - 2 * m - 2 * label - 20, w - panel_min - 3 * m - 20);
    if (size < 200) size = 200;
    size = floor(size / 8) * 8;
    double bx = m + 10, by = (h - size) / 2;

    char opp[96];
    opp_name(a->opp, a->level, opp, sizeof opp);
    if (a->drill >= 0) snprintf(opp, sizeof opp, "%s", a->dstate == DS_PLAY ? "Stockfish (full strength)" : "Opponent");
    if (a->scan) snprintf(opp, sizeof opp, "Opponent");
    int top_color = a->flipped ? WHITE : BLACK;
    const char *top_name = top_color == a->human ? "You" : opp;
    const char *bot_name = top_color == a->human ? opp : "You";
    draw_player(a, cr, top_color, top_name, bx - 10, by - 10 - label, size, scale);
    draw_player(a, cr, !top_color, bot_name, bx - 10, by + size + 16, size, scale);

    draw_board(a, cr, bx, by, size, scale);
    draw_scan_arrows(a, cr, bx, by, size);

    double px = bx + size + 10 + m, pw = w - px - m;
    draw_panel(a, cr, px, m, pw, h - 2 * m);
}
