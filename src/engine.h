/* engine.h - the opponent: Stockfish over UCI if available, otherwise the
 * small built-in engine in ai.c. Both think in the background. */
#ifndef ENGINE_H
#define ENGINE_H

#include "chess.h"

#define LEVEL_MIN 1
#define LEVEL_MAX 10

typedef struct Opponent Opponent;

Opponent *opp_create(const char *base_dir);
void opp_destroy(Opponent *o);
void opp_name(const Opponent *o, int level, char *buf, size_t n);
bool opp_is_stockfish(const Opponent *o);
void opp_go(Opponent *o, const Game *g, int level);
bool opp_poll(Opponent *o, Move *out); /* true once, when the move is ready */
void opp_cancel(Opponent *o);
bool opp_busy(const Opponent *o);
/* Blocking: centipawns for the side that plays m, after m (mates as
 * +-10000). Stockfish only; false with the built-in engine. */
bool opp_eval_after(Opponent *o, const Position *p, Move m, int nodes, int *cp);

/* ai.c: built-in engine */
typedef struct Ai Ai;
Ai *ai_create(void);
void ai_destroy(Ai *a);
void ai_go(Ai *a, const Game *g, int level);
bool ai_poll(Ai *a, Move *out);
void ai_cancel(Ai *a);

#endif
