"""Check the engine's duck perft against tests/duck_perft.epd.

Runs `go perft D` for every position and depth of the EPD (see
gen_duck_perft.py) and compares "Nodes searched". With --wakwak, a mismatch
also lists the root moves whose counts differ between the engine's divide and
wakwak's splitperft.

Usage: python tests/duck_perft_check.py ENGINE [--wakwak WAKWAK]
                                        [--max-depth N] [EPD]
ENGINE and WAKWAK should be absolute paths on Windows.
"""

import argparse
import os
import re
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("engine")
parser.add_argument("epd", nargs="?", default=os.path.join(os.path.dirname(__file__), "duck_perft.epd"))
parser.add_argument("--wakwak")
parser.add_argument("--max-depth", type=int, default=99)
args = parser.parse_args()


def is_chess960(fen):
    return any(c not in "KQkq-" for c in fen.split()[2])


def run(exe, fen, cmd):
    cmds = [f"setoption name UCI_Chess960 value {str(is_chess960(fen)).lower()}", f"position fen {fen}", cmd, "quit"]
    return subprocess.run([exe], input="\n".join(cmds) + "\n", capture_output=True, text=True).stdout


def divide(out):
    return {m: int(n) for m, n in re.findall(r"^([a-h][1-8][a-h][1-8][nbrq]?(?:,[a-h][1-8]{2}[a-h][1-8])?)\s*: (\d+)$", out, re.M)}


def show_diff(fen, depth):
    ours = divide(run(args.engine, fen, f"go perft {depth}"))
    ref = divide(run(args.wakwak, fen, f"splitperft {depth} true"))
    diffs = sorted(set(ours) | set(ref))
    diffs = [m for m in diffs if ours.get(m) != ref.get(m)]
    for m in diffs[:20]:
        print(f"       {m:<12} engine {ours.get(m, '-'):>10}  wakwak {ref.get(m, '-'):>10}")
    if len(diffs) > 20:
        print(f"       ... {len(diffs) - 20} more")


tests = []
with open(args.epd) as f:
    for line in f:
        if not line.strip() or line.startswith("#"):
            continue
        fen, *fields = [s.strip() for s in line.split(";")]
        for field in fields:
            depth, expected = re.match(r"D(\d+) (\d+)", field).groups()
            if int(depth) <= args.max_depth:
                tests.append((fen, int(depth), expected))

failed = 0
for fen, depth, expected in tests:
    start = time.time()
    match = re.search(r"Nodes searched: (\d+)", run(args.engine, fen, f"go perft {depth}"))
    got = match.group(1) if match else "none"

    ok = got == expected
    failed += not ok
    print(f"{'OK  ' if ok else 'FAIL'} depth {depth} {got:>11} (expected {expected:>11})"
          f" {time.time() - start:5.1f}s  {fen}", flush=True)
    if not ok and args.wakwak:
        show_diff(fen, depth)

print(f"{len(tests) - failed}/{len(tests)} passed")
sys.exit(1 if failed else 0)
