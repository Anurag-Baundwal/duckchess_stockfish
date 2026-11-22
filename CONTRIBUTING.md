# Contributing to duckchess_stockfish

Thanks for your interest. The full developer guidelines are in
[AGENTS.md](AGENTS.md); in short:

- Write code in Stockfish's style and keep its structure, so that upstream
  ideas port easily. Format with the repository's `.clang-format`.
- Make one change per commit.
- Test every functional change with an SPRT on OpenBench against master
  (10+0.1, Duck_DFRC.epd). It goes in only if it passes.
- End each commit message with `Bench: N`, the node count from
  `stockfish.exe bench`, after `No functional change` if the bench is
  unchanged.
- When a change comes from another engine's idea, say so in the commit
  message and the code comments, as is done for wakwak, and write the code
  fresh rather than translating it.

Report bugs in this repository's issues, with the position (FEN) and the UCI
commands that show the problem.

By contributing, you agree that your contributions are licensed under the
[GNU General Public License version 3](Copying.txt).
