/* chess.h - rules core: board, legal moves, SAN, FEN, game state. */
#ifndef CHESS_H
#define CHESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { EMPTY = 0, PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum { WHITE = 0, BLACK = 1 };

#define PIECE(c, t) ((uint8_t)((t) | ((c) << 3)))
#define PTYPE(p) ((p) & 7)
#define PCOLOR(p) ((p) >> 3)
#define SQ(f, r) ((r) * 8 + (f))
#define FILE_OF(s) ((s) & 7)
#define RANK_OF(s) ((s) >> 3)

enum { CASTLE_WK = 1, CASTLE_WQ = 2, CASTLE_BK = 4, CASTLE_BQ = 8 };
enum { MF_CAPTURE = 1, MF_EP = 2, MF_CASTLE = 4, MF_DOUBLE = 8 };

typedef struct {
    uint8_t from, to, promo, flags; /* promo is a piece type or 0 */
} Move;

typedef struct {
    uint8_t sq[64]; /* a1 = 0, h8 = 63 */
    int side;
    int castle;
    int ep; /* en passant target square, or -1 */
    int halfmove, fullmove;
    int king[2];
} Position;

#define MAX_MOVES 256
#define STARTPOS_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

/* Positions and moves */
bool pos_from_fen(Position *p, const char *fen);
void pos_to_fen(const Position *p, char *out, size_t n);
bool is_attacked(const Position *p, int sq, int by);
bool in_check(const Position *p);
int gen_pseudo(const Position *p, Move *out);
int gen_legal(const Position *p, Move *out);
void make_move(Position *p, Move m);
bool move_eq(Move a, Move b);
uint64_t pos_hash(const Position *p);
uint64_t perft(const Position *p, int depth);

/* Notation */
void move_to_san(const Position *p, Move m, char *out, size_t n);
void move_to_uci(Move m, char *out);
bool move_from_uci(const Position *p, const char *uci, Move *out);
void sq_name(int sq, char *out);

typedef enum { SAN_OK, SAN_OK_NOTE, SAN_ERR } SanResult;
/* Reads a move typed in standard notation. On SAN_OK_NOTE the move was
 * accepted but written non-standardly; msg says what the standard form is.
 * On SAN_ERR msg explains why it was refused. strict=true refuses anything
 * that is not the exact standard form, check signs included. */
SanResult san_parse(const Position *p, const char *in, bool strict, Move *out,
                    char *msg, size_t msglen);

/* Game: positions with history, for repetition, undo and PGN */
#define MAX_PLY 1024

typedef enum {
    ST_ONGOING, ST_CHECKMATE, ST_STALEMATE, ST_DRAW_50, ST_DRAW_REPETITION,
    ST_DRAW_MATERIAL, ST_RESIGNED, ST_DRAW_AGREED
} GameStatus;

typedef struct {
    char start_fen[100];
    Position start, pos;
    Move moves[MAX_PLY];
    char san[MAX_PLY][16];
    uint64_t hashes[MAX_PLY + 1];
    int nply;
    GameStatus forced; /* resignation or agreement, else ST_ONGOING */
    int resigned_side;
} Game;

bool game_init(Game *g, const char *fen);
bool game_push(Game *g, Move m);
void game_pop(Game *g);
GameStatus game_status(const Game *g);
const char *game_result(const Game *g); /* "1-0", "0-1", "1/2-1/2", "*" */
const char *status_text(GameStatus s);
void game_movetext(const Game *g, char *out, size_t n);

#endif
