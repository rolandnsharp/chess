"""Import a balanced subset of the lichess puzzle database (CC0).

    curl -o data/puzzles/lichess_db_puzzle.csv.zst https://database.lichess.org/lichess_db_puzzle.csv.zst
    python3 analysis/puzzles.py [--per 120] [--min 400] [--max 2600]

Keeps well-tested puzzles (low rating deviation, popular, often played) and
caps each motif at --per puzzles per 100-point rating band, so every theme has
puzzles at every level. Writes data/puzzles/lichess.tsv:
id, fen (before the opponent's move), moves (UCI; the first is the opponent's), rating, themes
"""
import argparse, collections, csv, io, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data/puzzles/lichess_db_puzzle.csv.zst")
OUT = os.path.join(ROOT, "data/puzzles/lichess.tsv")

# motifs worth drilling; phase/length tags are kept on each puzzle but not balanced on
MOTIFS = """hangingPiece fork pin skewer discoveredAttack doubleCheck mateIn1 mateIn2 mateIn3
backRankMate smotheredMate trappedPiece attraction deflection interference intermezzo clearance
capturingDefender defensiveMove quietMove advancedPawn promotion exposedKing kingsideAttack
sacrifice xRayAttack zugzwang rookEndgame pawnEndgame queenEndgame bishopEndgame knightEndgame""".split()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per", type=int, default=120, help="puzzles per motif per 100-point band")
    ap.add_argument("--min", type=int, default=400)
    ap.add_argument("--max", type=int, default=2600)
    a = ap.parse_args()
    count = collections.Counter()
    kept = 0
    seen = 0
    proc = subprocess.Popen(["zstdcat", SRC], stdout=subprocess.PIPE)
    reader = csv.reader(io.TextIOWrapper(proc.stdout, encoding="utf-8"))
    header = next(reader)
    col = {k: i for i, k in enumerate(header)}
    with open(OUT + ".tmp", "w") as out:
        out.write("# id\tfen\tmoves\trating\tthemes\n")
        for row in reader:
            seen += 1
            try:
                rating = int(row[col["Rating"]])
                rd = int(row[col["RatingDeviation"]])
                pop = int(row[col["Popularity"]])
                plays = int(row[col["NbPlays"]])
            except (ValueError, IndexError):
                continue
            if not (a.min <= rating < a.max) or rd > 90 or pop < 85 or plays < 300:
                continue
            themes = row[col["Themes"]].split()
            band = rating // 100
            wanted = [t for t in themes if t in MOTIFS and count[(t, band)] < a.per]
            if not wanted:
                continue
            for t in themes:
                if t in MOTIFS:
                    count[(t, band)] += 1
            out.write("\t".join((row[col["PuzzleId"]], row[col["FEN"]], row[col["Moves"]], str(rating),
                                 " ".join(themes))) + "\n")
            kept += 1
            if seen % 1000000 == 0:
                print(f"{seen // 1000000}M read, {kept} kept", flush=True)
    os.replace(OUT + ".tmp", OUT)
    per_motif = collections.Counter()
    for (t, _), c in count.items():
        per_motif[t] += c
    print(f"{seen} puzzles read, {kept} kept -> {OUT}")
    print("per motif:", ", ".join(f"{t} {per_motif[t]}" for t in MOTIFS))


if __name__ == "__main__":
    main()
