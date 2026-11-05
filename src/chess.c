/* chess.c - rules core. Mailbox board with copy-make: simple and easily
 * fast enough for play, notation and training. */
#include "chess.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int KN_DF[8] = {1, 2, 2, 1, -1, -2, -2, -1};
static const int KN_DR[8] = {2, 1, -1, -2, -2, -1, 1, 2};
static const int KG_DF[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static const int KG_DR[8] = {0, 1, 1, 1, 0, -1, -1, -1};
/* rook directions first, then bishop directions */
static const int DIR_DF[8] = {1, -1, 0, 0, 1, 1, -1, -1};
static const int DIR_DR[8] = {0, 0, 1, -1, 1, -1, 1, -1};

static const char PIECE_CH[] = " PNBRQK";

static inline bool on_board(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

void sq_name(int sq, char *out) {
    out[0] = (char)('a' + FILE_OF(sq));
    out[1] = (char)('1' + RANK_OF(sq));
    out[2] = 0;
}

bool move_eq(Move a, Move b) {
    return a.from == b.from && a.to == b.to && a.promo == b.promo;
}

/* ---------------------------------------------------------------- FEN */

bool pos_from_fen(Position *p, const char *fen) {
    memset(p, 0, sizeof *p);
    p->ep = -1;
    p->king[0] = p->king[1] = -1;
    int f = 0, r = 7;
    const char *s = fen;
    while (*s == ' ') s++;
    for (; *s && *s != ' '; s++) {
        char c = *s;
        if (c == '/') {
            if (f != 8) return false;
            f = 0;
            if (--r < 0) return false;
        } else if (c >= '1' && c <= '8') {
            f += c - '0';
            if (f > 8) return false;
        } else {
            const char *q = strchr(PIECE_CH, toupper((unsigned char)c));
            if (!q || q == PIECE_CH || f > 7) return false;
            int t = (int)(q - PIECE_CH);
            int col = isupper((unsigned char)c) ? WHITE : BLACK;
            p->sq[SQ(f, r)] = PIECE(col, t);
            if (t == KING) p->king[col] = SQ(f, r);
            f++;
        }
    }
    if (r != 0 || f != 8 || p->king[0] < 0 || p->king[1] < 0) return false;
    while (*s == ' ') s++;
    if (*s == 'w') p->side = WHITE;
    else if (*s == 'b') p->side = BLACK;
    else return false;
    s++;
    while (*s == ' ') s++;
    for (; *s && *s != ' '; s++) {
        switch (*s) {
        case 'K': p->castle |= CASTLE_WK; break;
        case 'Q': p->castle |= CASTLE_WQ; break;
        case 'k': p->castle |= CASTLE_BK; break;
        case 'q': p->castle |= CASTLE_BQ; break;
        case '-': break;
        default: return false;
        }
    }
    while (*s == ' ') s++;
    if (*s && *s != '-') {
        if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return false;
        p->ep = SQ(s[0] - 'a', s[1] - '1');
        s += 2;
    } else if (*s) {
        s++;
    }
    p->halfmove = 0;
    p->fullmove = 1;
    int hm, fm;
    int got = sscanf(s, "%d %d", &hm, &fm);
    if (got >= 1) p->halfmove = hm;
    if (got >= 2) p->fullmove = fm;
    /* Drop castling rights that the pieces cannot have. */
    if (p->sq[4] != PIECE(WHITE, KING)) p->castle &= ~(CASTLE_WK | CASTLE_WQ);
    if (p->sq[7] != PIECE(WHITE, ROOK)) p->castle &= ~CASTLE_WK;
    if (p->sq[0] != PIECE(WHITE, ROOK)) p->castle &= ~CASTLE_WQ;
    if (p->sq[60] != PIECE(BLACK, KING)) p->castle &= ~(CASTLE_BK | CASTLE_BQ);
    if (p->sq[63] != PIECE(BLACK, ROOK)) p->castle &= ~CASTLE_BK;
    if (p->sq[56] != PIECE(BLACK, ROOK)) p->castle &= ~CASTLE_BQ;
    return true;
}

void pos_to_fen(const Position *p, char *out, size_t n) {
    char buf[100];
    int k = 0;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            uint8_t pc = p->sq[SQ(f, r)];
            if (!pc) { empty++; continue; }
            if (empty) { buf[k++] = (char)('0' + empty); empty = 0; }
            char c = PIECE_CH[PTYPE(pc)];
            buf[k++] = PCOLOR(pc) == WHITE ? c : (char)tolower(c);
        }
        if (empty) buf[k++] = (char)('0' + empty);
        if (r) buf[k++] = '/';
    }
    buf[k++] = ' ';
    buf[k++] = p->side == WHITE ? 'w' : 'b';
    buf[k++] = ' ';
    if (!p->castle) buf[k++] = '-';
    if (p->castle & CASTLE_WK) buf[k++] = 'K';
    if (p->castle & CASTLE_WQ) buf[k++] = 'Q';
    if (p->castle & CASTLE_BK) buf[k++] = 'k';
    if (p->castle & CASTLE_BQ) buf[k++] = 'q';
    buf[k++] = ' ';
    if (p->ep >= 0) { sq_name(p->ep, buf + k); k += 2; }
    else buf[k++] = '-';
    buf[k] = 0;
    snprintf(out, n, "%s %d %d", buf, p->halfmove, p->fullmove);
}

/* ---------------------------------------------------------- attacks */

bool is_attacked(const Position *p, int sq, int by) {
    int f = FILE_OF(sq), r = RANK_OF(sq);
    /* pawns: a white pawn attacks upward, so look one rank below */
    int pr = by == WHITE ? r - 1 : r + 1;
    if (pr >= 0 && pr < 8) {
        if (f > 0 && p->sq[SQ(f - 1, pr)] == PIECE(by, PAWN)) return true;
        if (f < 7 && p->sq[SQ(f + 1, pr)] == PIECE(by, PAWN)) return true;
    }
    for (int i = 0; i < 8; i++) {
        int nf = f + KN_DF[i], nr = r + KN_DR[i];
        if (on_board(nf, nr) && p->sq[SQ(nf, nr)] == PIECE(by, KNIGHT)) return true;
        nf = f + KG_DF[i]; nr = r + KG_DR[i];
        if (on_board(nf, nr) && p->sq[SQ(nf, nr)] == PIECE(by, KING)) return true;
    }
    for (int d = 0; d < 8; d++) {
        int slider = d < 4 ? ROOK : BISHOP;
        int nf = f + DIR_DF[d], nr = r + DIR_DR[d];
        while (on_board(nf, nr)) {
            uint8_t pc = p->sq[SQ(nf, nr)];
            if (pc) {
                if (PCOLOR(pc) == by && (PTYPE(pc) == slider || PTYPE(pc) == QUEEN))
                    return true;
                break;
            }
            nf += DIR_DF[d]; nr += DIR_DR[d];
        }
    }
    return false;
}

bool in_check(const Position *p) { return is_attacked(p, p->king[p->side], !p->side); }

/* ------------------------------------------------------- generation */

static int add(Move *out, int n, int from, int to, int promo, int flags) {
    out[n].from = (uint8_t)from;
    out[n].to = (uint8_t)to;
    out[n].promo = (uint8_t)promo;
    out[n].flags = (uint8_t)flags;
    return n + 1;
}

static int add_pawn(Move *out, int n, int from, int to, int flags) {
    int r = RANK_OF(to);
    if (r == 0 || r == 7) {
        for (int t = QUEEN; t >= KNIGHT; t--) n = add(out, n, from, to, t, flags);
        return n;
    }
    return add(out, n, from, to, 0, flags);
}

int gen_pseudo(const Position *p, Move *out) {
    int n = 0, us = p->side, them = !us;
    for (int s = 0; s < 64; s++) {
        uint8_t pc = p->sq[s];
        if (!pc || PCOLOR(pc) != us) continue;
        int f = FILE_OF(s), r = RANK_OF(s), t = PTYPE(pc);
        if (t == PAWN) {
            int dr = us == WHITE ? 1 : -1, start = us == WHITE ? 1 : 6;
            int r1 = r + dr;
            if (r1 < 0 || r1 > 7) continue;
            if (!p->sq[SQ(f, r1)]) {
                n = add_pawn(out, n, s, SQ(f, r1), 0);
                if (r == start && !p->sq[SQ(f, r1 + dr)])
                    n = add(out, n, s, SQ(f, r1 + dr), 0, MF_DOUBLE);
            }
            for (int df = -1; df <= 1; df += 2) {
                int nf = f + df;
                if (nf < 0 || nf > 7) continue;
                int to = SQ(nf, r1);
                uint8_t tp = p->sq[to];
                if (tp && PCOLOR(tp) == them) n = add_pawn(out, n, s, to, MF_CAPTURE);
                else if (to == p->ep) n = add(out, n, s, to, 0, MF_CAPTURE | MF_EP);
            }
        } else if (t == KNIGHT || t == KING) {
            const int *df = t == KNIGHT ? KN_DF : KG_DF;
            const int *dr = t == KNIGHT ? KN_DR : KG_DR;
            for (int i = 0; i < 8; i++) {
                int nf = f + df[i], nr = r + dr[i];
                if (!on_board(nf, nr)) continue;
                uint8_t tp = p->sq[SQ(nf, nr)];
                if (!tp) n = add(out, n, s, SQ(nf, nr), 0, 0);
                else if (PCOLOR(tp) == them) n = add(out, n, s, SQ(nf, nr), 0, MF_CAPTURE);
            }
        } else {
            int d0 = t == BISHOP ? 4 : 0, d1 = t == ROOK ? 4 : 8;
            for (int d = d0; d < d1; d++) {
                int nf = f + DIR_DF[d], nr = r + DIR_DR[d];
                while (on_board(nf, nr)) {
                    uint8_t tp = p->sq[SQ(nf, nr)];
                    if (!tp) {
                        n = add(out, n, s, SQ(nf, nr), 0, 0);
                    } else {
                        if (PCOLOR(tp) == them) n = add(out, n, s, SQ(nf, nr), 0, MF_CAPTURE);
                        break;
                    }
                    nf += DIR_DF[d]; nr += DIR_DR[d];
                }
            }
        }
    }
    /* castling: squares between empty, king not in check and not
     * passing through or landing on an attacked square */
    int ks = p->king[us];
    if (us == WHITE ? (p->castle & (CASTLE_WK | CASTLE_WQ)) : (p->castle & (CASTLE_BK | CASTLE_BQ))) {
        int base = us == WHITE ? 0 : 56;
        int kf = us == WHITE ? CASTLE_WK : CASTLE_BK;
        int qf = us == WHITE ? CASTLE_WQ : CASTLE_BQ;
        if (ks == base + 4 && !is_attacked(p, ks, them)) {
            if ((p->castle & kf) && !p->sq[base + 5] && !p->sq[base + 6] &&
                !is_attacked(p, base + 5, them) && !is_attacked(p, base + 6, them))
                n = add(out, n, ks, base + 6, 0, MF_CASTLE);
            if ((p->castle & qf) && !p->sq[base + 3] && !p->sq[base + 2] && !p->sq[base + 1] &&
                !is_attacked(p, base + 3, them) && !is_attacked(p, base + 2, them))
                n = add(out, n, ks, base + 2, 0, MF_CASTLE);
        }
    }
    return n;
}

void make_move(Position *p, Move m) {
    int us = p->side;
    uint8_t pc = p->sq[m.from];
    int t = PTYPE(pc);
    bool capture = p->sq[m.to] != 0 || (m.flags & MF_EP);

    p->sq[m.to] = m.promo ? PIECE(us, m.promo) : pc;
    p->sq[m.from] = EMPTY;
    if (m.flags & MF_EP) p->sq[us == WHITE ? m.to - 8 : m.to + 8] = EMPTY;
    if (m.flags & MF_CASTLE) {
        if (FILE_OF(m.to) == 6) { p->sq[m.to - 1] = p->sq[m.to + 1]; p->sq[m.to + 1] = EMPTY; }
        else { p->sq[m.to + 1] = p->sq[m.to - 2]; p->sq[m.to - 2] = EMPTY; }
    }
    if (t == KING) p->king[us] = m.to;

    /* any move from or to a corner or king square removes those rights */
    static const int lose[64] = {
        [0] = CASTLE_WQ, [4] = CASTLE_WK | CASTLE_WQ, [7] = CASTLE_WK,
        [56] = CASTLE_BQ, [60] = CASTLE_BK | CASTLE_BQ, [63] = CASTLE_BK,
    };
    p->castle &= ~(lose[m.from] | lose[m.to]);

    p->ep = (m.flags & MF_DOUBLE) ? (m.from + m.to) / 2 : -1;
    p->halfmove = (t == PAWN || capture) ? 0 : p->halfmove + 1;
    if (us == BLACK) p->fullmove++;
    p->side = !us;
}

int gen_legal(const Position *p, Move *out) {
    Move tmp[MAX_MOVES];
    int n = gen_pseudo(p, tmp), k = 0;
    for (int i = 0; i < n; i++) {
        Position q = *p;
        make_move(&q, tmp[i]);
        if (!is_attacked(&q, q.king[p->side], q.side)) out[k++] = tmp[i];
    }
    return k;
}

uint64_t perft(const Position *p, int depth) {
    Move ms[MAX_MOVES];
    int n = gen_legal(p, ms);
    if (depth <= 1) return depth == 1 ? (uint64_t)n : 1;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        Position q = *p;
        make_move(&q, ms[i]);
        total += perft(&q, depth - 1);
    }
    return total;
}

/* ---------------------------------------------------------- hashing */

static uint64_t ZOB[16][64], ZOB_SIDE, ZOB_CASTLE[16], ZOB_EP[8];
static bool zob_ready;

static uint64_t splitmix(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void zob_init(void) {
    uint64_t s = 20261105;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 64; j++) ZOB[i][j] = splitmix(&s);
    ZOB_SIDE = splitmix(&s);
    for (int i = 0; i < 16; i++) ZOB_CASTLE[i] = splitmix(&s);
    for (int i = 0; i < 8; i++) ZOB_EP[i] = splitmix(&s);
    zob_ready = true;
}

uint64_t pos_hash(const Position *p) {
    if (!zob_ready) zob_init();
    uint64_t h = 0;
    for (int s = 0; s < 64; s++)
        if (p->sq[s]) h ^= ZOB[p->sq[s]][s];
    if (p->side) h ^= ZOB_SIDE;
    h ^= ZOB_CASTLE[p->castle];
    /* en passant only counts for repetition if a capture is possible */
    if (p->ep >= 0) {
        int f = FILE_OF(p->ep), r = p->side == WHITE ? 4 : 3;
        uint8_t pawn = PIECE(p->side, PAWN);
        if ((f > 0 && p->sq[SQ(f - 1, r)] == pawn) || (f < 7 && p->sq[SQ(f + 1, r)] == pawn))
            h ^= ZOB_EP[f];
    }
    return h;
}

/* --------------------------------------------------------- notation */

void move_to_uci(Move m, char *out) {
    sq_name(m.from, out);
    sq_name(m.to, out + 2);
    if (m.promo) { out[4] = (char)tolower(PIECE_CH[m.promo]); out[5] = 0; }
}

bool move_from_uci(const Position *p, const char *uci, Move *out) {
    Move ms[MAX_MOVES];
    int n = gen_legal(p, ms);
    for (int i = 0; i < n; i++) {
        char buf[8];
        move_to_uci(ms[i], buf);
        if (strcmp(buf, uci) == 0) { *out = ms[i]; return true; }
    }
    return false;
}

/* SAN without the check sign */
static void san_base(const Position *p, Move m, const Move *legal, int nlegal, char *out) {
    int k = 0;
    int t = PTYPE(p->sq[m.from]);
    if (m.flags & MF_CASTLE) {
        strcpy(out, FILE_OF(m.to) == 6 ? "O-O" : "O-O-O");
        return;
    }
    bool capture = (m.flags & MF_CAPTURE) != 0;
    if (t == PAWN) {
        if (capture) { out[k++] = (char)('a' + FILE_OF(m.from)); out[k++] = 'x'; }
    } else {
        out[k++] = PIECE_CH[t];
        bool other = false, same_file = false, same_rank = false;
        for (int i = 0; i < nlegal; i++) {
            Move o = legal[i];
            if (o.to != m.to || o.from == m.from || PTYPE(p->sq[o.from]) != t) continue;
            other = true;
            if (FILE_OF(o.from) == FILE_OF(m.from)) same_file = true;
            if (RANK_OF(o.from) == RANK_OF(m.from)) same_rank = true;
        }
        if (other) {
            if (!same_file) out[k++] = (char)('a' + FILE_OF(m.from));
            else if (!same_rank) out[k++] = (char)('1' + RANK_OF(m.from));
            else { out[k++] = (char)('a' + FILE_OF(m.from)); out[k++] = (char)('1' + RANK_OF(m.from)); }
        }
        if (capture) out[k++] = 'x';
    }
    sq_name(m.to, out + k);
    k += 2;
    if (m.promo) { out[k++] = '='; out[k++] = PIECE_CH[m.promo]; }
    out[k] = 0;
}

static const char *check_suffix(const Position *p, Move m) {
    Position q = *p;
    make_move(&q, m);
    if (!in_check(&q)) return "";
    Move ms[MAX_MOVES];
    return gen_legal(&q, ms) ? "+" : "#";
}

void move_to_san(const Position *p, Move m, char *out, size_t n) {
    Move legal[MAX_MOVES];
    int nl = gen_legal(p, legal);
    char base[16];
    san_base(p, m, legal, nl, base);
    snprintf(out, n, "%s%s", base, check_suffix(p, m));
}

static const char *piece_word(int t) {
    static const char *w[] = {"", "pawn", "knight", "bishop", "rook", "queen", "king"};
    return w[t];
}

/* What the player typed, broken into parts. */
typedef struct {
    bool castle_k, castle_q;
    int piece;              /* PAWN..KING */
    int from_file, from_rank; /* -1 when not given */
    bool capture;
    int to;
    int promo;              /* 0 if not given */
    bool promo_eq;          /* written with '=' */
    char suffix;            /* '+', '#' or 0 */
} Typed;

static bool parse_typed(const char *in, Typed *t, char *msg, size_t msglen) {
    char s[32];
    int k = 0;
    memset(t, 0, sizeof *t);
    t->from_file = t->from_rank = -1;
    t->to = -1;
    for (const char *c = in; *c && k < 30; c++) {
        if (isspace((unsigned char)*c) || *c == '!' || *c == '?') continue;
        if (*c == '+' || *c == '#') { t->suffix = *c; continue; }
        s[k++] = *c == '0' ? 'O' : *c;
    }
    s[k] = 0;
    /* "e.p." on the end */
    if (k > 4 && strcmp(s + k - 4, "e.p.") == 0) s[k -= 4] = 0;
    if (k == 0) { snprintf(msg, msglen, "Type a move, e.g. e4 or Nf3."); return false; }
    if (strcmp(s, "O-O") == 0 || strcmp(s, "OO") == 0 || strcmp(s, "o-o") == 0) { t->castle_k = true; return true; }
    if (strcmp(s, "O-O-O") == 0 || strcmp(s, "OOO") == 0 || strcmp(s, "o-o-o") == 0) { t->castle_q = true; return true; }

    int i = 0;
    t->piece = PAWN;
    if (strchr("KQRBN", s[0]) && s[0]) { t->piece = (int)(strchr(PIECE_CH, s[0]) - PIECE_CH); i = 1; }
    else if (strchr("kqrn", s[0]) && s[0]) {
        snprintf(msg, msglen, "Piece letters are capitals: %c%s.", toupper((unsigned char)s[0]), s + 1);
        return false;
    }
    /* promotion on the end: =Q, Q, or lower case */
    int end = k;
    if (end >= 3 && strchr("QRBNqrbn", s[end - 1]) && (s[end - 2] == '=' || s[end - 2] == '1' || s[end - 2] == '8')) {
        char pc = (char)toupper((unsigned char)s[end - 1]);
        t->promo = (int)(strchr(PIECE_CH, pc) - PIECE_CH);
        end--;
        if (s[end - 1] == '=') { t->promo_eq = true; end--; }
    }
    if (end - i < 2 || s[end - 2] < 'a' || s[end - 2] > 'h' || s[end - 1] < '1' || s[end - 1] > '8') {
        snprintf(msg, msglen, "'%s' isn't a move. Examples: e4, Nf3, exd5, O-O, e8=Q.", in);
        return false;
    }
    t->to = SQ(s[end - 2] - 'a', s[end - 1] - '1');
    end -= 2;
    /* middle: [file][rank][x or : or -] */
    for (; i < end; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'h' && t->from_file < 0 && t->from_rank < 0) t->from_file = c - 'a';
        else if (c >= '1' && c <= '8' && t->from_rank < 0) t->from_rank = c - '1';
        else if ((c == 'x' || c == ':') && !t->capture) t->capture = true;
        else if (c == '-') continue; /* long notation like Ng1-f3 */
        else {
            snprintf(msg, msglen, "'%s' isn't a move. Examples: e4, Nf3, exd5, O-O, e8=Q.", in);
            return false;
        }
    }
    return true;
}

static bool pawn_capture_hint(const Position *p, const Move *legal, int nl, int to,
                              char *msg, size_t msglen) {
    for (int i = 0; i < nl; i++) {
        if (legal[i].to == to && (legal[i].flags & MF_CAPTURE) && PTYPE(p->sq[legal[i].from]) == PAWN) {
            char s[16];
            move_to_san(p, legal[i], s, sizeof s);
            snprintf(msg, msglen, "Pawn captures name the file they come from: %s.", s);
            return true;
        }
    }
    return false;
}

SanResult san_parse(const Position *p, const char *in, bool strict, Move *out,
                    char *msg, size_t msglen) {
    msg[0] = 0;
    Typed t;
    if (!parse_typed(in, &t, msg, msglen)) return SAN_ERR;

    Move legal[MAX_MOVES];
    int nl = gen_legal(p, legal);
    Move cand[MAX_MOVES];
    int nc = 0;
    bool need_promo = false;
    for (int i = 0; i < nl; i++) {
        Move m = legal[i];
        int pt = PTYPE(p->sq[m.from]);
        if (t.castle_k || t.castle_q) {
            if ((m.flags & MF_CASTLE) && (FILE_OF(m.to) == 6) == t.castle_k) cand[nc++] = m;
            continue;
        }
        if (pt != t.piece || m.to != t.to) continue;
        if (t.from_file >= 0 && FILE_OF(m.from) != t.from_file) continue;
        if (t.from_rank >= 0 && RANK_OF(m.from) != t.from_rank) continue;
        if (m.promo != t.promo) { if (m.promo && !t.promo) need_promo = true; continue; }
        /* a pawn written without a file ("d5") is a push, never a capture */
        if (pt == PAWN && t.from_file < 0 && (m.flags & MF_CAPTURE)) continue;
        cand[nc++] = m;
    }

    char sq[3];
    if (nc == 0) {
        if (t.castle_k || t.castle_q) {
            const char *name = t.castle_k ? "O-O" : "O-O-O";
            int r = p->side == WHITE ? CASTLE_WK : CASTLE_BK;
            if (!t.castle_k) r <<= 1;
            if (!(p->castle & r)) snprintf(msg, msglen, "%s isn't possible: the king or that rook has already moved.", name);
            else if (in_check(p)) snprintf(msg, msglen, "You can't castle out of check.");
            else snprintf(msg, msglen, "%s isn't possible: the path is blocked or attacked.", name);
            return SAN_ERR;
        }
        sq_name(t.to, sq);
        if (need_promo) { snprintf(msg, msglen, "That pawn promotes: write %s=Q (or =R, =B, =N).", sq); return SAN_ERR; }
        /* Would it be possible ignoring check? */
        Move ps[MAX_MOVES];
        int np = gen_pseudo(p, ps);
        for (int i = 0; i < np; i++) {
            if (PTYPE(p->sq[ps[i].from]) == t.piece && ps[i].to == t.to &&
                (t.from_file < 0 || FILE_OF(ps[i].from) == t.from_file) &&
                (t.from_rank < 0 || RANK_OF(ps[i].from) == t.from_rank) &&
                !(t.piece == PAWN && t.from_file < 0 && (ps[i].flags & MF_CAPTURE))) {
                if (in_check(p)) snprintf(msg, msglen, "You're in check: that doesn't get you out of it.");
                else if (t.piece == KING) snprintf(msg, msglen, "Your king can't go to %s: it would be in check.", sq);
                else snprintf(msg, msglen, "That %s is pinned: moving it exposes your king.", piece_word(t.piece));
                return SAN_ERR;
            }
        }
        bool have = false;
        for (int s = 0; s < 64; s++)
            if (p->sq[s] == PIECE(p->side, t.piece)) have = true;
        if (!have) snprintf(msg, msglen, "You have no %s.", piece_word(t.piece));
        else if (t.piece == PAWN && t.from_file < 0 && pawn_capture_hint(p, legal, nl, t.to, msg, msglen))
            ;
        else if (t.from_file >= 0 || t.from_rank >= 0)
            snprintf(msg, msglen, "No %s from there can reach %s.", piece_word(t.piece), sq);
        else
            snprintf(msg, msglen, "No %s can move to %s.", piece_word(t.piece), sq);
        return SAN_ERR;
    }
    if (nc > 1) {
        int k = snprintf(msg, msglen, "Ambiguous - which one? ");
        for (int i = 0; i < nc && k < (int)msglen; i++) {
            char b[16];
            move_to_san(p, cand[i], b, sizeof b);
            k += snprintf(msg + k, msglen - (size_t)k, "%s%s", i ? " or " : "", b);
        }
        return SAN_ERR;
    }

    Move m = cand[0];
    char canon[16];
    move_to_san(p, m, canon, sizeof canon);
    /* compare the typed text with the standard form, stripped of
     * annotations; sign checks are separate */
    char typed[32], c_nosuf[16];
    int k = 0;
    for (const char *c = in; *c && k < 30; c++)
        if (!isspace((unsigned char)*c) && !strchr("!?+#", *c)) typed[k++] = *c;
    typed[k] = 0;
    if (k > 4 && strcmp(typed + k - 4, "e.p.") == 0) typed[k - 4] = 0;
    strcpy(c_nosuf, canon);
    size_t cl = strlen(c_nosuf);
    if (cl && (c_nosuf[cl - 1] == '+' || c_nosuf[cl - 1] == '#')) c_nosuf[cl - 1] = 0;
    char csuf = cl && (canon[cl - 1] == '+' || canon[cl - 1] == '#') ? canon[cl - 1] : 0;

    *out = m;
    bool body_ok = strcmp(typed, c_nosuf) == 0;
    bool suffix_ok = t.suffix == csuf;
    if (body_ok && suffix_ok) return SAN_OK;
    if (strict) {
        if (!body_ok) snprintf(msg, msglen, "Not standard notation - write %s.", canon);
        else if (csuf && !t.suffix) snprintf(msg, msglen, "Strict mode: that move gives %s - write %s.", csuf == '#' ? "mate" : "check", canon);
        else snprintf(msg, msglen, "Strict mode: wrong sign - write %s.", canon);
        return SAN_ERR;
    }
    if (!body_ok) { snprintf(msg, msglen, "Played %s (standard form).", canon); return SAN_OK_NOTE; }
    if (t.suffix && t.suffix != csuf) {
        if (!csuf) snprintf(msg, msglen, "Played %s - note: it isn't check.", canon);
        else snprintf(msg, msglen, "Played %s - it's %s!", canon, csuf == '#' ? "mate" : "check, not mate");
        return SAN_OK_NOTE;
    }
    if (csuf == '#') { snprintf(msg, msglen, "%s - checkmate.", canon); return SAN_OK_NOTE; }
    return SAN_OK; /* a check typed without '+' outside strict mode is fine */
}

/* ------------------------------------------------------------- game */

bool game_init(Game *g, const char *fen) {
    memset(g, 0, sizeof *g);
    if (!fen) fen = STARTPOS_FEN;
    if (!pos_from_fen(&g->start, fen)) return false;
    pos_to_fen(&g->start, g->start_fen, sizeof g->start_fen);
    g->pos = g->start;
    g->hashes[0] = pos_hash(&g->pos);
    g->forced = ST_ONGOING;
    return true;
}

bool game_push(Game *g, Move m) {
    if (g->nply >= MAX_PLY) return false;
    move_to_san(&g->pos, m, g->san[g->nply], sizeof g->san[0]);
    g->moves[g->nply] = m;
    make_move(&g->pos, m);
    g->nply++;
    g->hashes[g->nply] = pos_hash(&g->pos);
    return true;
}

void game_pop(Game *g) {
    if (g->nply == 0) return;
    int n = g->nply - 1;
    g->pos = g->start;
    for (int i = 0; i < n; i++) make_move(&g->pos, g->moves[i]);
    g->nply = n;
    g->forced = ST_ONGOING;
}

static bool insufficient(const Position *p) {
    int minors = 0, bishops_sq[2] = {0, 0}, nb = 0;
    for (int s = 0; s < 64; s++) {
        int t = PTYPE(p->sq[s]);
        if (t == PAWN || t == ROOK || t == QUEEN) return false;
        if (t == KNIGHT) minors++;
        if (t == BISHOP) { minors++; nb++; bishops_sq[(FILE_OF(s) + RANK_OF(s)) & 1]++; }
    }
    if (minors <= 1) return true;
    /* only bishops, all on one colour */
    if (nb == minors && (bishops_sq[0] == 0 || bishops_sq[1] == 0)) return true;
    return false;
}

GameStatus game_status(const Game *g) {
    if (g->forced != ST_ONGOING) return g->forced;
    Move ms[MAX_MOVES];
    if (gen_legal(&g->pos, ms) == 0) return in_check(&g->pos) ? ST_CHECKMATE : ST_STALEMATE;
    if (g->pos.halfmove >= 100) return ST_DRAW_50;
    int reps = 0;
    for (int i = g->nply; i >= 0 && i >= g->nply - g->pos.halfmove; i -= 2)
        if (g->hashes[i] == g->hashes[g->nply]) reps++;
    if (reps >= 3) return ST_DRAW_REPETITION;
    if (insufficient(&g->pos)) return ST_DRAW_MATERIAL;
    return ST_ONGOING;
}

const char *game_result(const Game *g) {
    switch (game_status(g)) {
    case ST_CHECKMATE: return g->pos.side == WHITE ? "0-1" : "1-0";
    case ST_RESIGNED: return g->resigned_side == WHITE ? "0-1" : "1-0";
    case ST_ONGOING: return "*";
    default: return "1/2-1/2";
    }
}

const char *status_text(GameStatus s) {
    switch (s) {
    case ST_CHECKMATE: return "Checkmate";
    case ST_STALEMATE: return "Stalemate";
    case ST_DRAW_50: return "Draw by the fifty-move rule";
    case ST_DRAW_REPETITION: return "Draw by threefold repetition";
    case ST_DRAW_MATERIAL: return "Draw: insufficient material";
    case ST_RESIGNED: return "Resignation";
    case ST_DRAW_AGREED: return "Draw agreed";
    default: return "";
    }
}

void game_movetext(const Game *g, char *out, size_t n) {
    size_t k = 0;
    out[0] = 0;
    int num = g->start.fullmove;
    bool black_first = g->start.side == BLACK;
    for (int i = 0; i < g->nply && k < n; i++) {
        bool white_move = (i % 2 == 0) != black_first;
        if (white_move) k += (size_t)snprintf(out + k, n - k, "%d. ", num);
        else if (i == 0) k += (size_t)snprintf(out + k, n - k, "%d... ", num);
        if (k >= n) break;
        k += (size_t)snprintf(out + k, n - k, "%s ", g->san[i]);
        if (!white_move) num++;
    }
    if (k < n) snprintf(out + k, n - k, "%s", game_result(g));
}
