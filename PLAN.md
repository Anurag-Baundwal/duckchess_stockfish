# duckchess_stockfish plan

A duck chess engine built on Stockfish's infrastructure. This plan covers the
board representation, movegen and perft, then a very simple handcrafted eval
and search. Search improvements and OpenBench integration come later, one SPRT
at a time.

- Base: official-stockfish/Stockfish ff7cee42 (2026-10-28), Bench 1556650,
  tagged `sf-base`. Stockfish 11 (last handcrafted eval) is in the same history
  as tag `sf_11`.
- Remotes: `origin` = github.com/Anurag-Baundwal/duckchess_stockfish,
  `upstream` = official-stockfish/Stockfish.
- Background analysis (fork vs scratch, wakwak PR-by-PR with SPRT Elo,
  OpenBench changes): `..\duck_engine_analysis.md` (repos7).
- Reference implementations: Fairy-Stockfish (`repos7/Fairy-Stockfish`, duck
  build `src/fsf-duck.exe`, which needs `/c/msys64/clang64/bin` on PATH) and
  wakwak (`repos7/wakwak`).
- Formats shared by Fairy-Stockfish and Silverrzz/fastchess:
  - FEN with the duck as `*`, e.g. `rnbqkbnr/pppppppp/8/4*3/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1`
  - Moves as `e2e4,e4d5` (chess move, comma, to-square + duck square);
    castling as `e1g1,g1b1`.
  - Having no legal move is a win for the side to move.
- Reference perft from the start position: depth 1 = 640, depth 2 = 379440.

## Ground rules

- Every commit ends with `Bench: N`, plus `No functional change` when the
  bench is identical.
- One change per commit, and the subject says what that commit changes.
- Push after each step.
- Phase 1 keeps the engine playing *chess*, so every step can be checked with
  chess perft (`tests/perft.sh`) and bench before any duck code exists.

## Phase 1: a lean chess base (still chess)

- **1.1 Rename the engine.**
  - `src/misc.cpp` `engine_info()`: "Stockfish" → "duckchess_stockfish".
  - Short README header.
  - Bench unchanged.
- **1.2 Remove Syzygy.**
  - Delete `src/syzygy/`, the `Syzygy*` options in `src/engine.cpp`, and the
    tablebase code in `src/search.cpp` (Step 7 probe, root TB ranking,
    `syzygy_extend_pv`).
  - Remove them from the Makefile `SRCS`/`VPATH`.
  - Bench must be unchanged.
- **1.3 Replace NNUE with a handcrafted eval (material + PSQT).**
  - First, in its own commit, remove universal binary builds: `src/universal/`,
    `scripts/check_universal*.sh`, `macos-lipo` and the Makefile's universal
    driver. They exist to ship one multi-arch binary with the net embedded;
    OpenBench builds natively on each worker. Bench must be unchanged.
  - Take Stockfish 11's material values (`git show sf_11:src/types.h`) and
    PSQT `Bonus`/`PBonus` tables (`git show sf_11:src/psqt.cpp`).
  - Taper midgame/endgame by non-pawn material, as SF11 did; keep a `psq`
    score in `StateInfo`, updated in `put_piece`/`remove_piece`/`move_piece`.
  - Delete `src/nnue/`, the `EvalFile` option, network loading
    (`src/engine.cpp`), accumulators and `Dirties` (`src/position.*`,
    `src/search.*`, `src/thread.*`), and the Makefile `net` target.
  - Result: the engine builds without downloading a net.
- **1.4 Remove NUMA, large pages and shared memory.**
  - Delete `src/numa.h`, `src/shm.h` and `src/shm_unix.h`, the `NumaPolicy`
    option, thread binding and per-node replication (`src/thread.*`,
    `src/engine.*`, `src/history.h`, `src/search.*`).
  - Replace `memory.cpp`'s large-page allocators with plain aligned
    allocation for the TT and workers.
  - Done after 1.3 because most of the NUMA code replicates the NNUE network.
    About 4-5% nps (stockfish_4pc measured +4.65% for large pages), for a lot
    less code.
  - Bench must be unchanged (1 thread).
- **1.5 Minimal chess search.** Rewrite `search()`/`qsearch()` in
  `src/search.cpp`.
  - **Keep:** Worker, threads, the iterative-deepening loop, time checks, root
    moves, PV, TT, check handling (only until Phase 2).
  - **Remove:** aspiration windows, RFP, razoring, futility, null move, IIR,
    ProbCut, singular and other extensions, LMP, LMR, SEE pruning, correction
    histories, and continuation/pawn/low-ply/capture histories.
  - **Search:** full-window ID plus PVS. The MovePicker (`src/movepick.*`)
    orders TT move, then captures by MVV-LVA, then quiets by butterfly history.
- **1.6 32-bit `Move` with a duck field** (still chess, field unused).
  - `src/types.h`: the low 16 bits keep Stockfish's layout (to, from,
    promotion, type); bits 16-21 hold the duck square.
  - Add `duck_sq()` and a chess-part accessor; histories index by the chess
    part (16 bits), so table sizes stay the same.
  - `src/tt.cpp`: the move field becomes 32-bit, so a 32-byte cluster holds 2
    entries instead of 3.
  - Chess perft must be unchanged.

## Phase 2: duck rules, movegen, perft

- **2.1 Position** (`src/position.*`):
  - Duck square in `StateInfo`, `SQ_NONE` until White's first move. Zobrist
    `duck[64]` is XORed into the key.
  - The duck is kept outside the piece arrays. Occupancy for attacks and
    movegen is `pieces() | duck`.
  - FEN: parse and print `*`. `d` shows the duck.
  - `do_move` places the duck; `undo_move` restores it.
- **2.2 Duck rules** (match Fairy-Stockfish and fastchess):
  - No check concept: remove checkers, pinners, blockers, check squares,
    `gives_check`, `EVASIONS` and the check-based `legal()`.
  - Kings can be captured.
  - Castling needs its path free of pieces *and* the duck; castling out of or
    through attacked squares is allowed.
  - The duck blocks pushes, double pushes and en passant.
  - The duck must move to a different empty square each turn; it may land on
    the square the piece just left.
  - Check the half-move clock on castling against Fairy-Stockfish.
- **2.3 Movegen** (`src/movegen.*`):
  - `generate<CAPTURES|QUIETS|ALL>` keeps producing chess moves (≤256, so
    `MAX_MOVES` is unchanged).
  - New `Position::duck_targets(Move)`: the empty squares after the chess move,
    minus the current duck square; any empty square on White's first move.
  - A helper expands chess moves into (chess move, duck) pairs.
  - Check against Fairy-Stockfish whether king captures also get ducks.
- **2.4 UCI** (`src/uci.cpp` `UCIEngine::move` / `to_move`):
  - Print and parse `e2e4,e4d5`, with castling as the king's destination in
    standard chess.
  - Accept a king capture without the duck suffix, as fastchess does.
  - Add a `UCI_Variant` combo with `duck` as default and only value, so
    fastchess `-variant duck` can set it.
- **2.5 Perft** (`src/perft.h`):
  - Divide output in Fairy-Stockfish's format (`e2e4,e4e2: 1`,
    `Nodes searched: N`).
  - Bulk count at the last ply with `popcount(duck_targets(m))`.
- **2.6 Game-end rules:**
  - A side with no king has lost; a side with no chess move wins.
  - 50-move rule at 100 plies; threefold repetition with the duck in the key.
  - Disable `upcoming_repetition`: its cuckoo tables assume normal moves. A
    duck version comes later.
- **2.7 Tests:**
  - `tests/gen_duck_perft.py` asks `fsf-duck.exe` for perft counts (depth 1-3,
    depth 4 where fast) on `tests/duck_perft.epd`.
  - `tests/duck_perft_check.py` runs our `go perft` and prints the divide diff
    on any mismatch, like stockfish_4pc's `perft_check.py`.
  - Positions to cover:
    - start and early openings
    - castling, with the duck on the path and through attacked squares
    - en passant, with the duck on the ep square and on the captured pawn's
      square
    - promotions, including capture-promotions
    - a king capture available
    - no moves at all (a win)
    - a boxed-in king
    - about 40 positions from random Fairy-Stockfish playouts
  - Optional: cross-check a subset with wakwak's `perft`, if the Rust
    toolchain is installed.

## Phase 3: a simple HCE and search for duck rules

- **3.1 Eval:** the Phase 1 material + PSQT with no king value (capturing the
  king ends the game), plus a tempo bonus. Nothing duck-specific yet.
- **3.2 Search:**
  - Terminal handling: no king → mated score; no move → win.
  - Remove the leftover check-based paths.
  - Move loop: chess moves in MovePicker order, each with its duck squares
    ordered by a duck history `[color][square]`.
  - Qsearch: captures and promotions only, capped at K duck squares per capture
    (start at K = 5, as wakwak's first qsearch did).
  - The TT stores the full move; mate scores and repetition draws handled.
- **3.3 Bench:** replace `Defaults` in `src/benchmark.cpp` with about 20 duck
  FENs from Fairy-Stockfish games. Set the default bench depth so `bench`
  takes a few seconds.
- **3.4 Next** (separate plans, one SPRT each): Duck or Die, duck refutations,
  duckless TT, neutral ducks, unique-move LMR/LMP, then Stockfish's search
  features re-added one at a time. See the analysis file.

## Phase 4: legality smoke test (local)

- Build Silverrzz/fastchess at a pinned SHA.
- Play 100+ games at a short time control against itself and against
  `fsf-duck.exe`.
- Pass condition: no illegal moves, crashes or timeouts.
- OpenBench support (per-variant runner) is a separate plan.

## Verification

- **Builds:** MSYS2 g++/make from PowerShell with `TMP` set, e.g.
  `make -j build ARCH=x86-64-avx2`; later `profile-build`, which OpenBench
  uses.
- **Phase 1:**
  - `tests/perft.sh` (chess perft) passes after every step.
  - Steps marked "No functional change" reproduce the previous bench exactly.
  - The engine answers `uci`/`go` and plays legal chess.
- **Phase 2:**
  - `tests/duck_perft_check.py` reports every position matching
    Fairy-Stockfish at every depth.
  - Startpos perft 1/2 = 640 / 379440.
- **Phase 3:**
  - `bench` runs and gives a stable node count (two runs identical).
  - `go depth N` from duck positions returns legal `e2e4,e4d5` moves; finds a
    mate in 1 (king capture).
- **Phase 4:** the fastchess games finish with 0 illegal moves, crashes or
  timeouts.
