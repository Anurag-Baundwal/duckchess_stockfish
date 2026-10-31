# Developer guidelines for duckchess_stockfish

duckchess_stockfish is a duck chess engine built on Stockfish master. It is not
written from scratch: it keeps Stockfish's architecture and code style, so that
the code reads like Stockfish and upstream ideas can be ported with little
friction. PLAN.md has the steps; progress.txt says where they stand.

## 1. Stay close to Stockfish

- Keep Stockfish's structure: the Worker/SearchManager split, `Stack` and
  `RootMove`, the staged MovePicker, `StateInfo` with incremental updates in
  `do_move` checked by `pos_is_ok`, and the TT cluster and aging logic.
- Make duck changes surgical. They should fit into the surrounding code, not
  stand out from it.
- Keep code where Stockfish keeps it. Types and helpers belong in `types.h`,
  `bitboard.h` and so on, not in whichever file is being edited.
- Keep comments. Update a comment when the code under it changes; never delete
  comments wholesale.
- Keep diffs clean: change only what the task needs. Don't reformat, reorder or
  rename unrelated code.
- Formatting follows Stockfish's `.clang-format`: column-aligned
  declarations, GPL headers, `assert`, `[[fallthrough]]`. Run
  `/c/msys64/clang64/bin/clang-format -i --style=file:.clang-format <file>` on
  every edited file (version 21.1.8 reproduces upstream's formatting exactly),
  then check that it only touched lines near your edit.
- Do only what the step asks. If the step is "a basic alpha-beta search", don't
  add qsearch or pruning on top.

## 2. What we deliberately leave out

These cost little or no strength but a lot of code. Don't add them back:

- NUMA: no thread binding, no per-node replication, no `numa.h`.
- Large pages: plain allocation (`std::make_unique`, `std::vector`, aligned
  `new`) instead of `memory.cpp`'s large-page allocators.
- Shared memory between processes (`shm.h`).
- Syzygy tablebases.
- Stockfish's NNUE, until we train our own duck net. The handcrafted eval comes
  first.

`NativeThread` stays: it is `std::thread` on Windows and Linux, and only adds
an 8 MB stack on macOS.

## 3. Duck chess specifics

- A turn is one combined move: a chess move plus a duck placement. `Move`
  keeps Stockfish's 16-bit layout in the low bits (to, from, promotion, type);
  the duck square sits above them. Histories index by the 16-bit chess part.
- The duck lives outside the piece arrays. Occupancy for attacks and movegen
  includes it: it blocks sliders, pushes, castling paths and en passant.
- There is no check. A king can be captured, which ends the game. A side with
  no legal move wins.
- UCI moves are written `e2e4,e4d5` (chess move, comma, to-square + duck
  square); castling as `e1g1,g1b1`. The FEN marks the duck with `*`. Both match
  Fairy-Stockfish and the Silverrzz/fastchess duck fork.
- Movegen is correct only when perft matches Fairy-Stockfish
  (`repos7/Fairy-Stockfish/src/fsf-duck.exe`, which needs
  `/c/msys64/clang64/bin` on PATH; `setoption name UCI_Variant value duck`).

## 4. Commits

- One change per commit. Steps in PLAN.md map to commits.
- Title: imperative mood, at most 60 characters ("Remove Syzygy tablebases").
  A follow-up commit says what it changes, never repeats its parent's title.
- Body: plain text, no Markdown. Explain what the logic does for the engine,
  not just which files changed, and point out duck-specific adaptations. Use
  bullet points only when they help.
- Last lines: `No functional change` when the bench is identical, then always
  `Bench: N` (OpenBench reads it from the head commit).
- Before committing, look at the staged diff only (`git diff --cached`), and at
  `git log -n 5` for the house style.
- Update progress.txt with every step: tick the box, add a log line (date,
  commit, bench, notes), set the next step.
- Never commit `.nnue` files, binaries or scratch files.

## 5. Building and checking

- Windows toolchain: MSYS2. Run make from PowerShell with
  `C:\msys64\ucrt64\bin;C:\msys64\usr\bin;` prepended to `$env:Path` and
  `$env:TMP`/`$env:TEMP` set to a writable folder, e.g.
  `make -j build ARCH=native COMP=mingw` in `src/`.
- Run git from Git Bash. MSYS2's git ignores `core.autocrlf` and would commit
  CRLF line endings.
- `stockfish.exe bench` gives the node count for the commit message. A step
  marked "No functional change" must reproduce the previous bench exactly.
- Pipe commands into the engine from Git Bash with `printf`. PowerShell puts a
  BOM in front of the first command.
