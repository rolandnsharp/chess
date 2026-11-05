"""Turn analysed games (data/analysis/games/*.json) into a report and drills.

    .venv/bin/python analysis/report.py

Writes data/analysis/report.md and data/analysis/drills/{tactics,openings,endgames}.json
"""
import collections, glob, json, math, os, statistics

import chess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = os.path.join(ROOT, "data/analysis/games")
OUT = os.path.join(ROOT, "data/analysis")
MATE = 10000
VAL = {chess.PAWN: 1, chess.KNIGHT: 3, chess.BISHOP: 3, chess.ROOK: 5, chess.QUEEN: 9, chess.KING: 0}


def wp(cp):
    """Win probability for White from centipawns (lichess's curve)."""
    return 1 / (1 + math.exp(-0.00368208 * max(-1500, min(1500, cp))))


def mine(cp, white):
    return cp if white else -cp


def phase(board, ply):
    pieces = sum(1 for p in board.piece_map().values() if p.piece_type not in (chess.PAWN, chess.KING))
    if pieces <= 6:
        return "endgame"
    return "opening" if ply < 20 else "middlegame"


def material_sig(board):
    def side(c):
        s = "".join(chess.piece_symbol(t).upper() * len(board.pieces(t, c))
                    for t in (chess.QUEEN, chess.ROOK, chess.BISHOP, chess.KNIGHT))
        return (s or "K") + (f"+{len(board.pieces(chess.PAWN, c))}p" if board.pieces(chess.PAWN, c) else "")
    return side(chess.WHITE), side(chess.BLACK)


def seconds(tc):
    try:
        return int(tc.split("+")[0])
    except (ValueError, AttributeError):
        return None


def classify(p, nxt, white):
    """Rough cause of a big mistake by the player."""
    before = mine(p["eval"], white)
    after = mine(p["eval_after"], white)
    if before >= MATE - 100:
        return "missed mate"
    if after <= -(MATE - 100):
        return "allowed mate"
    board = chess.Board(p["fen"])
    if p["best_san"] and "x" in p["best_san"]:
        best = chess.Move.from_uci(p["best"])
        victim = board.piece_at(best.to_square)
        if victim and VAL[victim.piece_type] >= 3:
            return "missed capture"
    if nxt and nxt.get("best_san") and "x" in nxt["best_san"]:
        b2 = chess.Board(nxt["fen"])
        reply = chess.Move.from_uci(nxt["best"])
        victim = b2.piece_at(reply.to_square)
        if victim and VAL[victim.piece_type] >= 3:
            return "hung a piece"
        return "allowed a tactic"
    if p["best_san"] and ("+" in p["best_san"]):
        return "missed a check"
    return "other"


MIN_GAMES = 4      # follow an opponent reply seen at least this often
KEEP_LOSS = 0.06   # keep your own move if it costs less than this on average...
KEEP_EARLY = 0.15  # ...or this in your first three moves: those are your opening
                   # choices (a gambit is a choice, not a mistake)


def repertoire(book):
    """Lines to memorise: your moves (corrected where they cost you) against
    the replies you actually meet. Each leaf becomes one drill."""
    out = []
    for colour in (True, False):
        nodes = book[colour]

        def emit(key, fixed, games):
            moves = key.split()
            mine_n = sum(1 for i in range(len(moves)) if (i % 2 == 0) == colour)
            last_mine = bool(moves) and ((len(moves) - 1) % 2 == 0) == colour
            if last_mine and mine_n >= 3:
                out.append({"as": "white" if colour else "black", "moves": moves,
                            "games": games, "fixed": fixed})

        def walk(key, ply, fixed, games):
            node = nodes.get(key)
            if node is None or ply >= 24:
                return emit(key, fixed, games)
            mv = node["moves"]
            if (ply % 2 == 0) == colour:  # your move
                common = max(mv.items(), key=lambda kv: len(kv[1]))[0]
                avg = statistics.mean(mv[common])
                limit = KEEP_EARLY if ply < 6 else KEEP_LOSS
                if avg < limit or common == node["best"] or not node["best"]:
                    choice, fix = common, fixed
                else:
                    choice, fix = node["best"], fixed + [(ply, common, node["best"])]
                nxt = (key + " " + choice).strip()
                if choice != common or nxt not in nodes:
                    return emit(nxt, fix, games)  # off your own map: the line ends here
                return walk(nxt, ply + 1, fix, len(mv[choice]))
            replies = [(san, len(v)) for san, v in mv.items() if len(v) >= MIN_GAMES]
            if not replies:
                return emit(key, fixed, games)
            for san, cnt in sorted(replies, key=lambda kv: -kv[1]):
                walk((key + " " + san).strip(), ply + 1, fixed, cnt)

        walk("", 0, [], 0)
    return out


def date_of(stamp):
    import datetime
    try:
        return datetime.date.fromisoformat(stamp[:10]).strftime("%-d %b %Y")
    except ValueError:
        return stamp[:10]


def solution_line(fen, pv, first_san):
    """SAN line for a puzzle: one move, or three plies if the first is forcing."""
    board = chess.Board(fen)
    sans = []
    for u in pv[:3]:
        m = chess.Move.from_uci(u)
        if m not in board.legal_moves:
            break
        sans.append(board.san(m))
        board.push(m)
    if not sans:
        return [first_san]
    if len(sans) == 3 and ("x" in sans[0] or "+" in sans[0]):
        return sans
    return sans[:1]


def write_tsv(tactics, unpunished, lines_out, thrown):
    """Drill file for the C app. Columns: kind, fen, solution (SANs), title,
    link, info (shown once the drill is answered)."""
    clean = lambda x: str(x).replace("\t", " ").replace("\n", " ")
    rows = []
    for t in tactics:
        rows.append(("tactic", t["fen"], " ".join(solution_line(t["fen"], t["pv"], t["best"])),
                     f"{date_of(t['stamp'])}, {t['speed']}: you played {t['played']} here and it cost you. Find the better move.",
                     t["link"], f"{t['kind']}; it cost you {t['loss']:.0%} winning chances"))
    for t in unpunished:
        rows.append(("punish", t["fen"], " ".join(solution_line(t["fen"], t["pv"], t["best"])),
                     f"{date_of(t['stamp'])}, {t['speed']}: your opponent just played {t['their_move']}, a blunder. Punish it.",
                     t["link"], f"in the game you played {t['played']}"))
    for l in lines_out:
        fix = "; ".join(f"move {p // 2 + 1}: {b} instead of {c}" for p, c, b in l["fixed"])
        rows.append(("line", chess.STARTING_FEN, " ".join(l["moves"]),
                     f"Your {l['as'].title()} repertoire, reached in {l['games']} of your games. Play your moves.",
                     "", ("Corrected: " + fix) if fix else "This is what you already play."))
    for t in thrown:
        rows.append(("endgame", t["fen"], "",
                     f"{date_of(t['stamp'])}: you had this won and didn't win it. Win it against full-strength Stockfish.",
                     t["link"], f"evaluation at the start {t['eval'] / 100:+.1f}"))
    with open(os.path.join(OUT, "drills", "drills.tsv"), "w") as f:
        f.write("# kind\tfen\tsolution\ttitle\tlink\tinfo\n")
        for r in rows:
            f.write("\t".join(clean(x) for x in r) + "\n")


def main():
    games = [json.load(open(f)) for f in glob.glob(os.path.join(GAMES, "*.json"))]
    games.sort(key=lambda g: g["stamp"])
    if not games:
        print("no analysed games yet")
        return
    moves = []           # every move the player made
    tactics = []         # puzzle candidates from the player's own games
    unpunished = []      # opponent blunders the player didn't exploit
    thrown = []          # winning endgames not won
    saved = []
    tree = {True: collections.defaultdict(lambda: collections.defaultdict(list)),
            False: collections.defaultdict(lambda: collections.defaultdict(list))}
    # book[colour][line] = {"fen", "best", "moves": {san: [loss or None]}}
    book = {True: {}, False: {}}
    results = {True: [], False: []}
    by_first = {True: collections.defaultdict(list), False: collections.defaultdict(list)}

    for g in games:
        white = g["me_white"]
        res = g["result"]
        score = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}.get(res)
        if score is None:
            continue
        if not white:
            score = 1 - score
        results[white].append(score)
        total = seconds(g["time_control"])
        plies = g["plies"]
        first = " ".join(p["san"] for p in plies[:4])
        by_first[white][first].append(score)
        endgame_seen = False
        line = []
        for i, p in enumerate(plies):
            my_turn = (i % 2 == 0) == white
            board = chess.Board(p["fen"])
            ph = phase(board, i)
            key = " ".join(line)
            line.append(p["san"])
            if ph == "endgame" and not endgame_seen:
                endgame_seen = True
                ev = mine(p["eval"], white)
                rec = {"fen": p["fen"], "eval": ev, "link": g["link"], "sig": material_sig(board),
                       "me_white": white, "score": score, "stamp": g["stamp"]}
                if wp(ev) >= 0.8 and score < 1:
                    thrown.append(rec)
                elif wp(ev) <= 0.2 and score > 0:
                    saved.append(rec)
            if i < 16:
                tree[white][key]["_reached"].append(score)
            if i < 24:
                node = book[white].setdefault(key, {"fen": p["fen"], "best": p["best_san"], "moves": collections.defaultdict(list)})
                node["moves"][p["san"]].append(
                    max(0.0, wp(mine(p["eval"], white)) - wp(mine(p["eval_after"], white))) if my_turn else None)
            before = wp(mine(p["eval"], white))
            after = wp(mine(p["eval_after"], white))
            if not my_turn:
                # opponent blundered; did the player cash in on the next move?
                if after - before >= 0.3 and i + 1 < len(plies):
                    q = plies[i + 1]
                    q_loss = wp(mine(q["eval"], white)) - wp(mine(q["eval_after"], white))
                    if q_loss >= 0.2 and q["best_san"]:
                        unpunished.append({"fen": q["fen"], "played": q["san"], "best": q["best_san"], "their_move": p["san"], "stamp": g["stamp"], "speed": g["speed"],
                                           "pv": q["pv"], "loss": q_loss, "link": g["link"], "phase": phase(chess.Board(q["fen"]), i + 1)})
                continue
            loss = max(0.0, before - after)
            clock = p.get("clock")
            frac = clock / total if clock is not None and total else None
            m = {"loss": loss, "phase": ph, "speed": g["speed"], "clock_frac": frac, "game": g["id"]}
            if i < 16:
                tree[white][key][p["san"]].append(loss)
                tree[white][key]["_best"] = p["best_san"]
                tree[white][key]["_fen"] = p["fen"]
            if loss >= 0.3:
                m["kind"] = classify(p, plies[i + 1] if i + 1 < len(plies) else None, white)
                if p["best_san"] and abs(p["eval"]) < MATE * 2:
                    tactics.append({"fen": p["fen"], "played": p["san"], "best": p["best_san"], "pv": p["pv"], "stamp": g["stamp"], "speed": g["speed"],
                                    "loss": round(loss, 3), "kind": m["kind"], "phase": ph, "link": g["link"],
                                    "eval_best": mine(p["eval"], white)})
            moves.append(m)

    n = len(games)
    lines = []
    w = lines.append
    w(f"# Game analysis: {n} games, {games[0]['stamp'][:10]} to {games[-1]['stamp'][:10]}\n")
    sp = collections.Counter(g["speed"] for g in games)
    w(f"Speeds: {dict(sp)}. Score as White {statistics.mean(results[True]):.0%} ({len(results[True])} games), "
      f"as Black {statistics.mean(results[False]):.0%} ({len(results[False])} games).\n")

    w("## Mistakes by phase\n")
    w("Per move you played. Loss = drop in your winning chances. Inaccuracy ≥10%, mistake ≥20%, blunder ≥30%.\n")
    w("| phase | your moves | avg loss | inaccuracies | mistakes | blunders | blunders per 100 moves |")
    w("|---|---|---|---|---|---|---|")
    for ph in ("opening", "middlegame", "endgame"):
        ms = [m for m in moves if m["phase"] == ph]
        if not ms:
            continue
        inacc = sum(1 for m in ms if 0.1 <= m["loss"] < 0.2)
        mist = sum(1 for m in ms if 0.2 <= m["loss"] < 0.3)
        blun = sum(1 for m in ms if m["loss"] >= 0.3)
        w(f"| {ph} | {len(ms)} | {statistics.mean(m['loss'] for m in ms):.1%} | {inacc} | {mist} | {blun} | {100*blun/len(ms):.1f} |")
    w("")

    blunders = [m for m in moves if m["loss"] >= 0.3]
    w("## What the blunders were\n")
    kinds = collections.Counter(m["kind"] for m in blunders)
    for k, c in kinds.most_common():
        w(f"- **{k}**: {c} ({c/len(blunders):.0%})")
    w(f"\nGames analysed: {n}; blunders per game: {len(blunders)/n:.1f}. "
      f"Opponent blunders you didn't punish: {len(unpunished)} ({len(unpunished)/n:.1f} per game).\n")

    timed = [m for m in moves if m["clock_frac"] is not None]
    if timed:
        w("## Clock\n")
        w("| time left | your moves | blunders per 100 moves |")
        w("|---|---|---|")
        for lo, hi, lab in ((0.5, 9, "over half"), (0.2, 0.5, "20–50%"), (0.1, 0.2, "10–20%"), (0, 0.1, "under 10%")):
            ms = [m for m in timed if lo <= m["clock_frac"] < hi]
            if ms:
                w(f"| {lab} | {len(ms)} | {100*sum(1 for m in ms if m['loss'] >= 0.3)/len(ms):.1f} |")
        w("")

    w("## Openings\n")
    for colour in (True, False):
        w(f"### As {'White' if colour else 'Black'}: most common starts\n")
        w("| first moves | games | your score |")
        w("|---|---|---|")
        for k, s in sorted(by_first[colour].items(), key=lambda kv: -len(kv[1]))[:8]:
            w(f"| {k} | {len(s)} | {statistics.mean(s):.0%} |")
        w("")
    w("### Opening leaks: positions you reach often where your usual move costs you\n")
    w("| as | after | times | you usually play | avg loss | engine prefers |")
    w("|---|---|---|---|---|---|")
    leaks = []
    for colour in (True, False):
        for key, node in tree[colour].items():
            mv = {k: v for k, v in node.items() if not k.startswith("_")}
            total_n = sum(len(v) for v in mv.values())
            if total_n < 6:
                continue
            common, losses = max(mv.items(), key=lambda kv: len(kv[1]))
            avg = statistics.mean(losses)
            if avg >= 0.05 and common != node.get("_best") and len(key.split()) >= 4:
                leaks.append((avg * len(losses), colour, key, total_n, common, avg, node.get("_best"), node.get("_fen")))
    leaks.sort(reverse=True)
    opening_drills = []
    for _, colour, key, total_n, common, avg, best, fen in leaks[:15]:
        w(f"| {'W' if colour else 'B'} | {key or '(start)'} | {total_n} | {common} | {avg:.0%} | {best} |")
        opening_drills.append({"as": "white" if colour else "black", "moves": key, "fen": fen,
                               "you_play": common, "best": best, "times": total_n, "avg_loss": round(avg, 3)})
    w("")

    w("## Endgames\n")
    w(f"Winning endgames (≥80% to win at the start of the endgame) not won: **{len(thrown)}**. "
      f"Lost endgames you saved or won: {len(saved)}.\n")
    sigs = collections.Counter()
    for t in thrown:
        a, b = t["sig"]
        mine_s, theirs = (a, b) if t["me_white"] else (b, a)
        strip = lambda s: s.split("+")[0]
        sigs[f"{strip(mine_s)} vs {strip(theirs)}"] += 1
    if sigs:
        w("Thrown, by pieces (yours vs theirs, pawns not shown): " +
          ", ".join(f"{k} ×{c}" for k, c in sigs.most_common(8)) + "\n")

    w("## Puzzles from your own games\n")
    w(f"{len(tactics)} positions where you blundered and a better move existed, "
      f"plus {len(unpunished)} where the opponent blundered and you missed the punishment.\n")
    tk = collections.Counter(t["kind"] for t in tactics)
    w("Themes: " + ", ".join(f"{k} ×{c}" for k, c in tk.most_common()) + "\n")

    os.makedirs(os.path.join(OUT, "drills"), exist_ok=True)
    tactics.sort(key=lambda t: -t["loss"])
    unpunished.sort(key=lambda t: -t["loss"])
    json.dump({"own_blunders": tactics, "missed_punishments": unpunished},
              open(os.path.join(OUT, "drills/tactics.json"), "w"), indent=1)
    json.dump(opening_drills, open(os.path.join(OUT, "drills/openings.json"), "w"), indent=1)
    json.dump({"thrown": thrown, "saved": saved}, open(os.path.join(OUT, "drills/endgames.json"), "w"), indent=1)
    lines_out = repertoire(book)
    w("## Repertoire lines to drill\n")
    for colour in ("white", "black"):
        ls = [l for l in lines_out if l["as"] == colour]
        w(f"As {colour.title()}: {len(ls)} lines. "
          f"{sum(1 for l in ls if l['fixed'])} include a correction to what you usually play.\n")
    write_tsv(tactics, unpunished, lines_out, thrown)
    open(os.path.join(OUT, "report.md"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
