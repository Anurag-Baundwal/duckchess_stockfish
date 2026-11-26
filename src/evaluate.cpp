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

// PassedRank[Rank] contains the bonus for a passed pawn by its rank, and
// PassedFile the penalty per file between it and the edge of the board. These
// are Stockfish 11's values.
constexpr TaperedScore PassedRank[RANK_NB] = {
  make_score(0, 0),   make_score(10, 28),   make_score(17, 33),  make_score(15, 41),
  make_score(62, 72), make_score(168, 177), make_score(276, 260)};
constexpr TaperedScore PassedFile = make_score(11, 8);

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

// KingAttackWeights[PieceType] contains the king attack weights by piece type.
// These are Stockfish 11's values.
constexpr int KingAttackWeights[PIECE_TYPE_NB] = {0, 0, 81, 52, 44, 10};

// Pawn-defended pieces, as in wakwak: returns how many pieces of the given
// color, pawns and king included, stand on squares that its own pawns attack.
template<Color Us>
int pawn_defended(const Position& pos) {
    return popcount(pawn_attacks_bb<Us>(pos.pieces(Us, PAWN)) & pos.pieces(Us));
}

// Passed pawns, as in Stockfish 11: returns the bonus for the pawns of the
// given color that no enemy pawn stands in front of or next to on their way to
// promotion, by their rank, less a little for each file away from the edge. A
// passed pawn with another pawn in front of it gets half. The duck doesn't
// count, as it moves every turn.
template<Color Us>
TaperedScore passed_pawns(const Position& pos) {

    TaperedScore score = SCORE_ZERO;

    for (Bitboard b = pos.pieces(Us, PAWN); b;)
    {
        Square s = pop_lsb(b);

        if (!pos.pawn_passed(Us, s))
            continue;

        TaperedScore bonus = PassedRank[relative_rank(Us, s)];

        // Scale down the bonus when a pawn stands in front of it
        if (pos.pieces(PAWN) & forward_file_bb(Us, s))
            bonus = make_score(mg_value(bonus) / 2, eg_value(bonus) / 2);

        score += bonus - PassedFile * edge_distance(file_of(s));
    }

    return score;
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

// Evaluation, as in Stockfish 11, computes the squares that each side attacks
// and scores the kings with them: the pawn shelter, and a penalty for the
// enemy pieces aiming at the squares around the king. The duck is lifted, as it
// moves every turn and either side may place it next. Nothing is pinned in duck
// chess, where a king may be left attacked, so every piece attacks all its
// squares.
class Evaluation {
   public:
    Evaluation()                             = delete;
    Evaluation(const Evaluation&)            = delete;
    Evaluation& operator=(const Evaluation&) = delete;

    explicit Evaluation(const Position& p);
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

    // attackedBy[color][piece type] is a bitboard representing all squares
    // attacked by a given color and piece type. Special "piece types" which
    // is also calculated is ALL_PIECES.
    Bitboard attackedBy[COLOR_NB][PIECE_TYPE_NB];

    // attackedBy2[color] are the squares attacked by at least 2 units of a given
    // color, including x-rays. But diagonal x-rays through pawns are not computed.
    Bitboard attackedBy2[COLOR_NB];

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

Evaluation::Evaluation(const Position& p) :
    pos(p),
    occupied(p.duck_square() != SQ_NONE ? p.pieces() ^ p.duck_square() : p.pieces()) {

    initialize<WHITE>();
    initialize<BLACK>();

    // Add the attacks of the other pieces, counting the king attackers
    pieces<WHITE, KNIGHT>();
    pieces<BLACK, KNIGHT>();
    pieces<WHITE, BISHOP>();
    pieces<BLACK, BISHOP>();
    pieces<WHITE, ROOK>();
    pieces<BLACK, ROOK>();
    pieces<WHITE, QUEEN>();
    pieces<BLACK, QUEEN>();
}

// Computes the king and pawn attacks and the king ring of the given color
template<Color Us>
void Evaluation::initialize() {

    constexpr Color Them = ~Us;

    Bitboard dblAttackByPawn = pawn_double_attacks_bb<Us>(pos.pieces(Us, PAWN));

    // Initialize attackedBy[] for king and pawns
    attackedBy[Us][KING] =
      pos.pieces(Us, KING) ? Attacks::attacks_bb(KING, pos.square<KING>(Us)) : Bitboard(0);
    attackedBy[Us][PAWN]       = pawn_attacks_bb<Us>(pos.pieces(Us, PAWN));
    attackedBy[Us][ALL_PIECES] = attackedBy[Us][KING] | attackedBy[Us][PAWN];
    attackedBy2[Us]            = dblAttackByPawn | (attackedBy[Us][KING] & attackedBy[Us][PAWN]);

    kingAttacksCount[Them] = kingAttackersWeight[Them] = 0;

    // A side may have lost its king, and the game ends then
    if (!pos.pieces(Us, KING))
    {
        kingRing[Us]             = 0;
        kingAttackersCount[Them] = 0;
        return;
    }

    // Init our king safety tables
    Square ksq   = pos.square<KING>(Us);
    Square s     = make_square(std::clamp(file_of(ksq), FILE_B, FILE_G),
                               std::clamp(rank_of(ksq), RANK_2, RANK_7));
    kingRing[Us] = Attacks::attacks_bb(KING, s) | s;

    kingAttackersCount[Them] =
      popcount(kingRing[Us] & pawn_attacks_bb<Them>(pos.pieces(Them, PAWN)));

    // Remove from kingRing[] the squares defended by two pawns
    kingRing[Us] &= ~dblAttackByPawn;
}

// Adds the attacks of the pieces of the given color and type, and counts those
// that attack the enemy king ring. Bishops see through queens, and rooks
// through queens and our rooks.
template<Color Us, PieceType Pt>
void Evaluation::pieces() {

    constexpr Color Them = ~Us;

    attackedBy[Us][Pt] = 0;

    for (Bitboard bb = pos.pieces(Us, Pt); bb;)
    {
        Square s = pop_lsb(bb);

        // Find attacked squares, including x-ray attacks for bishops and rooks
        Bitboard b =
          Pt == BISHOP ? Attacks::attacks_bb(BISHOP, s, occupied ^ pos.pieces(QUEEN))
          : Pt == ROOK
            ? Attacks::attacks_bb(ROOK, s, occupied ^ pos.pieces(QUEEN) ^ pos.pieces(Us, ROOK))
            : Attacks::attacks_bb(Pt, s, occupied);

        attackedBy2[Us] |= attackedBy[Us][ALL_PIECES] & b;
        attackedBy[Us][Pt] |= b;
        attackedBy[Us][ALL_PIECES] |= b;

        if (b & kingRing[Them])
        {
            kingAttackersCount[Us]++;
            kingAttackersWeight[Us] += KingAttackWeights[Pt];
            kingAttacksCount[Us] += popcount(b & attackedBy[Them][KING]);
        }
    }
}

// Scores the king of the given color: the pawn shelter (king_shelter()), less a
// penalty for the king danger, as in Stockfish 11. The danger counts the enemy
// pieces attacking the king ring, by number and weight, the weak squares in the
// ring (attacked by the enemy and defended at most once, by our king or queen
// alone), and the attacks on the squares next to the king. It is lower when the
// enemy has no queen or our shelter is good. Over 100 it becomes a penalty that
// grows with its square in the middlegame.
template<Color Us>
TaperedScore Evaluation::king() const {

    constexpr Color Them = ~Us;

    // Init the score with king shelter and enemy pawns storm
    TaperedScore score = king_shelter<Us>(pos);

    if (!pos.pieces(Us, KING))
        return score;

    // Attacked squares defended at most once by our queen or king
    Bitboard weak = attackedBy[Them][ALL_PIECES] & ~attackedBy2[Us]
                  & (~attackedBy[Us][ALL_PIECES] | attackedBy[Us][KING] | attackedBy[Us][QUEEN]);

    int kingDanger = kingAttackersCount[Them] * kingAttackersWeight[Them]
                   + 185 * popcount(kingRing[Us] & weak) + 69 * kingAttacksCount[Them]
                   - 873 * !pos.count<QUEEN>(Them) - 6 * mg_value(score) / 8 + 37;

    // Transform the kingDanger units into a TaperedScore, and subtract it from the evaluation
    if (kingDanger > 100)
        score -= make_score(kingDanger * kingDanger / 4096, kingDanger / 16);

    return score;
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
// and piece-square tables, pawn-defended pieces, passed pawns, the pawn
// shelter of the kings and the attacks on them, and the danger to the king of
// the side to move, tapered by the game phase.
Value Eval::evaluate(const Position& pos) {

    int          phase  = game_phase(pos);
    int          danger = king_danger(pos);
    TaperedScore score =
      pos.psq_score() + PawnDefended * (pawn_defended<WHITE>(pos) - pawn_defended<BLACK>(pos))
      + passed_pawns<WHITE>(pos) - passed_pawns<BLACK>(pos) + Evaluation(pos).value();
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
    ss << "Passed pawns             mg "
       << mg_value(passed_pawns<WHITE>(pos) - passed_pawns<BLACK>(pos)) << ", eg "
       << eg_value(passed_pawns<WHITE>(pos) - passed_pawns<BLACK>(pos))
       << " (white side, internal units)\n";
    ss << "King shelter             mg "
       << mg_value(king_shelter<WHITE>(pos) - king_shelter<BLACK>(pos)) << ", eg "
       << eg_value(king_shelter<WHITE>(pos) - king_shelter<BLACK>(pos))
       << " (white side, internal units)\n";
    TaperedScore kingAttacks =
      Evaluation(pos).value() - (king_shelter<WHITE>(pos) - king_shelter<BLACK>(pos));
    ss << "King attacks             mg " << mg_value(kingAttacks) << ", eg "
       << eg_value(kingAttacks) << " (white side, internal units)\n";
    ss << "King danger (duck shield) " << king_danger(pos) << " for the side to move, penalty "
       << interpolate(DuckShield * (king_danger(pos) * king_danger(pos)), phase)
       << " (internal units)\n";
    ss << "Final evaluation         " << v << " (white side, internal units)\n";
    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2);
    ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";

    return ss.str();
}

}  // namespace Stockfish
