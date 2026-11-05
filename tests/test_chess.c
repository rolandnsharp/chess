/* Rules tests: perft counts on the standard positions, notation checks,
 * and an optional PGN corpus whose every move must parse in strict mode. */
#include "../src/chess.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_perft(void) {
    struct { const char *fen; int depth; uint64_t nodes; } cases[] = {
        {STARTPOS_FEN, 1, 20}, {STARTPOS_FEN, 2, 400}, {STARTPOS_FEN, 3, 8902},
        {STARTPOS_FEN, 4, 197281}, {STARTPOS_FEN, 5, 4865609},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487},
        {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Position p;
        CHECK(pos_from_fen(&p, cases[i].fen), "fen %s", cases[i].fen);
        clock_t t0 = clock();
        uint64_t n = perft(&p, cases[i].depth);
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        CHECK(n == cases[i].nodes, "perft %d of %s: got %llu want %llu", cases[i].depth,
              cases[i].fen, (unsigned long long)n, (unsigned long long)cases[i].nodes);
        printf("perft %d = %10llu  %.2fs  %s\n", cases[i].depth, (unsigned long long)n, secs, cases[i].fen);
    }
}

static void expect_san(const char *fen, const char *in, bool strict, SanResult want, const char *want_san) {
    Position p;
    pos_from_fen(&p, fen);
    Move m;
    char msg[160];
    SanResult r = san_parse(&p, in, strict, &m, msg, sizeof msg);
    CHECK(r == want, "'%s' gave %d want %d (%s)", in, r, want, msg);
    if (r != SAN_ERR && want_san) {
        char s[16];
        move_to_san(&p, m, s, sizeof s);
        CHECK(strcmp(s, want_san) == 0, "'%s' became %s want %s", in, s, want_san);
    }
    printf("  %-8s -> %-11s %s\n", in, r == SAN_OK ? "ok" : r == SAN_OK_NOTE ? "ok+note" : "refused", msg);
}

static void test_san(void) {
    const char *two_knights = "4k3/8/8/8/8/8/8/1N2KN2 w - - 0 1"; /* Nb1, Nf1: both reach d2 */
    printf("notation:\n");
    expect_san(STARTPOS_FEN, "e4", false, SAN_OK, "e4");
    expect_san(STARTPOS_FEN, "Nf3", false, SAN_OK, "Nf3");
    expect_san(STARTPOS_FEN, "nf3", false, SAN_ERR, NULL);
    expect_san(STARTPOS_FEN, "Ng1f3", false, SAN_OK_NOTE, "Nf3");
    expect_san(STARTPOS_FEN, "Nf3", true, SAN_OK, "Nf3");
    expect_san(STARTPOS_FEN, "e5", false, SAN_ERR, NULL);
    expect_san(STARTPOS_FEN, "Bc4", false, SAN_ERR, NULL);
    expect_san(STARTPOS_FEN, "O-O", false, SAN_ERR, NULL);
    expect_san(two_knights, "Nd2", false, SAN_ERR, NULL);
    expect_san(two_knights, "Nbd2", false, SAN_OK, "Nbd2");
    expect_san(two_knights, "Nfd2", true, SAN_OK, "Nfd2");
    expect_san("4k3/8/8/8/8/8/8/R3K2R w KQ - 0 1", "0-0", false, SAN_OK_NOTE, "O-O");
    expect_san("4k3/8/8/8/8/8/8/R3K2R w KQ - 0 1", "O-O-O", true, SAN_OK, "O-O-O");
    /* promotions */
    expect_san("8/P3k3/8/8/8/8/8/4K3 w - - 0 1", "a8", false, SAN_ERR, NULL);
    expect_san("8/P3k3/8/8/8/8/8/4K3 w - - 0 1", "a8=Q", false, SAN_OK, "a8=Q");
    expect_san("8/P3k3/8/8/8/8/8/4K3 w - - 0 1", "a8Q", false, SAN_OK_NOTE, "a8=Q");
    expect_san("8/P3k3/8/8/8/8/8/4K3 w - - 0 1", "a8=N", true, SAN_OK, "a8=N");
    /* en passant */
    expect_san("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2", "exd6", true, SAN_OK, "exd6");
    expect_san("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2", "exd6 e.p.", false, SAN_OK, "exd6");
    /* check and mate signs: scholar's mate */
    const char *sm = "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5Q2/PPPP1PPP/RNB1K1NR w KQkq - 2 3";
    expect_san(sm, "Qxf7#", true, SAN_OK, "Qxf7#");
    expect_san(sm, "Qxf7", true, SAN_ERR, NULL);
    expect_san(sm, "Qxf7+", false, SAN_OK_NOTE, "Qxf7#");
    expect_san(sm, "Qf7", false, SAN_OK_NOTE, "Qxf7#");
    /* pinned piece and king into check */
    expect_san("4k3/4r3/8/8/8/8/4N3/4K3 w - - 0 1", "Nc3", false, SAN_ERR, NULL);
    expect_san("4k3/8/8/8/8/8/3r4/4K3 w - - 0 1", "Kd1", false, SAN_ERR, NULL);
    /* pawn capture without file */
    expect_san("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "d5", false, SAN_ERR, NULL);
    expect_san("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "exd5", true, SAN_OK, "exd5");
}

/* Reads a PGN file; returns the number of moves checked. */
static long test_pgn_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("no corpus at %s, skipping\n", path); return 0; }
    static char line[1 << 16];
    Game g;
    game_init(&g, NULL);
    bool skip = false, in_game = false;
    long games = 0, moves = 0, bad_games = 0;
    char fen[128] = "";
    int bad_here = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '[') {
            if (in_game) { games++; if (bad_here) bad_games++; in_game = false; bad_here = 0; fen[0] = 0; skip = false; }
            if ((strncmp(line, "[Variant ", 9) == 0 && !strstr(line, "\"Standard\"")) ||
                (strncmp(line, "[Event ", 7) == 0 && (strstr(line, "960") || strstr(line, "Crazyhouse"))))
                skip = true;
            if (strncmp(line, "[FEN \"", 6) == 0) { snprintf(fen, sizeof fen, "%s", line + 6); char *q = strchr(fen, '"'); if (q) *q = 0; }
            continue;
        }
        if (line[0] == '\n' || line[0] == '\r') continue;
        if (!in_game) { in_game = true; game_init(&g, fen[0] ? fen : NULL); }
        if (skip) continue;
        /* strip {comments} (chess.com clock annotations) */
        char *tok = strtok(line, " \t\r\n");
        int depth = 0;
        for (; tok; tok = strtok(NULL, " \t\r\n")) {
            if (bad_here) break;
            for (char *c = tok; *c; c++) { if (*c == '{') depth++; }
            if (depth) { for (char *c = tok; *c; c++) if (*c == '}') depth--; continue; }
            if (strchr(tok, '.') || strcmp(tok, "1-0") == 0 || strcmp(tok, "0-1") == 0 ||
                strcmp(tok, "1/2-1/2") == 0 || strcmp(tok, "*") == 0 || tok[0] == '$')
                continue;
            Move m;
            char msg[160];
            SanResult r = san_parse(&g.pos, tok, true, &m, msg, sizeof msg);
            if (r != SAN_OK) {
                char fenb[100];
                pos_to_fen(&g.pos, fenb, sizeof fenb);
                if (bad_games < 5) printf("FAIL corpus move '%s' in %s: %s\n", tok, fenb, msg);
                bad_here = 1;
                failures++;
                break;
            }
            game_push(&g, m);
            CHECK(strcmp(g.san[g.nply - 1], tok) == 0, "san %s vs %s", g.san[g.nply - 1], tok);
            moves++;
        }
    }
    if (in_game) { games++; if (bad_here) bad_games++; }
    fclose(f);
    printf("corpus %s: %ld games, %ld moves, %ld games with a refused move\n", path, games, moves, bad_games);
    return moves;
}

int main(int argc, char **argv) {
    test_san();
    test_perft();
    for (int i = 1; i < argc; i++) test_pgn_file(argv[i]);
    printf(failures ? "\n%d FAILURES\n" : "\nall tests passed\n", failures);
    return failures != 0;
}
