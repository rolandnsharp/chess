"""Find threats in the player's games for the threat-spotting drill.

For a position after the opponent's move (player to move, not in check), let
the opponent move again: if their best free move wins clearly, that move is
the threat. Positions with no real threat are kept too; their answer is
"none". Cached per game in data/analysis/threats/.

    .venv/bin/python analysis/threats.py [--procs 2] [--nodes 60000]

Writes data/analysis/drills/threats.tsv:
fen_before, opponent's last move, answers (SANs or "none"), your game move, note
"""
import argparse, glob, json, math, multiprocessing as mp, os, random

import chess, chess.engine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "third_party/Stockfish/src/stockfish")
GAMES = os.path.join(ROOT, "data/analysis/games")
CACHE = os.path.join(ROOT, "data/analysis/threats")
OUT = os.path.join(ROOT, "data/analysis/drills/threats.tsv")
MATE = 10000

THREAT = 0.15   # a free move must raise their winning chances this much
ALSO = 0.05     # other free moves within this of the best also count
NONE = 0.04     # below this gain there is no threat


def wp(cp):
    return 1 / (1 + math.exp(-0.00368208 * max(-1500, min(1500, cp))))


_engine = None


def _init(nodes):
    global _engine, _limit
    _engine = chess.engine.SimpleEngine.popen_uci(ENGINE)
    _engine.configure({"Threads": 1, "Hash": 32})
    _limit = chess.engine.Limit(nodes=nodes)


def candidates(g):
    """Positions to test: every one before a blunder, plus a few others."""
    white = g["me_white"]
    plies = g["plies"]
    rng = random.Random(g["id"])
    out = []
    for i in range(12, min(len(plies), 90)):
        if (i % 2 == 0) != white:
            continue
        p = plies[i]
        if chess.Board(p["fen"]).is_check():
            continue
        e = p["eval"] if white else -p["eval"]
        a = p["eval_after"] if white else -p["eval_after"]
        if abs(e) > 600:  # already decided: threats matter less
            continue
        loss = wp(e) - wp(a)
        out.append((i, loss))
    blunders = [c for c in out if c[1] >= 0.2]
    others = [c for c in out if c[1] < 0.2]
    rng.shuffle(others)
    return blunders + others[:3]


def threat_of(fen, my_eval):
    """my_eval: centipawns for the player (to move). Returns (gain, answers)."""
    board = chess.Board(fen)
    board.turn = not board.turn
    board.ep_square = None
    if board.is_check() or not board.is_valid():
        return None
    infos = _engine.analyse(board, _limit, multipv=4)
    base = wp(-my_eval)  # their chances now
    scored = []
    for info in infos:
        if "pv" not in info or not info["pv"]:
            continue
        sc = info["score"].pov(board.turn).score(mate_score=MATE)
        scored.append((wp(sc), board.san(info["pv"][0])))
    if not scored:
        return None
    best = max(s for s, _ in scored)
    gain = best - base
    if gain < THREAT:
        return gain, []
    return gain, [san for s, san in scored if s >= best - ALSO and s - base >= THREAT - 0.03]


def do_game(path):
    gid = os.path.basename(path)[:-5]
    out = os.path.join(CACHE, gid + ".json")
    if os.path.exists(out):
        return json.load(open(out))
    g = json.load(open(path))
    white = g["me_white"]
    rows = []
    for i, loss in candidates(g):
        p = g["plies"][i]
        e = p["eval"] if white else -p["eval"]
        r = threat_of(p["fen"], e)
        if r is None:
            continue
        gain, answers = r
        rows.append({"fen_before": g["plies"][i - 1]["fen"], "last": g["plies"][i - 1]["san"],
                     "answers": answers, "gain": round(gain, 3), "played": p["san"],
                     "loss": round(loss, 3), "link": g["link"], "stamp": g["stamp"]})
    json.dump(rows, open(out + ".tmp", "w"))
    os.replace(out + ".tmp", out)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--procs", type=int, default=2)
    ap.add_argument("--nodes", type=int, default=60000)
    ap.add_argument("--cached", action="store_true", help="no engine: rebuild the file from cached games only")
    a = ap.parse_args()
    os.makedirs(CACHE, exist_ok=True)
    rows = []
    if a.cached:
        for f in glob.glob(os.path.join(CACHE, "*.json")):
            rows.extend(json.load(open(f)))
    else:
        files = sorted(glob.glob(os.path.join(GAMES, "*.json")))
        with mp.Pool(a.procs, initializer=_init, initargs=(a.nodes,)) as pool:
            for n, r in enumerate(pool.imap_unordered(do_game, files, chunksize=4), 1):
                rows.extend(r)
                if n % 50 == 0:
                    print(f"{n}/{len(files)} games", flush=True)
    write(rows)


def main_threats(answers):
    """The best free move, plus others only if they are captures or checks
    themselves (a quiet move that merely keeps the threat isn't one)."""
    return answers[:1] + [a for a in answers[1:] if "x" in a or "+" in a or "#" in a]


def write(rows):
    for r in rows:
        r["answers"] = main_threats(r["answers"])
    missed = [r for r in rows if r["answers"] and r["loss"] >= 0.2]
    threats = [r for r in rows if r["answers"] and r["loss"] < 0.2]
    calm = [r for r in rows if not r["answers"] and r["gain"] < NONE]
    random.seed(2)
    random.shuffle(threats)
    random.shuffle(calm)
    # about one in five boards has no threat, so "none" is a real answer
    picked = missed + threats[:max(0, 300 - len(missed))]
    picked += calm[: len(picked) // 4]
    random.shuffle(picked)
    clean = lambda x: str(x).replace("\t", " ").replace("\n", " ")
    with open(OUT, "w") as f:
        f.write("# fen_before\tlast_move\tanswers\tgame_move\tnote\n")
        for r in picked:
            if r["answers"]:
                note = (f"In the game you played {r['played']} and it cost you." if r["loss"] >= 0.2
                        else f"In the game you played {r['played']}.")
            else:
                note = f"Nothing serious was threatened. In the game you played {r['played']}."
            f.write("\t".join(clean(x) for x in (r["fen_before"], r["last"], " ".join(r["answers"]) or "none",
                                                 r["played"], note)) + "\n")
    print(f"threats.tsv: {len(picked)} positions - {len(missed)} threats you missed, "
          f"{min(len(threats), max(0, 300 - len(missed)))} other threats, {len(picked) - len(missed) - min(len(threats), max(0, 300 - len(missed)))} with none")


if __name__ == "__main__":
    main()
