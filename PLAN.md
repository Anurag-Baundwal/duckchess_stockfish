# duckchess_stockfish plan

A duck chess engine built on Stockfish's infrastructure. This plan covers the
board representation, movegen and perft, then a very simple handcrafted eval
and search, then OpenBench support (Phase 5). From then on, every search and
eval improvement is validated with an SPRT, one change at a time.

- Base: official-stockfish/Stockfish ff7cee42 (2026-10-28), Bench 1556650,
  tagged `sf-base`. Stockfish 11 (last handcrafted eval) is in the same history
  as tag `sf_11`.
- Remotes: `origin` = github.com/Anurag-Baundwal/duckchess_stockfish,
  `upstream` = official-stockfish/Stockfish.
- Background analysis (fork vs scratch, wakwak PR-by-PR with SPRT Elo,
  OpenBench changes): `..\duck_engine_analysis.md` (repos7).
- References (decided 2026-11-21):
  - Rules: Silverrzz/fastchess's `DuckPosition`
    (`app/src/game/variant/position.cpp` at `cfa3671a44`), because it
    adjudicates our games.
  - Design and perft: wakwak (`repos7/wakwak`). It builds with Rust 1.99
    (rustup, installed 2026-11-21): `cargo build --release --locked`, with
    `CARGO_TARGET_DIR` outside the repo.
  - Fairy-Stockfish (`repos7/Fairy-Stockfish`, duck build `src/fsf-duck.exe`,
    which needs `/c/msys64/clang64/bin` on PATH): a cross-check only. It is
    weak at duck chess, and its perft keeps generating moves after a king
    capture.
- Formats used by fastchess and wakwak:
  - FEN with the duck as `*`, e.g. `rnbqkbnr/pppppppp/8/4*3/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1`
  - Moves as `e2e4,e4d5` (chess move, comma, to-square + duck square);
    castling as `e1g1,g1b1`.
  - Having no legal move is a win for the side to move.
- Reference perft from the start position: 640, 379440, 249921262 and
  164318412891 at depths 1-4.
- Duck960 (double Fischer random starts) is what wakwak tests with, and what
  we will test with (Phase 5): engine options `UCI_Variant=duck
  UCI_Chess960=true`, castling written as king takes rook, with the rook's
  square in the duck suffix (`b1h1,h1h7`).

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

Rules, from fastchess's `DuckPosition`; wakwak agrees:
- A turn is a chess move plus a duck square. The duck moves to a different
  square that is empty after the chess move. The square the piece left counts
  as empty, and so does the pawn captured en passant. After castling, the
  king's and rook's destinations are taken and their starting squares free.
  Before the first placement (a FEN without `*`), any empty square will do.
- No check: pins, checks and attacked squares restrict nothing, and kings can
  be captured, also by the other king.
- Pieces can't move onto or through the duck. It blocks pushes, double pushes,
  and en passant onto its square.
- Castling: the squares between the king and its destination and between the
  rook and its destination, destinations included, must be free of other
  pieces and of the duck. Attacks don't matter. Duck960 castles as Chess960.
- The game ends, checked in this order: the side to move has no king (it has
  lost); it has no chess move (it wins); the halfmove clock is 100 (draw);
  threefold repetition, the duck included (draw).
- A king capture may omit the duck suffix. We always send one.

Steps:
- **2.1 Perft suite**, before any engine change:
  - `tests/duck_perft.epd`: 80 positions with D1-D3, D4 for two of them.
    - Hand-built rule cases: the first duck placement; castling with the duck
      on the king's or the rook's path, and through attacked squares; en
      passant, with the duck on its square or dropped there by the double
      push; promotions; king captures; no-move wins; the halfmove clock at
      99; ducks blocking pieces and pawns; Duck960 castling, including a king
      or rook that is already on its castling square.
    - 30 standard and 22 DFRC positions from wakwak's `genfens`.
  - `tests/gen_duck_perft.py WAKWAK`: counts from wakwak's `splitperft`. Its
    perft counts a terminal position (no king or no chess move for the side to
    move, or a halfmove clock of 100) as one node. Like our `go perft`, its
    `splitperft` doesn't apply that to the root.
  - `tests/duck_perft_check.py ENGINE [--wakwak WAKWAK]`: runs our `go perft`;
    on a mismatch it lists the root moves whose counts differ from wakwak's.
- **2.2 The duck on the board** (No functional change):
  - `StateInfo::duckSquare`, `SQ_NONE` until the first placement, and
    `Zobrist::duck[64]` in the key.
  - `byTypeBB[ALL_PIECES]` includes the duck, so every occupancy test sees it:
    slider attacks, pushes and castling paths. The piece-type and color
    bitboards and `board[]` don't.
  - FEN: parse and print `*`. `d` shows it.
  - `do_move` doesn't move it yet, so positions without a duck play chess as
    before.
- **2.3 Play duck chess** (one commit, checked by the perft suite):
  - Position: remove the check machinery (checkers, pinners, blockers, check
    squares, `gives_check`, `EVASIONS`, the castling attack tests). Add
    `duck_targets(Move)`. `do_move` moves the duck, and sets the en passant
    square only when a capture onto it is possible. `legal()`, for TT moves
    and UCI input: a pseudo-legal chess move with a duck square from
    `duck_targets()`.
  - Movegen: `generate<CAPTURES|QUIETS|ALL>` keep producing chess moves
    (≤ 256, so `MAX_MOVES` is unchanged), king captures included.
  - Game end as above. `upcoming_repetition()` is switched off: its cuckoo
    tables assume chess moves.
  - UCI: `e2e4,e4d5`, `e7e8q,e8d5`, castling `e1g1,g1b1`, or with
    `UCI_Chess960` king takes rook (`e1h1,h1b1`). Accept a king capture
    without a duck. Add a `UCI_Variant` combo whose only value is `duck`.
  - Perft (`src/perft.h`): wakwak's counting, divide lines `e2e4,e4d5: N`,
    and a bulk count at the last ply with `popcount(duck_targets(m))`.
  - Search: Phase 1.5's search over whole turns, with nothing duck-specific.
    Each chess move, in MovePicker order, is tried with every duck square in
    square order, in qsearch too (captures only). Terminal scores: no king is
    a loss, no chess move a win. Duck pruning and duck ordering come in
    Phase 6, by SPRT.
  - Bench: the same positions at a lower depth, so that `bench` takes a few
    seconds.
  - Verified: `duck_perft_check.py` passes; bench stable; debug and ARM builds.

## Phase 3: duck bench

- Replace `Defaults` in `src/benchmark.cpp` with about 20 duck FENs, some of
  them Duck960, from wakwak's `genfens`, and set the bench depth so `bench`
  takes a few seconds. No eval or search change: every one of those waits for
  an SPRT (Phase 6), even ones that look obvious, like a tempo bonus or a cap
  on duck squares in qsearch.

## Phase 4: legality smoke test (local)

- Build Silverrzz/fastchess pinned at `cfa3671a44` (its "duck960" commit, two
  commits on top of upstream fastchess `4e691463ce`).
- Play 100+ games at a short time control against itself, wakwak (built
  locally) and `fsf-duck.exe`: `-variant duck960` with the DFRC book of
  Phase 5, and `-variant duck` from the start position.
- Pass condition: no illegal moves, crashes or timeouts.

## Phase 5: OpenBench

wakwak's test setup, from its OpenBench instance (chess.n9x.co, checked
2026-11-21), which we copy:
- Variant Duck960, engine options `UCI_Variant=duck UCI_Chess960=true`.
- Book `Duck_DFRC.epd` = `DFRC_4852_v2.epd.zip` from
  https://file.ax/DFRC_4852_v2.epd.zip (121 MB zipped, sha256 `54f494a2…` for
  the 333 MB DFRC_4852_v2.epd inside): 5,030,384 DFRC openings about ten
  plies in, without a duck. Silverrzz/wakwak-books (generated duck
  books, no license file) is not used by its presets.
- Match runner: Silverrzz/fastchess `-variant duck960`.
- Presets: STC 8+0.08 (Hash 16), LTC 40+0.4 (Hash 64); gainers `[0, 4.20]`,
  non-regression `[-4.20, 0]`, confidence `[0.10, 0.05]`; win adjudication
  `movecount=8 score=800`, draw adjudication `movenumber=32 movecount=10
  score=0`; scale_nps 700000; workload 32 at STC, 8 at LTC.

Our fork (`repos7/openbench`) is 4PC-only today: Teams through `match.py`, FFA
through `ffa_match.py`. **Teams and FFA must keep working unchanged.**
- A `variant` field on EngineConfig and Test, default empty (= 4PC, where the
  `ffa` flag still picks Teams or FFA), following the FFA precedent
  (416782b) and MattBench 213d671's per-variant runners. Every new code path
  is gated on it; `match.py` and `ffa_match.py` are not edited.
- Server: migration, `import_engines` reads `variants` (`Duck960`,
  `Duckchess`), `verify_workload.py` requires Dev and Base of the same
  variant and keeps the 4PC limits for 4PC tests, `get_workload.py` sends the
  runner config, `Engines/duckchess_stockfish.json` with presets copied from
  wakwak's, the book through a Books migration.
- Client: for duck workloads, build the pinned fastchess into
  `Runners/<variant>`, run it with `-variant duck960`, the book, adjudication,
  `-pgnout` and `-recover`, and parse its "Finished game" lines; add
  fastchess to the cleanup lists; bump the client version.
- Before relying on it: Teams, FFA and duck tests side by side on a local
  server and worker.
- First test: a fixed-games duck test (dev = base) to prove the pipeline,
  then SPRTs against master for every change of Phase 6.

## Phase 6: improvements, one SPRT each

Every eval and search change is its own commit and passes an SPRT against
master on OpenBench before it is merged: gainer bounds for additions,
non-regression bounds for simplifications, and an LTC for search changes.
The commit message carries the "Passed STC/LTC" block. Candidates, roughly in
the order wakwak gained from them (see the analysis file):
- First, time management: Stockfish's expects each iteration to take about
  twice the last, ours take about ten times as long, so at 8+0.08 most
  searches start with under 500 ms on the clock (Phase 4 in progress.txt).
- Duck square ordering, e.g. a duck history `[color][square]`.
- Qsearch: a limit on duck squares per capture (wakwak started at 5), Duck or
  Die.
- Duck refutations, duckless TT, neutral ducks, unique-move LMR/LMP.
- Stockfish's search features, one at a time.
- Eval: a tempo bonus, Stockfish 11 terms that still make sense, then NNUE.

## Verification

- **Builds:** MSYS2 g++/make from PowerShell with `TMP` set, e.g.
  `make -j build ARCH=x86-64-avx2`; later `profile-build`, which OpenBench
  uses.
- **Phase 1:**
  - `tests/perft.sh` (chess perft) passes after every step.
  - Steps marked "No functional change" reproduce the previous bench exactly.
  - The engine answers `uci`/`go` and plays legal chess.
- **Phase 2:**
  - `tests/duck_perft_check.py` reports every position matching wakwak at
    every depth.
  - Startpos perft 640 / 379440 / 249921262 / 164318412891.
- **Phase 3:**
  - `bench` runs and gives a stable node count (two runs identical).
  - `go depth N` from duck positions returns legal `e2e4,e4d5` moves; finds a
    mate in 1 (king capture).
- **Phase 4:** the fastchess games finish with 0 illegal moves, crashes or
  timeouts.
- **Phase 5:** a fixed-games duck test finishes on OpenBench, while Teams and
  FFA tests keep running as before.
