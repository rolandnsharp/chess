# Notation Trainer — notes for Claude

C chess trainer: every move is typed in standard notation (SAN). The player is
**hacktrick** on chess.com and **trincyolo** on lichess. Claude acts as their coach.

## Build and test

    make && make test       # test_chess also replays every move of data/*/all.pgn in strict SAN
    ./chess --screenshot out.png [--size WxH] [--moves "e4 e5"]   # render a frame, no window
    CHESS_SEED=5 SHOT_SCRIPT="/train;.;none;." ./chess --screenshot out.png
        # scripted input: ';'-separated, '.' = Enter; prints each message. Remove data/*_log.tsv
        # and data/progress.tsv after tests: they are the player's real records.

## Coaching loop ("review my games", "update my plan")

1. `make review` — fetches new games, Stockfish-analyses the newest 300 (cached), finds
   threats, rewrites `data/analysis/report.md` and the drill files in `data/analysis/drills/`.
2. Read `data/analysis/report.md` and the practice logs:
   - `data/train_log.tsv` — one line per /train step: date, step, planned, done, result
   - `data/scan_log.tsv` — checks & captures: found/total, wrong entries, seconds
   - `data/threat_log.tsv` — threat spotting: answer, guess, right?, seconds
   - `data/progress.tsv` — spaced-repetition boxes per drill
3. Rewrite `data/plan.txt` (format in `plans/default.txt`). Put the reasons in `note`
   lines, with the numbers they come from, and date the header. Weight steps toward
   what the newest games and logs show; drop what is fixed. Keep a session ~20 min.
4. Tell the player what changed and why.

`data/` is gitignored: games, analysis, plan and logs are personal and never pushed.
