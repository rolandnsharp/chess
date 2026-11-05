/* engine.c - opponent: Stockfish over UCI through pipes, or the built-in
 * engine. Reads from Stockfish are non-blocking so the window never stalls. */
#include "engine.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct Opponent {
    /* Stockfish */
    pid_t pid;
    int to_fd, from_fd;
    char buf[8192];
    size_t len;
    bool sf_busy;
    int ignore; /* bestmoves to discard after a cancel */
    Game game;  /* the position being thought about */
    /* fallback */
    Ai *ai;
    bool ai_busy;
};

static void sf_send(Opponent *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sf_send(Opponent *o, const char *fmt, ...) {
    char line[16384];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = write(o->to_fd, line + off, (size_t)(n - off));
        if (w <= 0) { if (errno == EINTR) continue; return; }
        off += w;
    }
}

/* Next complete line from Stockfish, or false if none yet. */
static bool sf_line(Opponent *o, char *out, size_t n) {
    for (;;) {
        char *nl = memchr(o->buf, '\n', o->len);
        if (nl) {
            size_t l = (size_t)(nl - o->buf);
            size_t c = l < n - 1 ? l : n - 1;
            memcpy(out, o->buf, c);
            out[c] = 0;
            memmove(o->buf, nl + 1, o->len - l - 1);
            o->len -= l + 1;
            return true;
        }
        if (o->len == sizeof o->buf) o->len = 0; /* overlong line: drop */
        ssize_t r = read(o->from_fd, o->buf + o->len, sizeof o->buf - o->len);
        if (r <= 0) return false;
        o->len += (size_t)r;
    }
}

static bool sf_wait_for(Opponent *o, const char *prefix, int timeout_ms) {
    char line[1024];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        while (sf_line(o, line, sizeof line))
            if (strncmp(line, prefix, strlen(prefix)) == 0) return true;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms > timeout_ms) return false;
        struct pollfd pf = {o->from_fd, POLLIN, 0};
        poll(&pf, 1, 50);
    }
}

static bool sf_start(Opponent *o, const char *path) {
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execlp(path, path, (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    o->pid = pid;
    o->to_fd = in[1];
    o->from_fd = out[0];
    fcntl(o->from_fd, F_SETFL, O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);
    sf_send(o, "uci");
    if (!sf_wait_for(o, "uciok", 3000)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(o->to_fd); close(o->from_fd);
        o->pid = 0;
        return false;
    }
    sf_send(o, "setoption name Threads value 2");
    sf_send(o, "setoption name Hash value 64");
    sf_send(o, "isready");
    sf_wait_for(o, "readyok", 5000);
    return true;
}

Opponent *opp_create(const char *base_dir) {
    Opponent *o = calloc(1, sizeof *o);
    const char *env = getenv("CHESS_ENGINE");
    char local[1024];
    snprintf(local, sizeof local, "%sthird_party/Stockfish/src/stockfish", base_dir ? base_dir : "");
    const char *tries[] = {env, local, "stockfish", "/usr/games/stockfish"};
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        if (!tries[i] || !*tries[i]) continue;
        if (strchr(tries[i], '/') && access(tries[i], X_OK) != 0) continue;
        if (sf_start(o, tries[i])) return o;
    }
    o->ai = ai_create();
    return o;
}

bool opp_is_stockfish(const Opponent *o) { return o->pid > 0; }

static int level_elo(int level) { return 1320 + (level - 1) * 200; }

void opp_name(const Opponent *o, int level, char *buf, size_t n) {
    if (o->pid > 0) {
        if (level >= LEVEL_MAX) snprintf(buf, n, "Stockfish (full strength)");
        else snprintf(buf, n, "Stockfish (Elo %d)", level_elo(level));
    } else {
        snprintf(buf, n, "Built-in engine (level %d)", level);
    }
}

void opp_go(Opponent *o, const Game *g, int level) {
    opp_cancel(o);
    o->game = *g;
    if (o->pid > 0) {
        if (level >= LEVEL_MAX) {
            sf_send(o, "setoption name UCI_LimitStrength value false");
        } else {
            sf_send(o, "setoption name UCI_LimitStrength value true");
            sf_send(o, "setoption name UCI_Elo value %d", level_elo(level));
        }
        char moves[MAX_PLY * 6 + 16];
        size_t k = 0;
        moves[0] = 0;
        for (int i = 0; i < g->nply; i++) {
            char u[8];
            move_to_uci(g->moves[i], u);
            k += (size_t)snprintf(moves + k, sizeof moves - k, " %s", u);
        }
        sf_send(o, "position fen %s%s%s", g->start_fen, g->nply ? " moves" : "", moves);
        /* think a little longer at higher levels; it's a training game */
        sf_send(o, "go movetime %d", 400 + level * 150);
        o->sf_busy = true;
    } else {
        ai_go(o->ai, g, level);
        o->ai_busy = true;
    }
}

bool opp_poll(Opponent *o, Move *out) {
    if (o->pid > 0) {
        char line[1024];
        while (sf_line(o, line, sizeof line)) {
            if (strncmp(line, "bestmove ", 9) != 0) continue;
            if (o->ignore > 0) { o->ignore--; continue; }
            o->sf_busy = false;
            char uci[8] = {0};
            sscanf(line + 9, "%7s", uci);
            if (move_from_uci(&o->game.pos, uci, out)) return true;
        }
        return false;
    }
    if (o->ai_busy && ai_poll(o->ai, out)) { o->ai_busy = false; return true; }
    return false;
}

void opp_cancel(Opponent *o) {
    if (o->pid > 0) {
        if (o->sf_busy) { sf_send(o, "stop"); o->ignore++; o->sf_busy = false; }
    } else if (o->ai_busy) {
        ai_cancel(o->ai);
        o->ai_busy = false;
    }
}

bool opp_eval_after(Opponent *o, const Position *p, Move m, int nodes, int *cp) {
    if (o->pid <= 0) return false;
    opp_cancel(o);
    /* let the cancelled search's bestmove arrive and be discarded */
    if (o->ignore > 0) {
        char line[1024];
        for (int tries = 0; o->ignore > 0 && tries < 200; tries++) {
            while (o->ignore > 0 && sf_line(o, line, sizeof line))
                if (!strncmp(line, "bestmove", 8)) o->ignore--;
            if (o->ignore > 0) { struct pollfd pf = {o->from_fd, POLLIN, 0}; poll(&pf, 1, 20); }
        }
        o->ignore = 0;
    }
    char fen[100], uci[8];
    pos_to_fen(p, fen, sizeof fen);
    move_to_uci(m, uci);
    sf_send(o, "setoption name UCI_LimitStrength value false");
    sf_send(o, "position fen %s moves %s", fen, uci);
    sf_send(o, "go nodes %d", nodes);
    char line[1024];
    int score = 0;
    bool have = false;
    for (int waited = 0; waited < 20000;) {
        if (!sf_line(o, line, sizeof line)) {
            struct pollfd pf = {o->from_fd, POLLIN, 0};
            poll(&pf, 1, 20);
            waited += 20;
            continue;
        }
        if (!strncmp(line, "bestmove", 8)) break;
        char *sc = strstr(line, " score ");
        if (!sc) continue;
        int v;
        /* scores are for the side to move, i.e. the opponent */
        if (sscanf(sc, " score cp %d", &v) == 1) { score = -v; have = true; }
        else if (sscanf(sc, " score mate %d", &v) == 1) {
            score = v <= 0 ? 10000 + v : -10000 + v; /* mate 0: we delivered mate */
            have = true;
        }
    }
    if (have) *cp = score;
    return have;
}

bool opp_busy(const Opponent *o) { return o->sf_busy || o->ai_busy; }

void opp_destroy(Opponent *o) {
    if (!o) return;
    if (o->pid > 0) {
        sf_send(o, "quit");
        close(o->to_fd);
        close(o->from_fd);
        waitpid(o->pid, NULL, 0);
    }
    ai_destroy(o->ai);
    free(o);
}
