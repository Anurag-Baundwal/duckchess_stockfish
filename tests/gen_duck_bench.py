"""Make the bench positions in src/benchmark.cpp with wakwak.

wakwak (github.com/Silverrzz/wakwak) makes every position: its genfens gives
random openings that its search at 1000 nodes scores within +-1000, and its
self-play from more of them gives the later positions. Half the openings are
standard and half double Fischer random (Duck960).

For each kind, the first OPENINGS openings go in as they are. Each of the
next GAMES is played on by wakwak at NODES nodes a move, and the position
after each ply in PLIES goes in, unless the game has ended by then or the side
to move can take a king. The output is the C++ list, with the Duck960
positions last, between UCI_Chess960 settings.

Usage: python tests/gen_duck_bench.py WAKWAK
WAKWAK should be an absolute path on Windows.
"""

import re
import subprocess
import sys

SEED = 20261121
OPENINGS = 3
GAMES = 4
PLIES = (24, 56, 96)
NODES = 20000


class Wakwak:
    def __init__(self, path):
        self.p = subprocess.Popen(
            [path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        self.send("uci")
        self.read_until("uciok")
        self.send("setoption name UCI_Variant value duck")

    def send(self, cmd):
        self.p.stdin.write(cmd + "\n")

    def read_until(self, prefix):
        lines = []
        while True:
            line = self.p.stdout.readline().strip()
            lines.append(line)
            if line.startswith(prefix):
                return lines

    def sync(self):
        self.send("isready")
        return self.read_until("readyok")

    def genfens(self, n, dfrc):
        self.send(f"genfens {n} seed {SEED} book None dfrc {str(dfrc).lower()} moves 8")
        lines = self.sync()
        return [l[len("info string genfens ") :] for l in lines if l.startswith("info string genfens ")]

    def fen(self, start, moves):
        self.send(f"position fen {start}" + (" moves " + " ".join(moves) if moves else ""))
        self.send("d")
        return next(l[5:] for l in self.sync() if l.startswith("FEN: "))

    def go(self, start, moves):
        self.send(f"position fen {start}" + (" moves " + " ".join(moves) if moves else ""))
        self.send(f"go nodes {NODES}")
        lines = self.read_until("bestmove")
        scores = [re.search(r"score (cp|mate) (-?\d+)", l) for l in lines]
        score = [m.groups() for m in scores if m][-1:]
        return lines[-1].split()[1], score[0] if score else None


def kings(fen):
    board = fen.split()[0]
    return "K" in board and "k" in board


def positions(w, dfrc):
    w.send(f"setoption name UCI_Chess960 value {str(dfrc).lower()}")
    w.send("ucinewgame")
    w.sync()
    starts = w.genfens(OPENINGS + GAMES, dfrc)
    result = starts[:OPENINGS]

    for start in starts[OPENINGS:]:
        w.send("ucinewgame")
        moves = []
        for ply in range(max(PLIES) + 1):
            fen = w.fen(start, moves)
            if not kings(fen):
                break
            move, score = w.go(start, moves)
            if ply in PLIES and not (score and score[0] == "mate" and int(score[1]) == 1):
                result.append(fen)
            if move in ("(none)", "0000"):
                break
            moves.append(move)
            print(f"{ply} {move} {score}", file=sys.stderr, flush=True)
    return result


w = Wakwak(sys.argv[1])
standard = positions(w, False)
dfrc = positions(w, True)
w.send("quit")

print('  "setoption name UCI_Chess960 value false",')
print('  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",')
for fen in standard:
    print(f'  "{fen}",')
print()
print("  // Duck960")
print('  "setoption name UCI_Chess960 value true",')
for fen in dfrc:
    print(f'  "{fen}",')
print('  "setoption name UCI_Chess960 value false"')
