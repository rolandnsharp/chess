# Notation Trainer

A chess board you play by typing standard notation (SAN) — `e4`, `Nf3`, `exd5`,
`Nbd7`, `O-O`, `e8=Q`. No mouse, so every move is visualised first.

## Build and run

    make            # builds ./chess and ./test_chess
    make test       # perft + notation tests + every move of the games in data/, if downloaded
    make stockfish  # clones and builds Stockfish in third_party/ (~10 min on ARM;
                    # set SF_ARCH=x86-64-avx2 etc. on other machines)
    ./chess [--black] [--level 1-10] [--fen FEN]

Needs SDL3, cairo, pango and librsvg. Without Stockfish the app uses its small
built-in engine; with it, levels 1–9 are Elo 1320–2920 and 10 is full strength.

## Commands (type in the move box)

| | |
|---|---|
| `/new [white\|black]` | new game |
| `/undo` | take back your last move |
| `/level N` | opponent strength 1–10 |
| `/blind` / `/peek` | hide the pieces / show them for 3 s |
| `/strict` | exact SAN only, including `+` and `#` |
| `/coords` | board labels on/off (`--no-coords` to start without) |
| `/squares` | 30 s drill: name the highlighted square (no Enter needed) |
| `/drill [puzzles\|punish\|lines\|endgames]` | drills made from your own games (all kinds if none given) |
| `/answer`, `/next`, `/play` | give up and see the answer, skip, leave drills |
| `/flip`, `/fen`, `/pgn`, `/resign`, `/help`, `/quit` | |

Up/Down recall earlier input; Esc clears. Finished games are saved to `games/`.

Moves that are legal but written non-standardly (`Ng1f3`, `0-0`, `a8Q`, a missing `x`)
are played, and the standard form is shown. Refusals say why: ambiguous, pinned,
king would be in check, pawn captures need their file, and so on.

## Drills from your games

    python3 -m venv .venv && .venv/bin/pip install chess
    .venv/bin/python analysis/analyse.py --games 1000   # Stockfish over your recent games (resumable)
    .venv/bin/python analysis/report.py                 # data/analysis/report.md + drills.tsv

Games are read from `data/chesscom/all.pgn` and `data/lichess/all.pgn`; set the
player names in `ME` in `analysis/analyse.py`. Four kinds of drill:

- **puzzles**: positions where you blundered; find the better move. Stockfish
  accepts any move that is as good as the solution.
- **punish**: your opponent blundered and you missed it.
- **lines**: your opening repertoire as you actually play it, against the replies
  you actually meet, with costly moves replaced by the engine's choice.
- **endgames**: winning endgames you didn't win; win them against full-strength Stockfish.

Spaced repetition: a drill you get right comes back after 1, 3, 7, 21, then 60
days; a miss comes back later in the same session. Progress is in `data/progress.tsv`.

## Layout

- `src/chess.[ch]` — rules, SAN parse/print, FEN, game state (no dependencies)
- `src/engine.c` — Stockfish over UCI; `src/ai.c` — fallback alpha-beta engine
- `src/render.c` — cairo/pango drawing; `src/main.c` — SDL3 window, input, drill flow
- `src/drill.c` — drill loading and spaced-repetition scheduling
- `assets/pieces/cburnett` — piece set by Colin M.L. Burnett (GPLv2+), via lichess
- `analysis/` — game analysis scripts (python-chess + Stockfish; `python3 -m venv .venv && .venv/bin/pip install chess`)
- `data/` — downloaded games (chess.com `hacktrick`, lichess `trincyolo`)
