/*
  duckchess_stockfish, a UCI duck chess engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Evaluation with wakwak's network (github.com/Silverrzz/wakwak-nets, net
// "shotis"). The inference gives the same numbers as wakwak's own code.
//
// Network: 768 piece inputs (6 piece types x 2 colors x 64 squares, from the
// perspective's side, mirrored left-right when its king is on files E-H),
// 64 duck inputs and 11 halfmove clock buckets -> 2 x 1536 (pairwise
// multiplied, so 2 x 768) -> 16 (one of them is a skip connection, the
// other 15 go through both a clipped and a squared activation) -> 32 -> 1.
// The accumulator holds the pieces only; the duck and the halfmove clock rows
// are added when evaluating.

#ifndef NNUE_H_INCLUDED
#define NNUE_H_INCLUDED

#include <cstdint>
#include <memory>

#include "types.h"

namespace Stockfish {

class Position;

namespace NNUE {

constexpr int Inputs = 843;
constexpr int L1     = 1536;
constexpr int L2     = 16;
constexpr int L3     = 32;

// Changes of the piece inputs made by one move: at most two pieces removed
// and two added (castling). A king that crosses the middle of the board
// changes the mirroring of its side's inputs, which then must be rebuilt.
struct DirtyPieces {
    int    removed, added;
    Piece  removedPc[2], addedPc[2];
    Square removedSq[2], addedSq[2];
    bool   rebuild[COLOR_NB];
};

struct alignas(64) Accumulator {
    std::int16_t values[COLOR_NB][L1];
    bool         computed[COLOR_NB];
    DirtyPieces  dirty;  // Changes from the previous accumulator
};

// One accumulator per ply of the search, brought up to date lazily: only
// when a position is evaluated, starting from the last computed one.
class AccumulatorStack {
   public:
    AccumulatorStack();

    void reset();
    void push(const Position& pos, Move m);  // Call before pos.do_move(m)
    void push_null();
    void pop() { --size; }

    Value evaluate(const Position& pos);

   private:
    void update(const Position& pos, Color perspective);

    std::unique_ptr<Accumulator[]> stack;
    int                            size;
};

void init();

// Evaluates without a stack, building the accumulators from scratch
Value evaluate(const Position& pos);

}  // namespace NNUE

}  // namespace Stockfish

#endif  // #ifndef NNUE_H_INCLUDED
