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

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>

#include "position.h"
#include "types.h"
#include "uci.h"

namespace Stockfish {

namespace {

// Game phase limits of Stockfish 11: with at least MidgameLimit of non-pawn
// material on the board the position counts as a pure middlegame, with at most
// EndgameLimit as a pure endgame.
constexpr Value MidgameLimit = 15258;
constexpr Value EndgameLimit = 3915;
constexpr int   PhaseMidgame = 128;

// Returns the game phase, from 0 (endgame) to PhaseMidgame (middlegame)
int game_phase(const Position& pos) {
    Value npm = std::clamp(pos.non_pawn_material(), EndgameLimit, MidgameLimit);
    return (npm - EndgameLimit) * PhaseMidgame / (MidgameLimit - EndgameLimit);
}

// Interpolates between the middlegame and the endgame value of a score
Value interpolate(TaperedScore s, int phase) {
    return (mg_value(s) * phase + eg_value(s) * (PhaseMidgame - phase)) / PhaseMidgame;
}

// Penalty per unit of king danger squared (twice wakwak's 45 and 30 centipawns)
constexpr TaperedScore DuckShield = make_score(140, 136);

// Duck shield, as in wakwak: returns how many attacks on the king of the side
// to move its duck can't block. The side to move is about to move the duck, so
// it counts as lifted. An attack by a knight, pawn or king, or by a slider
// next to the king, can't be blocked. Of the slider attacks from further away,
// the duck blocks one. With any danger left, the move must deal with the
// attacks itself, or the king is lost.
int king_danger(const Position& pos) {

    Color us = pos.side_to_move();

    if (!pos.pieces(us, KING))
        return 0;

    Square   ksq      = pos.square<KING>(us);
    Bitboard occupied = pos.pieces();

    if (pos.duck_square() != SQ_NONE)
        occupied ^= pos.duck_square();

    int unblockable = 0, blockable = 0;

    for (Bitboard attackers = pos.attackers_to(ksq, occupied) & pos.pieces(~us); attackers;)
    {
        // between_bb() includes the attacker's square, so nothing lies
        // between them if that is all it holds
        Square s = pop_lsb(attackers);
        if (Attacks::between_bb(ksq, s) == square_bb(s))
            ++unblockable;
        else
            ++blockable;
    }

    return unblockable + std::max(blockable - 1, 0);
}

}  // namespace

// Evaluates the position from the point of view of the side to move: material
// and piece-square tables, and the danger to the king of the side to move,
// tapered by the game phase.
Value Eval::evaluate(const Position& pos) {

    int   phase  = game_phase(pos);
    int   danger = king_danger(pos);
    Value v      = interpolate(pos.psq_score(), phase);

    v = pos.side_to_move() == WHITE ? v : -v;
    v -= interpolate(DuckShield * (danger * danger), phase);

    // Damp down the evaluation linearly when shuffling
    v -= v * pos.rule50_count() / 189;

    // Guarantee that the evaluation does not hit the tablebase range
    v = std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);

    return v;
}

// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
std::string Eval::trace(Position& pos) {

    TaperedScore s     = pos.psq_score();
    int          phase = game_phase(pos);
    Value        v     = evaluate(pos);
    v                  = pos.side_to_move() == WHITE ? v : -v;

    std::stringstream ss;
    ss << "Material and PSQT        mg " << mg_value(s) << ", eg " << eg_value(s) << ", phase "
       << phase << "/" << PhaseMidgame << " (white side, internal units)\n";
    ss << "King danger (duck shield) " << king_danger(pos) << " for the side to move, penalty "
       << interpolate(DuckShield * (king_danger(pos) * king_danger(pos)), phase)
       << " (internal units)\n";
    ss << "Final evaluation         " << v << " (white side, internal units)\n";
    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2);
    ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";

    return ss.str();
}

}  // namespace Stockfish
