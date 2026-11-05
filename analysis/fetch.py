"""Download the player's games from chess.com and lichess.

chess.com: one PGN per month in data/chesscom/; only the last two months are
fetched again unless --all. lichess: everything, into data/lichess/all.pgn.

    python3 analysis/fetch.py [--all]
"""
import argparse, json, os, sys, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHESSCOM = "hacktrick"
LICHESS = "trincyolo"
UA = {"User-Agent": "notation-trainer/0.1 (personal use)"}


def get(url, accept=None):
    h = dict(UA)
    if accept:
        h["Accept"] = accept
    with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=120) as r:
        return r.read()


def chesscom(all_months):
    d = os.path.join(ROOT, "data/chesscom")
    os.makedirs(d, exist_ok=True)
    archives = json.loads(get(f"https://api.chess.com/pub/player/{CHESSCOM}/games/archives"))["archives"]
    todo = archives if all_months else archives[-2:]
    for url in todo:
        y, m = url.rstrip("/").split("/")[-2:]
        path = os.path.join(d, f"{y}-{m}.pgn")
        if all_months or url in archives[-2:] or not os.path.exists(path):
            open(path, "wb").write(get(url + "/pgn"))
            time.sleep(0.3)
    months = sorted(f for f in os.listdir(d) if f[:4].isdigit() and f.endswith(".pgn"))
    with open(os.path.join(d, "all.pgn"), "wb") as out:
        for f in months:
            out.write(open(os.path.join(d, f), "rb").read().strip() + b"\n\n")
    return sum(open(os.path.join(d, f), encoding="utf-8", errors="replace").read().count("[Event ") for f in months)


def lichess():
    d = os.path.join(ROOT, "data/lichess")
    os.makedirs(d, exist_ok=True)
    data = get(f"https://lichess.org/api/games/user/{LICHESS}?opening=true&clocks=true", "application/x-chess-pgn")
    open(os.path.join(d, "all.pgn"), "wb").write(data)
    return data.count(b"[Event ")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true", help="re-fetch every chess.com month")
    a = ap.parse_args()
    print(f"chess.com {CHESSCOM}: {chesscom(a.all)} games")
    print(f"lichess {LICHESS}: {lichess()} games")
