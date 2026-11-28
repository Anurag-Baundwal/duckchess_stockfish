/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MOVEPICK_H_INCLUDED
#define MOVEPICK_H_INCLUDED

#include <memory>

#include "history.h"
#include "movegen.h"
#include "types.h"

namespace Stockfish {

class Position;

enum Stages : i8 {
    TT,
    GENERATE_NOISY,
    GOOD_NOISY,
    GENERATE_QUIET,
    QUIET,
    BAD_NOISY,
    FINISHED
};

// The moves of all the move pickers on the current path of the search, a stack
// owned by the search thread
struct MoveStack {
    static constexpr int Size = 1 << 21;

    MoveStack() :
        moves(new ExtMove[Size]) {}

    std::unique_ptr<ExtMove[]> moves;
    int                        top = 0;
};

// The continuation history moves of a position: 1, 2 and 4 plies before
struct ContinuationMoves {
    PieceTo ply1, ply2, ply4;
};

// Move scores for the main search, as in wakwak. Every move is a chess move
// with a duck square, and the two are scored together.
int quiet_piece_history(
  const Histories& h, const Position& pos, const ContinuationMoves& cm, Bitboard threats, Move m);

// The MovePicker class is used to pick one move at a time from the current
// position, as in wakwak: the TT move, then the captures and promotions with
// a good SEE, then the quiet moves, then the other captures. Each kind is
// generated with every duck square and sorted by the scores of the chess
// move and the duck square together. Ducks that restrict no move of the
// opponent ("neutral" ducks) can be pruned but for the best of them.
class MovePicker {
   public:
    MovePicker(const MovePicker&)            = delete;
    MovePicker& operator=(const MovePicker&) = delete;
    MovePicker(const Position&,
               Move,
               MoveStack&,
               const Histories&,
               const ContinuationMoves&,
               Bitboard threats,
               Bitboard neutralDucks,
               bool     pruneQuietNeutrals,
               bool     pruneNoisyNeutrals,
               int      seeThreshold,
               bool     qsearch);
    ~MovePicker() { stack.top = start; }

    Move   next_move();
    void   skip_quiets() { skipQuiets = true; }
    Stages stage_of() const { return stage; }

   private:
    template<GenType T>
    void generate(ExtMove* list);
    void sort_next(int end);

    const Position&          pos;
    MoveStack&               stack;
    const Histories&         histories;
    const ContinuationMoves& contMoves;
    Move                     ttMove;
    Bitboard                 threats, neutralDucks;
    bool                     pruneQuietNeutrals, pruneNoisyNeutrals;
    int                      seeThreshold;
    bool                     skipQuiets = false, skipBadNoisy = false;
    Stages                   stage;

    ExtMove* moves;  // Our part of the stack: the captures, then the quiets
    int      start, size = 0, cursor = 0, sortStart = 0, sortedEnd = 0, badCount = 0;
};

}  // namespace Stockfish

#endif  // #ifndef MOVEPICK_H_INCLUDED
