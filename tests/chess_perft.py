"""Run the perft tests of tests/perft.sh without `expect`.

perft.sh drives the engine through an expect script, which is not available
in every environment (e.g. Git Bash on Windows). This reads its run_test lines
and checks each "Nodes searched" count directly.

Usage: python tests/chess_perft.py ENGINE [PERFT_SH]
ENGINE should be an absolute path on Windows.
"""

import os
import re
import subprocess
import sys
import time

engine = sys.argv[1]
script = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "perft.sh")

with open(script) as f:
    tests = re.findall(r'^run_test "([^"]+)" (\d+) (\d+) "(true|false)"', f.read(), re.M)

failed = 0
for pos, depth, expected, chess960 in tests:
    cmds = []
    if chess960 == "true":
        cmds.append("setoption name UCI_Chess960 value true")
    cmds += [f"position {pos}", f"go perft {depth}", "quit"]

    start = time.time()
    out = subprocess.run(
        [engine], input="\n".join(cmds) + "\n", capture_output=True, text=True
    ).stdout
    match = re.search(r"Nodes searched: (\d+)", out)
    got = match.group(1) if match else "none"

    ok = got == expected
    failed += not ok
    print(
        f"{'OK  ' if ok else 'FAIL'} depth {depth} {got:>11} (expected {expected})"
        f" {time.time() - start:5.1f}s  {pos[:50]}"
    )

print(f"{len(tests) - failed}/{len(tests)} passed")
sys.exit(1 if failed else 0)
