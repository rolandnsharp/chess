/* ai.c - a small alpha-beta engine, used when Stockfish isn't available.
 * Roughly club strength at the top level; lower levels search less and add
 * noise so they make human-looking mistakes. */
#include "engine.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Ai {
    pthread_t thread;
    bool running;
    atomic_int done, abort;
    Game game;
    int level;
    Move best;
    long nodes;
    double deadline;
};

static const int VALUE[7] = {0, 100, 320, 330, 500, 900, 0};

/* piece-square tables, white's view, a1 = index 0 */
static const int PST[7][64] = {
    {0},
    { 0, 0, 0, 0, 0, 0, 0, 0,  5, 10, 10,-20,-20, 10, 10,  5,  5, -5,-10,  0,  0,-10, -5,  5,
      0, 0, 0,20,20, 0, 0, 0,  5,  5, 10, 25, 25, 10,  5,  5, 10, 10, 20, 30, 30, 20, 10, 10,
     50,50,50,50,50,50,50,50,  0,  0,  0,  0,  0,  0,  0,  0},
    {-50,-40,-30,-30,-30,-30,-40,-50, -40,-20,0,5,5,0,-20,-40, -30,5,10,15,15,10,5,-30,
     -30,0,15,20,20,15,0,-30, -30,5,15,20,20,15,5,-30, -30,0,10,15,15,10,0,-30,
     -40,-20,0,0,0,0,-20,-40, -50,-40,-30,-30,-30,-30,-40,-50},
    {-20,-10,-10,-10,-10,-10,-10,-20, -10,5,0,0,0,0,5,-10, -10,10,10,10,10,10,10,-10,
     -10,0,10,10,10,10,0,-10, -10,5,5,10,10,5,5,-10, -10,0,5,10,10,5,0,-10,
     -10,0,0,0,0,0,0,-10, -20,-10,-10,-10,-10,-10,-10,-20},
    {0,0,0,5,5,0,0,0, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5,
     -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, 5,10,10,10,10,10,10,5, 0,0,0,0,0,0,0,0},
    {-20,-10,-10,-5,-5,-10,-10,-20, -10,0,5,0,0,0,0,-10, -10,5,5,5,5,5,0,-10,
     0,0,5,5,5,5,0,-5, -5,0,5,5,5,5,0,-5, -10,0,5,5,5,5,0,-10,
     -10,0,0,0,0,0,0,-10, -20,-10,-10,-5,-5,-10,-10,-20},
    {20,30,10,0,0,10,30,20, 20,20,0,0,0,0,20,20, -10,-20,-20,-20,-20,-20,-20,-10,
     -20,-30,-30,-40,-40,-30,-30,-20, -30,-40,-40,-50,-50,-40,-40,-30,
     -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30,
     -30,-40,-40,-50,-50,-40,-40,-30},
};
/* king in the endgame: head for the centre */
static const int KING_END[64] = {
    -50,-30,-30,-30,-30,-30,-30,-50, -30,-30,0,0,0,0,-30,-30, -30,-10,20,30,30,20,-10,-30,
    -30,-10,30,40,40,30,-10,-30, -30,-10,30,40,40,30,-10,-30, -30,-10,20,30,30,20,-10,-30,
    -30,-20,-10,0,0,-10,-20,-30, -50,-40,-30,-20,-20,-30,-40,-50};

#define MATE 30000
#define INF 32000

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int evaluate(const Position *p) {
    int score = 0, material = 0;
    for (int s = 0; s < 64; s++) {
        int t = PTYPE(p->sq[s]);
        if (t && t != PAWN && t != KING) material += VALUE[t];
    }
    bool endgame = material <= 2600;
    for (int s = 0; s < 64; s++) {
        uint8_t pc = p->sq[s];
        if (!pc) continue;
        int t = PTYPE(pc), c = PCOLOR(pc);
        int idx = c == WHITE ? s : (s ^ 56);
        int v = VALUE[t] + (t == KING && endgame ? KING_END[idx] : PST[t][idx]);
        score += c == WHITE ? v : -v;
    }
    return p->side == WHITE ? score : -score;
}

static int order_key(const Position *p, Move m) {
    int k = 0;
    if (m.flags & MF_CAPTURE) k += 10 * VALUE[PTYPE(p->sq[m.to])] - VALUE[PTYPE(p->sq[m.from])] + 10000;
    if (m.promo) k += VALUE[m.promo] + 9000;
    return k;
}

static void sort_moves(const Position *p, Move *ms, int n, const Move *first) {
    int key[MAX_MOVES];
    for (int i = 0; i < n; i++) {
        key[i] = order_key(p, ms[i]);
        if (first && move_eq(ms[i], *first)) key[i] = 1 << 20;
    }
    for (int i = 1; i < n; i++) {
        Move m = ms[i];
        int k = key[i], j = i - 1;
        while (j >= 0 && key[j] < k) { ms[j + 1] = ms[j]; key[j + 1] = key[j]; j--; }
        ms[j + 1] = m;
        key[j + 1] = k;
    }
}

static bool out_of_time(Ai *a) {
    if ((++a->nodes & 1023) == 0 && (atomic_load(&a->abort) || now_s() > a->deadline))
        atomic_store(&a->abort, 1);
    return atomic_load(&a->abort);
}

/* pseudo-legal moves filtered to legal ones; captures_only for quiescence */
static int gen_moves(const Position *p, Move *out, bool captures_only) {
    Move tmp[MAX_MOVES];
    int n = gen_pseudo(p, tmp), k = 0;
    for (int i = 0; i < n; i++) {
        if (captures_only && !(tmp[i].flags & MF_CAPTURE) && tmp[i].promo != QUEEN) continue;
        if (captures_only && tmp[i].promo && tmp[i].promo != QUEEN) continue;
        Position q = *p;
        make_move(&q, tmp[i]);
        if (!is_attacked(&q, q.king[p->side], q.side)) out[k++] = tmp[i];
    }
    return k;
}

static int quiesce(Ai *a, const Position *p, int alpha, int beta, int ply) {
    if (out_of_time(a)) return 0;
    int stand = evaluate(p);
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;
    if (ply > 30) return stand;
    Move ms[MAX_MOVES];
    int k = gen_moves(p, ms, true);
    sort_moves(p, ms, k, NULL);
    for (int i = 0; i < k; i++) {
        /* delta pruning: even winning this piece can't lift us to alpha */
        int gain = VALUE[PTYPE(p->sq[ms[i].to])] + (ms[i].promo ? 800 : 0) + ((ms[i].flags & MF_EP) ? 100 : 0);
        if (stand + gain + 200 < alpha) continue;
        Position q = *p;
        make_move(&q, ms[i]);
        int s = -quiesce(a, &q, -beta, -alpha, ply + 1);
        if (s >= beta) return s;
        if (s > alpha) alpha = s;
    }
    return alpha;
}

static int search(Ai *a, const Position *p, int depth, int alpha, int beta, int ply) {
    if (out_of_time(a)) return 0;
    Move ms[MAX_MOVES];
    int n = gen_moves(p, ms, false);
    if (n == 0) return in_check(p) ? -MATE + ply : 0;
    if (p->halfmove >= 100) return 0;
    bool check = in_check(p);
    if (depth <= 0 && !check) return quiesce(a, p, alpha, beta, ply);
    if (depth <= 0) depth = 1; /* extend out of check */
    sort_moves(p, ms, n, NULL);
    int best = -INF;
    for (int i = 0; i < n; i++) {
        Position q = *p;
        make_move(&q, ms[i]);
        int s = -search(a, &q, depth - 1, -beta, -alpha, ply + 1);
        if (s > best) best = s;
        if (s > alpha) alpha = s;
        if (alpha >= beta) break;
    }
    return best;
}

static void *think(void *arg) {
    Ai *a = arg;
    const Position *p = &a->game.pos;
    Move ms[MAX_MOVES];
    int n = gen_legal(p, ms);
    a->best = ms[0];
    /* level 1..10: time and depth grow, noise shrinks */
    int max_depth = a->level <= 2 ? 1 : a->level <= 4 ? 2 : a->level - 2;
    double budget = 0.3 + 0.3 * a->level;
    int noise = a->level >= 8 ? 0 : (8 - a->level) * 14;
    a->deadline = now_s() + budget;
    a->nodes = 0;

    Move best = ms[0];
    for (int depth = 1; depth <= max_depth; depth++) {
        sort_moves(p, ms, n, &best);
        /* Alpha-beta only bounds the scores of moves worse than alpha. With
         * noise, a move up to 2*noise worse could still win, so the window
         * is widened by that much to get its exact score. */
        int best_true = -INF, best_score = -INF;
        Move cur = ms[0];
        for (int i = 0; i < n; i++) {
            Position q = *p;
            make_move(&q, ms[i]);
            int floor = best_true <= -INF + 2 * noise ? -INF : best_true - 2 * noise;
            int s = -search(a, &q, depth - 1, -INF, -floor, 1);
            if (atomic_load(&a->abort)) break; /* this move's score is unfinished */
            if (s > best_true) best_true = s;
            if (s <= floor) continue; /* only a bound, and too low to win */
            if (noise) s += rand() % (2 * noise + 1) - noise;
            if (s > best_score) { best_score = s; cur = ms[i]; }
        }
        /* a partly searched depth still counts if it found something
         * better than the previous best, which it searched first */
        if (best_score > -INF) best = cur;
        if (atomic_load(&a->abort)) break;
        if (best_score > MATE - 100) break;
    }
    a->best = best;
    atomic_store(&a->done, 1);
    return NULL;
}

Ai *ai_create(void) {
    Ai *a = calloc(1, sizeof *a);
    srand((unsigned)time(NULL));
    return a;
}

void ai_go(Ai *a, const Game *g, int level) {
    ai_cancel(a);
    a->game = *g;
    a->level = level;
    atomic_store(&a->done, 0);
    atomic_store(&a->abort, 0);
    a->running = pthread_create(&a->thread, NULL, think, a) == 0;
}

bool ai_poll(Ai *a, Move *out) {
    if (!a->running || !atomic_load(&a->done)) return false;
    pthread_join(a->thread, NULL);
    a->running = false;
    *out = a->best;
    return true;
}

void ai_cancel(Ai *a) {
    if (!a->running) return;
    atomic_store(&a->abort, 1);
    pthread_join(a->thread, NULL);
    a->running = false;
}

void ai_destroy(Ai *a) {
    if (!a) return;
    ai_cancel(a);
    free(a);
}
