"""Evaluate every position of the player's recent games with Stockfish.

Writes one JSON file per game to data/analysis/games/, so runs can be stopped
and resumed. report.py turns these into findings and drills.

    .venv/bin/python analysis/analyse.py [--games 1500] [--nodes 40000] [--procs 4]
"""
import argparse, hashlib, json, multiprocessing as mp, os, re, sys, time

import chess, chess.engine, chess.pgn

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "third_party/Stockfish/src/stockfish")
OUT = os.path.join(ROOT, "data/analysis/games")
SOURCES = [("data/chesscom/all.pgn", "chesscom"), ("data/lichess/all.pgn", "lichess")]
ME = {"hacktrick", "trincyolo"}
MATE = 10000


def speed_of(tc):
    m = re.match(r"^(\d+)(?:\+(\d+))?$", tc or "")
    if not m:
        return "daily"
    est = int(m.group(1)) + 40 * int(m.group(2) or 0)
    return "bullet" if est < 180 else "blitz" if est < 600 else "rapid" if est <= 3600 else "classical"


def load_games(speeds):
    """Yield (id, meta, movetext-pgn) for standard games, newest last."""
    found = []
    for rel, site in SOURCES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            while True:
                offset = f.tell()
                headers = chess.pgn.read_headers(f)
                if headers is None:
                    break
                if headers.get("Variant", "Standard") != "Standard" or "960" in headers.get("Event", ""):
                    continue
                if headers.get("SetUp") == "1":
                    continue
                white = headers.get("White", "").lower() in ME
                black = headers.get("Black", "").lower() in ME
                if white == black:
                    continue
                speed = speed_of(headers.get("TimeControl"))
                if speed not in speeds:
                    continue
                date = headers.get("UTCDate", headers.get("Date", "")).replace(".", "-")
                tm = headers.get("UTCTime", headers.get("StartTime", "00:00:00"))
                link = headers.get("Link") or headers.get("Site", "")
                gid = hashlib.sha1(f"{site}|{link}|{date}|{tm}|{headers.get('White')}".encode()).hexdigest()[:16]
                found.append((f"{date} {tm}", gid, rel, offset, site, speed))
    found.sort()
    return found


_engine = None


def _init(nodes):
    global _engine, _limit
    _engine = chess.engine.SimpleEngine.popen_uci(ENGINE)
    _engine.configure({"Threads": 1, "Hash": 32})
    _limit = chess.engine.Limit(nodes=nodes)


_cache = {}


def evaluate(board):
    """White-relative centipawns (mates as +-MATE), best move UCI, and PV."""
    if board.is_checkmate():
        return (-MATE if board.turn == chess.WHITE else MATE), None, []
    if board.is_game_over(claim_draw=False):
        return 0, None, []
    key = board.epd()
    if key in _cache:
        return _cache[key]
    info = _engine.analyse(board, _limit)
    score = info["score"].white().score(mate_score=MATE)
    pv = [m.uci() for m in info.get("pv", [])[:8]]
    res = (score, pv[0] if pv else None, pv)
    if len(_cache) > 200000:
        _cache.clear()
    _cache[key] = res
    return res


def analyse_one(job):
    stamp, gid, rel, offset, site, speed = job
    out = os.path.join(OUT, gid + ".json")
    if os.path.exists(out):
        return gid, "cached"
    with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
        f.seek(offset)
        game = chess.pgn.read_game(f)
    h = game.headers
    me_white = h.get("White", "").lower() in ME
    board = game.board()
    plies = []
    node = game
    score, best, pv = evaluate(board)
    while node.variations:
        nxt = node.variations[0]
        move = nxt.move
        san = board.san(move)
        best_san = board.san(chess.Move.from_uci(best)) if best else None
        entry = {"fen": board.fen(), "san": san, "uci": move.uci(), "eval": score,
                 "best": best, "best_san": best_san, "pv": pv, "clock": nxt.clock()}
        board.push(move)
        score, best, pv = evaluate(board)
        entry["eval_after"] = score
        plies.append(entry)
        node = nxt
    rec = {
        "id": gid, "site": site, "speed": speed, "stamp": stamp,
        "link": h.get("Link") or h.get("Site", ""), "white": h.get("White"), "black": h.get("Black"),
        "me_white": me_white, "result": h.get("Result"), "termination": h.get("Termination", ""),
        "my_elo": h.get("WhiteElo" if me_white else "BlackElo"),
        "opp_elo": h.get("BlackElo" if me_white else "WhiteElo"),
        "time_control": h.get("TimeControl"), "opening": h.get("Opening") or h.get("ECOUrl", "").rsplit("/", 1)[-1],
        "plies": plies,
    }
    tmp = out + ".tmp"
    with open(tmp, "w") as f:
        json.dump(rec, f)
    os.replace(tmp, out)
    return gid, f"{len(plies)} plies"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", type=int, default=1500, help="most recent N games")
    ap.add_argument("--nodes", type=int, default=40000)
    ap.add_argument("--procs", type=int, default=4)
    ap.add_argument("--speeds", default="rapid,blitz")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    jobs = load_games(set(a.speeds.split(",")))[-a.games:]
    jobs.reverse()  # newest first, so partial runs cover recent play
    print(f"{len(jobs)} games to analyse ({jobs[-1][0]} .. {jobs[0][0]})", flush=True)
    t0 = time.time()
    done = 0
    with mp.Pool(a.procs, initializer=_init, initargs=(a.nodes,)) as pool:
        for gid, what in pool.imap_unordered(analyse_one, jobs, chunksize=2):
            done += 1
            if done % 25 == 0 or done == len(jobs):
                el = time.time() - t0
                print(f"{done}/{len(jobs)}  {el/60:.1f} min elapsed", flush=True)


if __name__ == "__main__":
    main()
