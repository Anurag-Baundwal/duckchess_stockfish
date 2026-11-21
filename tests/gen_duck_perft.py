"""Fill in the perft counts of tests/duck_perft.epd with wakwak's splitperft.

wakwak (github.com/Silverrzz/wakwak) is the reference. Its rules match
fastchess, which adjudicates duck chess games. Its perft counts a terminal
position (the side to move has no king or no chess move, or the halfmove
clock is at least 100) as one node; splitperft, like our `go perft`, does
not apply this to the root.

Each position line is "<fen> ;D1 <n> ;D2 <n> ...". The depths already on a
line are recomputed, and a line without depths gets D1-D3. Lines starting
with "#" are comments. A castling field other than KQkq is Shredder-FEN and
runs with UCI_Chess960 true.

Usage: python tests/gen_duck_perft.py WAKWAK [EPD]
WAKWAK should be an absolute path on Windows.
"""

import os
import re
import subprocess
import sys

wakwak = sys.argv[1]
epd = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "duck_perft.epd")


def is_chess960(fen):
    return any(c not in "KQkq-" for c in fen.split()[2])


def splitperft(fen, depths):
    cmds = [f"setoption name UCI_Chess960 value {str(is_chess960(fen)).lower()}", f"position fen {fen}"]
    cmds += [f"splitperft {d} true" for d in depths] + ["quit"]
    out = subprocess.run(
        [wakwak], input="\n".join(cmds) + "\n", capture_output=True, text=True
    ).stdout
    counts = dict(re.findall(r"info string splitperft depth (\d+) nodes (\d+)", out))
    return [counts[str(d)] for d in depths]


lines = []
with open(epd) as f:
    for line in f:
        line = line.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            lines.append(line)
            continue

        fen, *fields = [s.strip() for s in line.split(";")]
        depths = [int(re.match(r"D(\d+)", s).group(1)) for s in fields] or [1, 2, 3]
        counts = splitperft(fen, depths)
        lines.append(fen + "".join(f" ;D{d} {n}" for d, n in zip(depths, counts)))
        print(lines[-1], flush=True)

with open(epd, "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
