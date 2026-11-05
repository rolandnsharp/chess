# Notation Trainer

A chess board you play by typing standard notation (SAN) — `e4`, `Nf3`, `exd5`,
`Nbd7`, `O-O`, `e8=Q`. No mouse, so every move is visualised first.

## Build and run

    make            # builds ./chess and ./test_chess
    make test       # perft + notation tests + every move of the games in data/, if downloaded
    make stockfish  # clones and builds Stockfish in third_party/ (~10 min on ARM;
                    # set SF_ARCH=x86-64-avx2 etc. on other machines)
    ./chess [--black] [--level 1-10] [--fen FEN]

Needs SDL3, cairo, pango, librsvg, libcurl and ALSA. Without Stockfish the app uses its small
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
| `/train` (or `./chess train`) | today's guided session from your plan; `/skip` a step, `/stop` |
| `/puzzles [themes]` | lichess puzzles near your level (e.g. `fork`, `defence,quiet`); tracks a puzzle rating |
| `/review [win\|loss\|last]` | review one of your games yourself first (←/→ to step, type `Nd2 threat` at a move you'd change), then `/engine` |
| `/lichess` (or `./chess lichess`) | a rated **15+10** game on lichess, moves typed as always; `/resign`, `/draw`, `/abort`, `/claim` |
| `/threats` | what does your opponent's last move threaten? type it, or `none` |
| `/scan [captures\|checks]` | type every check and capture, yours then your opponent's; scored against the legal-move list |
| `/sound [on\|off]` (or `--mute`) | sound effects: moves, captures, checks, right/wrong answers, game start and end |
| `/flip`, `/fen`, `/pgn`, `/resign`, `/help`, `/quit` | |

Up/Down recall earlier input; Esc clears. Finished games are saved to `games/`.

Moves that are legal but written non-standardly (`Ng1f3`, `0-0`, `a8Q`, a missing `x`)
are played, and the standard form is shown. Refusals say why: ambiguous, pinned,
king would be in check, pawn captures need their file, and so on.

## Drills from your games

    python3 -m venv .venv && .venv/bin/pip install chess
    .venv/bin/python analysis/analyse.py --games 1000   # Stockfish over your recent games (resumable)
    .venv/bin/python analysis/threats.py               # threat-spotting positions
    .venv/bin/python analysis/report.py                 # data/analysis/report.md + drills.tsv
    make update                                         # all of the above, after fetching new games

Games are read from `data/chesscom/all.pgn` and `data/lichess/all.pgn`; set the
player names in `ME` in `analysis/analyse.py`. Four kinds of drill:

- **puzzles**: positions where you blundered; find the better move. Stockfish
  accepts any move that is as good as the solution.
- **punish**: your opponent blundered and you missed it.
- **lines**: your opening repertoire as you actually play it, against the replies
  you actually meet, with costly moves replaced by the engine's choice.
- **endgames**: winning endgames you didn't win; win them against full-strength Stockfish.

**Checks and captures** (`/scan`) uses positions from your games (those just
before one of your blunders first). List every check and capture for your side,
then for your opponent's, which is the threat scan that catches hung pieces.
Results go to `data/scan_log.tsv`, so you can watch the score and time improve.

Spaced repetition: a drill you get right comes back after 1, 3, 7, 21, then 60
days; a miss comes back later in the same session. Progress is in `data/progress.tsv`.

## Lichess puzzles

    curl -o data/puzzles/lichess_db_puzzle.csv.zst https://database.lichess.org/lichess_db_puzzle.csv.zst
    python3 analysis/puzzles.py      # ~58k well-tested puzzles, balanced by motif and rating

Puzzles are picked about 300 points below your puzzle rating, aiming for about
85% first-try success (see `docs/improving.md`). Missed ones come back the next
day and then on a spaced schedule. The puzzle database is CC0, from lichess.org.

## Live lichess games

One time control only: rated 15+10 rapid, slow enough to check the opponent's
threats on every move. First time: `/lichess` copies the token page link to
your clipboard; create a token with the "Play games with the board API"
permission, then type `/token ` and paste it (Ctrl+V). It is saved in
`data/lichess_token`, readable only by you. During a live game the engine,
drills and hints are disabled (only `/resign /draw /abort /claim /flip /coords`
work), so the app stays within lichess's fair-play rules. Your finished games
come into the analysis with `make update`.

## Training plan

`/train` walks through `data/plan.txt` step by step (falls back to
`plans/default.txt`), then logs each step's result to `data/train_log.tsv`.
The plan is meant to be rewritten by a coach (Claude, see `CLAUDE.md`) after
`make update` fetches and analyses your newest games.

## Layout

- `src/chess.[ch]` — rules, SAN parse/print, FEN, game state (no dependencies)
- `src/engine.c` — Stockfish over UCI; `src/ai.c` — fallback alpha-beta engine
- `src/render.c` — cairo/pango drawing; `src/main.c` — SDL3 window, input, drill flow
- `src/drill.c` — drill loading and spaced-repetition scheduling
- `src/puzzles.c` — lichess puzzle selection, puzzle rating, reviews
- `src/sound.c` — synthesised sound effects over ALSA (the card is only opened while a sound plays; `CHESS_SOUND_DEVICE` overrides the device, `CHESS_SOUND=0` mutes)
- `src/lichess.c`, `src/json.c` — lichess Board API over libcurl (threaded streams), small JSON reader
- `docs/improving.md` — what the research says about improving, and how the trainer applies it
- `assets/pieces/cburnett` — piece set by Colin M.L. Burnett (GPLv2+), via lichess
- `analysis/` — game analysis scripts (python-chess + Stockfish; `python3 -m venv .venv && .venv/bin/pip install chess`)
- `data/` — downloaded games (chess.com `hacktrick`, lichess `trincyolo`)
