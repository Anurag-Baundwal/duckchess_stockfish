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

// Bonus per piece on a square that its own pawns attack (wakwak's 8 and 6
// centipawns)
constexpr TaperedScore PawnDefended = make_score(12, 14);

// Penalty per unit of king danger squared (twice wakwak's 45 and 30 centipawns)
constexpr TaperedScore DuckShield = make_score(140, 136);

// clang-format off

// ShelterStrength[distance from edge][rank] contains the bonus for our pawn
// in front of our king, by the distance of its file from the edge and its
// rank. Rank 0 means there is no pawn on that file. These are Stockfish 11's
// values.
constexpr Value ShelterStrength[int(FILE_NB) / 2][RANK_NB] = {
  { -6, 81, 93,  58,  39,  18,   25 },
  {-43, 61, 35, -49, -29, -11,  -63 },
  {-10, 75, 23,  -2,  32,   3,  -45 },
  {-39,-13,-29, -52, -48, -67, -166 }
};

// UnblockedStorm[distance from edge][rank] contains the penalty for an enemy
// pawn on those files, by its rank, when no pawn of ours stands right in front
// of it. Rank 0 means there is no enemy pawn on that file, or it is behind our
// king. UnblockedStorm[0][1-2] covers an enemy pawn on the edge, likely
// blocked by our king. These are Stockfish 11's values.
constexpr Value UnblockedStorm[int(FILE_NB) / 2][RANK_NB] = {
  { 85,-289,-166, 97, 50, 45, 50 },
  { 46, -25, 122, 45, 37,-10, 20 },
  { -6,  51, 168, 34, -2,-22,-14 },
  {-15, -11, 101,  4, 11,-15,-29 }
};

// clang-format on

// Penalty for an enemy pawn on our third rank, blocked by our pawn
constexpr TaperedScore BlockedStorm = make_score(82, 82);

// Pawn-defended pieces, as in wakwak: returns how many pieces of the given
// color, pawns and king included, stand on squares that its own pawns attack.
template<Color Us>
int pawn_defended(const Position& pos) {
    return popcount(pawn_attacks_bb<Us>(pos.pieces(Us, PAWN)) & pos.pieces(Us));
}

// Returns the pawn shelter bonus and the pawn storm penalty for a king of the
// given color on the given square, as in Stockfish 11. It looks at the king's
// file and the two closest files, at our pawns and the enemy pawns that are not
// behind the king.
template<Color Us>
TaperedScore evaluate_shelter(const Position& pos, Square ksq) {

    constexpr Color Them = ~Us;

    Bitboard b          = pos.pieces(PAWN) & ~forward_ranks_bb(Them, ksq);
    Bitboard ourPawns   = b & pos.pieces(Us);
    Bitboard theirPawns = b & pos.pieces(Them);

    TaperedScore bonus = make_score(5, 5);

    File center = std::clamp(file_of(ksq), FILE_B, FILE_G);
    for (File f = File(center - 1); f <= File(center + 1); ++f)
    {
        b           = ourPawns & file_bb(f);
        int ourRank = b ? relative_rank(Us, frontmost_sq(Them, b)) : 0;

        b             = theirPawns & file_bb(f);
        int theirRank = b ? relative_rank(Us, frontmost_sq(Them, b)) : 0;

        int d = edge_distance(f);
        bonus += make_score(ShelterStrength[d][ourRank], 0);

        if (ourRank && (ourRank == theirRank - 1))
            bonus -= BlockedStorm * int(theirRank == RANK_3);
        else
            bonus -= make_score(UnblockedStorm[d][theirRank], 0);
    }

    return bonus;
}

// King safety from the pawns, as in Stockfish 11: the pawn shelter and storm
// score of the king's square, or of the square it castles to if that scores
// better in the middlegame and it may still castle, and an endgame penalty by
// the distance from the king to our closest pawn. The duck isn't considered:
// it moves every turn.
template<Color Us>
TaperedScore king_shelter(const Position& pos) {

    if (!pos.pieces(Us, KING))
        return SCORE_ZERO;

    Square ksq     = pos.square<KING>(Us);
    auto   compare = [](TaperedScore a, TaperedScore b) { return mg_value(a) < mg_value(b); };

    TaperedScore shelter = evaluate_shelter<Us>(pos, ksq);

    // If we can castle use the bonus after castling if it is bigger
    if (pos.can_castle(Us & KING_SIDE))
        shelter = std::max(shelter, evaluate_shelter<Us>(pos, relative_square(Us, SQ_G1)), compare);

    if (pos.can_castle(Us & QUEEN_SIDE))
        shelter = std::max(shelter, evaluate_shelter<Us>(pos, relative_square(Us, SQ_C1)), compare);

    // In endgame we like to bring our king near our closest pawn
    Bitboard pawns       = pos.pieces(Us, PAWN);
    int      minPawnDist = pawns ? 8 : 0;

    if (pawns & Attacks::attacks_bb(KING, ksq))
        minPawnDist = 1;
    else
        while (pawns)
            minPawnDist = std::min(minPawnDist, distance(ksq, pop_lsb(pawns)));

    return shelter - make_score(0, 16 * minPawnDist);
}

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
// and piece-square tables, pawn-defended pieces, the pawn shelter of the kings,
// and the danger to the king of the side to move, tapered by the game phase.
Value Eval::evaluate(const Position& pos) {

    int          phase  = game_phase(pos);
    int          danger = king_danger(pos);
    TaperedScore score  = pos.psq_score()
                       + PawnDefended * (pawn_defended<WHITE>(pos) - pawn_defended<BLACK>(pos))
                       + king_shelter<WHITE>(pos) - king_shelter<BLACK>(pos);
    Value v = interpolate(score, phase);

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
    ss << "Pawn-defended pieces     white " << pawn_defended<WHITE>(pos) << ", black "
       << pawn_defended<BLACK>(pos) << " (each mg " << mg_value(PawnDefended) << ", eg "
       << eg_value(PawnDefended) << ")\n";
    ss << "King shelter             mg "
       << mg_value(king_shelter<WHITE>(pos) - king_shelter<BLACK>(pos)) << ", eg "
       << eg_value(king_shelter<WHITE>(pos) - king_shelter<BLACK>(pos))
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
