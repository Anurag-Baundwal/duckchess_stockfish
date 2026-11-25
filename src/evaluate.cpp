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

// KingAttackWeights[PieceType] contains the king attack weights by piece type.
// These are Stockfish 11's values.
constexpr int KingAttackWeights[PIECE_TYPE_NB] = {0, 0, 81, 52, 44, 10};

// Pawn-defended pieces, as in wakwak: returns how many pieces of the given
// color, pawns and king included, stand on squares that its own pawns attack.
template<Color Us>
int pawn_defended(const Position& pos) {
    return popcount(pawn_attacks_bb<Us>(pos.pieces(Us, PAWN)) & pos.pieces(Us));
}

// King attacks, as in Stockfish 11's Evaluation class: for each king, the
// enemy pieces attacking the ring of squares around it, and a penalty that
// grows with their number and weight. The duck is lifted, as it moves every
// turn and either side may place it next.
class KingAttacks {
   public:
    KingAttacks()                              = delete;
    KingAttacks(const KingAttacks&)            = delete;
    KingAttacks& operator=(const KingAttacks&) = delete;

    explicit KingAttacks(const Position& p);
    TaperedScore value() const { return king<WHITE>() - king<BLACK>(); }

   private:
    template<Color Us>
    void initialize();
    template<Color Us, PieceType Pt>
    void pieces();
    template<Color Us>
    TaperedScore king() const;

    const Position& pos;
    Bitboard        occupied;

    // attackedBy[color][KING] are the squares attacked by the king of the
    // given color
    Bitboard attackedBy[COLOR_NB][PIECE_TYPE_NB];

    // kingRing[color] are the squares adjacent to the king plus some other
    // very near squares, depending on king position.
    Bitboard kingRing[COLOR_NB];

    // kingAttackersCount[color] is the number of pieces of the given color
    // which attack a square in the kingRing of the enemy king.
    int kingAttackersCount[COLOR_NB];

    // kingAttackersWeight[color] is the sum of the "weights" of the pieces of
    // the given color which attack a square in the kingRing of the enemy king.
    // The weights of the individual piece types are given by the elements in
    // the KingAttackWeights array.
    int kingAttackersWeight[COLOR_NB];

    // kingAttacksCount[color] is the number of attacks by the given color to
    // squares directly adjacent to the enemy king. Pieces which attack more
    // than one square are counted multiple times. For instance, if there is
    // a white knight on g5 and black's king is on g8, this white knight adds 2
    // to kingAttacksCount[WHITE].
    int kingAttacksCount[COLOR_NB];
};

KingAttacks::KingAttacks(const Position& p) :
    pos(p),
    occupied(p.duck_square() != SQ_NONE ? p.pieces() ^ p.duck_square() : p.pieces()) {

    initialize<WHITE>();
    initialize<BLACK>();

    pieces<WHITE, KNIGHT>();
    pieces<BLACK, KNIGHT>();
    pieces<WHITE, BISHOP>();
    pieces<BLACK, BISHOP>();
    pieces<WHITE, ROOK>();
    pieces<BLACK, ROOK>();
    pieces<WHITE, QUEEN>();
    pieces<BLACK, QUEEN>();
}

// Computes the king attacks and the king ring of the given color, and counts
// the enemy pawns' attacks on that ring
template<Color Us>
void KingAttacks::initialize() {

    constexpr Color Them = ~Us;

    kingAttacksCount[Them] = kingAttackersWeight[Them] = 0;

    if (!pos.pieces(Us, KING))
    {
        attackedBy[Us][KING] = kingRing[Us] = 0;
        kingAttackersCount[Them]            = 0;
        return;
    }

    const Square ksq             = pos.square<KING>(Us);
    Bitboard     dblAttackByPawn = pawn_double_attacks_bb<Us>(pos.pieces(Us, PAWN));

    attackedBy[Us][KING] = Attacks::attacks_bb(KING, ksq);

    // Init our king safety tables
    Square s     = make_square(std::clamp(file_of(ksq), FILE_B, FILE_G),
                               std::clamp(rank_of(ksq), RANK_2, RANK_7));
    kingRing[Us] = Attacks::attacks_bb(KING, s) | s;

    kingAttackersCount[Them] =
      popcount(kingRing[Us] & pawn_attacks_bb<Them>(pos.pieces(Them, PAWN)));

    // Remove from kingRing[] the squares defended by two pawns
    kingRing[Us] &= ~dblAttackByPawn;
}

// Counts the pieces of the given color and type that attack the enemy king
// ring. Bishops see through queens, and rooks through queens and our rooks.
template<Color Us, PieceType Pt>
void KingAttacks::pieces() {

    constexpr Color Them = ~Us;

    for (Bitboard b = pos.pieces(Us, Pt); b;)
    {
        Square   s = pop_lsb(b);
        Bitboard attacks =
          Pt == BISHOP ? Attacks::attacks_bb(BISHOP, s, occupied ^ pos.pieces(QUEEN))
          : Pt == ROOK
            ? Attacks::attacks_bb(ROOK, s, occupied ^ pos.pieces(QUEEN) ^ pos.pieces(Us, ROOK))
            : Attacks::attacks_bb(Pt, s, occupied);

        if (attacks & kingRing[Them])
        {
            kingAttackersCount[Us]++;
            kingAttackersWeight[Us] += KingAttackWeights[Pt];
            kingAttacksCount[Us] += popcount(attacks & attackedBy[Them][KING]);
        }
    }
}

// Returns the penalty for the danger to the king of the given color: the
// number of enemy king attackers times their total weight, plus the attacks on
// the squares next to the king, less if the enemy has no queen. The danger
// turns into a penalty once it is over 100, growing with its square in the
// middlegame.
template<Color Us>
TaperedScore KingAttacks::king() const {

    constexpr Color Them = ~Us;

    if (!pos.pieces(Us, KING))
        return SCORE_ZERO;

    int kingDanger = kingAttackersCount[Them] * kingAttackersWeight[Them]
                   + 69 * kingAttacksCount[Them] - 873 * !pos.count<QUEEN>(Them) + 37;

    // Transform the kingDanger units into a TaperedScore
    if (kingDanger > 100)
        return -make_score(kingDanger * kingDanger / 4096, kingDanger / 16);

    return SCORE_ZERO;
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
// and piece-square tables, pawn-defended pieces, the attacks on both kings, and
// the danger to the king of the side to move, tapered by the game phase.
Value Eval::evaluate(const Position& pos) {

    int          phase  = game_phase(pos);
    int          danger = king_danger(pos);
    TaperedScore score  = pos.psq_score()
                       + PawnDefended * (pawn_defended<WHITE>(pos) - pawn_defended<BLACK>(pos))
                       + KingAttacks(pos).value();
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
    ss << "King attacks             mg " << mg_value(KingAttacks(pos).value()) << ", eg "
       << eg_value(KingAttacks(pos).value()) << " (white side, internal units)\n";
    ss << "King danger (duck shield) " << king_danger(pos) << " for the side to move, penalty "
       << interpolate(DuckShield * (king_danger(pos) * king_danger(pos)), phase)
       << " (internal units)\n";
    ss << "Final evaluation         " << v << " (white side, internal units)\n";
    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2);
    ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";

    return ss.str();
}

}  // namespace Stockfish
