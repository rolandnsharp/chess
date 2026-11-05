"""Summarise games from PGN headers only: ratings, time controls, openings."""
import re, sys, collections

ME = {"hacktrick", "trincyolo"}

def games(path, site):
    tags = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r'\[(\w+) "(.*)"\]', line)
        if m:
            tags[m.group(1)] = m.group(2)
        elif line.strip() and tags:
            if not line.startswith("[") and "_done" not in tags:
                tags["_done"] = 1
                tags["_site"] = site
                yield tags
                tags = {}

def opening(t):
    if t.get("Opening"):
        return t["Opening"]
    u = t.get("ECOUrl", "")
    return u.rsplit("/", 1)[-1].replace("-", " ") if u else "?"

def family(name):
    return re.split(r"[:,]", name)[0].strip()

def main():
    rows = []
    for path, site in [("data/chesscom/all.pgn", "chesscom"), ("data/lichess/all.pgn", "lichess")]:
        for t in games(path, site):
            if t.get("Variant", "Standard") != "Standard" or "960" in t.get("Event", ""):
                continue
            white = t.get("White", "").lower() in ME
            res = t.get("Result", "*")
            score = {"1-0": 1, "0-1": 0, "1/2-1/2": 0.5}.get(res)
            if score is None:
                continue
            if not white:
                score = 1 - score
            elo = t.get("WhiteElo" if white else "BlackElo", "?")
            tc = t.get("TimeControl", "?")
            base = int(tc.split("+")[0]) if re.match(r"^\d+(\+\d+)?$", tc) else 0
            speed = "bullet" if base < 180 else "blitz" if base < 600 else "rapid" if base <= 3600 else "daily"
            if "/" in tc: speed = "daily"
            rows.append(dict(site=site, white=white, score=score, elo=elo, speed=speed,
                             date=t.get("UTCDate", t.get("Date", "?")), op=opening(t)))
    print(f"{len(rows)} standard games")
    by = collections.defaultdict(list)
    for r in rows:
        by[(r["site"], r["speed"])].append(r)
    print("\nsite/speed: games, score, first and latest rating, date range")
    for k, v in sorted(by.items(), key=lambda kv: -len(kv[1])):
        v.sort(key=lambda r: r["date"])
        print(f"  {k[0]:8} {k[1]:7} {len(v):5}  {sum(r['score'] for r in v)/len(v):.0%}  "
              f"{v[0]['elo']:>5} -> {v[-1]['elo']:>5}   {v[0]['date']} .. {v[-1]['date']}")
    per_year = collections.Counter(r["date"][:4] for r in rows)
    print("\ngames per year:", dict(sorted(per_year.items())))

    recent = [r for r in rows if r["date"] >= "2024"]
    print(f"\nopening families since 2024 ({len(recent)} games), by colour: games, score")
    for colour in (True, False):
        c = collections.defaultdict(list)
        for r in recent:
            if r["white"] == colour:
                c[family(r["op"])].append(r["score"])
        print(f"  as {'White' if colour else 'Black'}:")
        for name, s in sorted(c.items(), key=lambda kv: -len(kv[1]))[:12]:
            print(f"    {len(s):4}  {sum(s)/len(s):4.0%}  {name}")

main()
