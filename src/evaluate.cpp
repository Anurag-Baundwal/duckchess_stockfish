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

#define S(mg, eg) make_score(mg, eg)

// clang-format off

// MobilityBonus[PieceType - 2][attacked] contains bonuses for middle and end
// game, indexed by piece type and number of attacked squares in the mobility
// area. These are Stockfish 11's values.
constexpr TaperedScore MobilityBonus[][32] = {
  { S(-62,-81), S(-53,-56), S(-12,-30), S( -4,-14), S(  3,  8), S( 13, 15), // Knights
    S( 22, 23), S( 28, 27), S( 33, 33) },
  { S(-48,-59), S(-20,-23), S( 16, -3), S( 26, 13), S( 38, 24), S( 51, 42), // Bishops
    S( 55, 54), S( 63, 57), S( 63, 65), S( 68, 73), S( 81, 78), S( 81, 86),
    S( 91, 88), S( 98, 97) },
  { S(-58,-76), S(-27,-18), S(-15, 28), S(-10, 55), S( -5, 69), S( -2, 82), // Rooks
    S(  9,112), S( 16,118), S( 30,132), S( 29,142), S( 32,155), S( 38,165),
    S( 46,166), S( 48,169), S( 58,171) },
  { S(-39,-36), S(-21,-15), S(  3,  8), S(  3, 18), S( 14, 34), S( 22, 54), // Queens
    S( 28, 61), S( 41, 73), S( 43, 79), S( 48, 92), S( 56, 94), S( 60,104),
    S( 60,113), S( 66,120), S( 67,123), S( 70,126), S( 71,133), S( 73,136),
    S( 79,140), S( 88,143), S( 88,148), S( 99,166), S(102,170), S(102,175),
    S(106,184), S(109,191), S(113,206), S(116,212) }
};

// clang-format on

#undef S

// Mobility, as in Stockfish 11: each knight, bishop, rook and queen gets a
// bonus by the number of squares it attacks in its side's mobility area. That
// area leaves out the squares of our king and queen, of our pawns that are
// blocked or still on their first two ranks, and the squares attacked by enemy
// pawns. Bishops and rooks see through queens, and rooks through rooks of their
// color. The duck blocks like any piece. Unlike Stockfish 11 there is no
// restriction for pinned pieces, as duck chess has no pins.
template<Color Us>
TaperedScore mobility(const Position& pos) {

    constexpr Color     Them     = ~Us;
    constexpr Direction Down     = Us == WHITE ? SOUTH : NORTH;
    constexpr Bitboard  LowRanks = Us == WHITE ? Rank2BB | Rank3BB : Rank7BB | Rank6BB;

    Bitboard blockedPawns = pos.pieces(Us, PAWN) & (shift(pos.pieces(), Down) | LowRanks);
    Bitboard mobilityArea =
      ~(blockedPawns | pos.pieces(Us, KING, QUEEN) | pawn_attacks_bb<Them>(pos.pieces(Them, PAWN)));

    TaperedScore score = SCORE_ZERO;

    for (PieceType pt : {KNIGHT, BISHOP, ROOK, QUEEN})
        for (Bitboard pieces = pos.pieces(Us, pt); pieces;)
        {
            Square   s        = pop_lsb(pieces);
            Bitboard occupied = pt == BISHOP ? pos.pieces() ^ pos.pieces(QUEEN)
                              : pt == ROOK ? pos.pieces() ^ pos.pieces(QUEEN) ^ pos.pieces(Us, ROOK)
                                           : pos.pieces();

            score +=
              MobilityBonus[pt - 2][popcount(Attacks::attacks_bb(pt, s, occupied) & mobilityArea)];
        }

    return score;
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
// and piece-square tables, mobility, and the danger to the king of the side to
// move, tapered by the game phase.
Value Eval::evaluate(const Position& pos) {

    int          phase  = game_phase(pos);
    int          danger = king_danger(pos);
    TaperedScore score  = pos.psq_score() + mobility<WHITE>(pos) - mobility<BLACK>(pos);
    Value        v      = interpolate(score, phase);

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
    ss << "Mobility                 mg " << mg_value(mobility<WHITE>(pos) - mobility<BLACK>(pos))
       << ", eg " << eg_value(mobility<WHITE>(pos) - mobility<BLACK>(pos))
       << " (white side, internal units)\n";
    ss << "King danger (duck shield) " << king_danger(pos) << " for the side to move, penalty "
       << interpolate(DuckShield * (king_danger(pos) * king_danger(pos)), phase)
       << " (internal units)\n";
    ss << "Final evaluation         " << v << " (white side, internal units)\n";
    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2);
    ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";

    return ss.str();
}

}  // namespace Stockfish
