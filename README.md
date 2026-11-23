# duckchess_stockfish

duckchess_stockfish is a UCI engine for duck chess. It is a modified version
of [Stockfish][stockfish-link]: it was forked from Stockfish master ff7cee42
(28 October 2026) and has been changed for duck chess since 31 October 2026.
The git history records each change. It is in early development.

## Usage

duckchess_stockfish speaks UCI and has no graphical interface of its own.

  * `UCI_Variant` has one value, `duck`.
  * Set `UCI_Chess960` to `true` for Fischer random (DFRC) starting positions.
  * A move is a chess move and a duck square, separated by a comma, for
    example `e2e4,e4d5`.
  * In a FEN, `*` marks the duck.

## Compiling

```
cd src
make -j profile-build
```

`make help` lists the other targets.

## Testing

Every change is tested by an SPRT on OpenBench at 10+0.1 against master. The
opening book (Duck_DFRC.epd) and the match runner
([Silverrzz's fastchess][fastchess-link]) are the ones wakwak's OpenBench uses.

## Acknowledgements

  * The [Stockfish developers](AUTHORS). The code base is theirs, and
    src/psqt.cpp holds Stockfish 11's piece-square tables and piece values.

  * [wakwak][wakwak-link] (AGPL-3.0) and its contributors. The duck chess
    techniques below are reimplemented here from wakwak's published ideas, in
    Stockfish's style:
      * Duck or Die (wakwak PR #49)
      * duck refutations (#50, #52, #55, #58, #68, #78 and #89)
      * late duck pruning (#45 and #46)
      * duck history (#42)
      * duck shield (#92)
      * pawn-defended pieces (#87)

    The duck perft counts in tests/duck_perft.epd and the bench positions were
    made by running wakwak, and our OpenBench test setup follows wakwak's.

  * [Fairy-Stockfish][fairy-link], an opponent and a cross-check for perft.

  * [Silverrzz's fastchess][fastchess-link], the duck chess match runner.

## License

duckchess_stockfish, like Stockfish, is distributed under the
[GNU General Public License version 3](Copying.txt) (GPL v3). Whenever you
distribute it, you must include the license and the full source code (or a
pointer to where it can be found), and any changes must also be made available
under GPL v3.


[stockfish-link]: https://github.com/official-stockfish/Stockfish
[wakwak-link]:    https://github.com/Silverrzz/wakwak
[fairy-link]:     https://github.com/fairy-stockfish/Fairy-Stockfish
[fastchess-link]: https://github.com/Silverrzz/fastchess
