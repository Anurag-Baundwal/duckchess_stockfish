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

// ThreatByMinor/ByRook[attacked PieceType] contains bonuses according to
// which piece type attacks which one. Attacks on lesser pieces which are
// pawn-defended are not considered. These are Stockfish 11's values.
constexpr TaperedScore ThreatByMinor[PIECE_TYPE_NB] = {make_score(0, 0),    make_score(6, 32),
                                                       make_score(59, 41),  make_score(79, 56),
                                                       make_score(90, 119), make_score(79, 161)};

constexpr TaperedScore ThreatByRook[PIECE_TYPE_NB] = {make_score(0, 0),   make_score(3, 44),
                                                      make_score(38, 71), make_score(38, 61),
                                                      make_score(0, 38),  make_score(51, 38)};

// Bonus for our king attacking a weak enemy piece, and for an attacked enemy
// piece that is not defended, or attacked twice and defended once.
// Stockfish 11's values.
constexpr TaperedScore ThreatByKing = make_score(24, 89);
constexpr TaperedScore Hanging      = make_score(69, 36);

// Pawn-defended pieces, as in wakwak: returns how many pieces of the given
// color, pawns and king included, stand on squares that its own pawns attack.
template<Color Us>
int pawn_defended(const Position& pos) {
    return popcount(pawn_attacks_bb<Us>(pos.pieces(Us, PAWN)) & pos.pieces(Us));
}

// Passed pawns, as in Stockfish 11: returns the bonus for the pawns of the
// given color that no enemy pawn stands in front of or next to on their way to
// promotion, by their rank, less a little for each file away from the edge. From
// the fourth rank on, the bonus grows in the endgame the further the enemy king
// is from the square in front of the pawn, and the closer our king is. A passed
// pawn with another pawn in front of it gets half. The duck doesn't count, as it
// moves every turn.
template<Color Us>
TaperedScore passed_pawns(const Position& pos) {

    constexpr Color     Them = ~Us;
    constexpr Direction Up   = pawn_push(Us);

    // The distance of a king to a square, at most 5
    auto king_proximity = [&](Color c, Square s) {
        return std::min(distance(pos.square<KING>(c), s), 5);
    };

    // A side may have lost its king, and the game ends then
    bool bothKings = pos.pieces(WHITE, KING) && pos.pieces(BLACK, KING);

    TaperedScore score = SCORE_ZERO;

    for (Bitboard b = pos.pieces(Us, PAWN); b;)
    {
        Square s = pop_lsb(b);

        if (!pos.pawn_passed(Us, s))
            continue;

        int          r     = relative_rank(Us, s);
        TaperedScore bonus = PassedRank[r];

        if (r > RANK_3 && bothKings)
        {
            int    w       = 5 * r - 13;
            Square blockSq = s + Up;

            // Adjust bonus based on the king's proximity
            bonus += make_score(
              0, ((king_proximity(Them, blockSq) * 19) / 4 - king_proximity(Us, blockSq) * 2) * w);

            // If blockSq is not the queening square then consider also a second push
            if (r != RANK_7)
                bonus -= make_score(0, king_proximity(Us, blockSq + Up) * w);
        }

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
// and scores the threats on enemy pieces with them. The duck is lifted, as it
// moves every turn and either side may place it next. Nothing is pinned in duck
// chess, where a king may be left attacked, so every piece attacks all its
// squares.
class Evaluation {
   public:
    Evaluation()                             = delete;
    Evaluation(const Evaluation&)            = delete;
    Evaluation& operator=(const Evaluation&) = delete;

    explicit Evaluation(const Position& p);
    TaperedScore value() const { return threats<WHITE>() - threats<BLACK>(); }

   private:
    template<Color Us>
    void initialize();
    template<Color Us, PieceType Pt>
    void pieces();
    template<Color Us>
    TaperedScore threats() const;

    const Position& pos;
    Bitboard        occupied;

    // attackedBy[color][piece type] is a bitboard representing all squares
    // attacked by a given color and piece type. Special "piece types" which
    // is also calculated is ALL_PIECES.
    Bitboard attackedBy[COLOR_NB][PIECE_TYPE_NB];

    // attackedBy2[color] are the squares attacked by at least 2 units of a given
    // color, including x-rays. But diagonal x-rays through pawns are not computed.
    Bitboard attackedBy2[COLOR_NB];
};

Evaluation::Evaluation(const Position& p) :
    pos(p),
    occupied(p.duck_square() != SQ_NONE ? p.pieces() ^ p.duck_square() : p.pieces()) {

    initialize<WHITE>();
    initialize<BLACK>();

    // Add the attacks of the other pieces
    pieces<WHITE, KNIGHT>();
    pieces<BLACK, KNIGHT>();
    pieces<WHITE, BISHOP>();
    pieces<BLACK, BISHOP>();
    pieces<WHITE, ROOK>();
    pieces<BLACK, ROOK>();
    pieces<WHITE, QUEEN>();
    pieces<BLACK, QUEEN>();
}

// Computes the king and pawn attacks of the given color
template<Color Us>
void Evaluation::initialize() {

    Bitboard dblAttackByPawn = pawn_double_attacks_bb<Us>(pos.pieces(Us, PAWN));

    // Initialize attackedBy[] for king and pawns
    attackedBy[Us][KING] =
      pos.pieces(Us, KING) ? Attacks::attacks_bb(KING, pos.square<KING>(Us)) : Bitboard(0);
    attackedBy[Us][PAWN]       = pawn_attacks_bb<Us>(pos.pieces(Us, PAWN));
    attackedBy[Us][ALL_PIECES] = attackedBy[Us][KING] | attackedBy[Us][PAWN];
    attackedBy2[Us]            = dblAttackByPawn | (attackedBy[Us][KING] & attackedBy[Us][PAWN]);
}

// Adds the attacks of the pieces of the given color and type. Bishops see
// through queens, and rooks through queens and our rooks.
template<Color Us, PieceType Pt>
void Evaluation::pieces() {

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
    }
}

// Threats, as in Stockfish 11: returns the bonus for the enemy pieces that the
// given color attacks. A piece attacked by our knight or bishop gets
// ThreatByMinor by its type, unless it is a pawn defended by a pawn. A weak
// piece, one the enemy doesn't strongly protect (with a pawn, or twice when we
// attack it only once), gets ThreatByRook by its type when our rook attacks it,
// ThreatByKing when our king does, and Hanging when the enemy doesn't defend
// it at all or it is not a pawn and we attack it twice. The enemy king is left
// out: an attack on it is the duck shield's business.
template<Color Us>
TaperedScore Evaluation::threats() const {

    constexpr Color Them = ~Us;

    Bitboard enemies = pos.pieces(Them) & ~pos.pieces(KING);

    // Non-pawn enemies
    Bitboard nonPawnEnemies = enemies & ~pos.pieces(PAWN);

    // Squares strongly protected by the enemy, either because they defend the
    // square with a pawn, or because they defend the square twice and we don't.
    Bitboard stronglyProtected = attackedBy[Them][PAWN] | (attackedBy2[Them] & ~attackedBy2[Us]);

    // Non-pawn enemies, strongly protected
    Bitboard defended = nonPawnEnemies & stronglyProtected;

    // Enemies not strongly protected and under our attack
    Bitboard weak = enemies & ~stronglyProtected & attackedBy[Us][ALL_PIECES];

    TaperedScore score = SCORE_ZERO;

    // Bonus according to the kind of attacking pieces
    if (defended | weak)
    {
        Bitboard b = (defended | weak) & (attackedBy[Us][KNIGHT] | attackedBy[Us][BISHOP]);
        while (b)
            score += ThreatByMinor[type_of(pos.piece_on(pop_lsb(b)))];

        b = weak & attackedBy[Us][ROOK];
        while (b)
            score += ThreatByRook[type_of(pos.piece_on(pop_lsb(b)))];

        if (weak & attackedBy[Us][KING])
            score += ThreatByKing;

        b = ~attackedBy[Them][ALL_PIECES] | (nonPawnEnemies & attackedBy2[Us]);
        score += Hanging * popcount(weak & b);
    }

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
// shelter of the kings, threats on enemy pieces, and the danger to the king of
// the side to move, tapered by the game phase.
Value Eval::evaluate(const Position& pos) {

    int          phase  = game_phase(pos);
    int          danger = king_danger(pos);
    TaperedScore score =
      pos.psq_score() + PawnDefended * (pawn_defended<WHITE>(pos) - pawn_defended<BLACK>(pos))
      + passed_pawns<WHITE>(pos) - passed_pawns<BLACK>(pos) + king_shelter<WHITE>(pos)
      - king_shelter<BLACK>(pos) + Evaluation(pos).value();
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
    ss << "Threats                  mg " << mg_value(Evaluation(pos).value()) << ", eg "
       << eg_value(Evaluation(pos).value()) << " (white side, internal units)\n";
    ss << "King danger (duck shield) " << king_danger(pos) << " for the side to move, penalty "
       << interpolate(DuckShield * (king_danger(pos) * king_danger(pos)), phase)
       << " (internal units)\n";
    ss << "Final evaluation         " << v << " (white side, internal units)\n";
    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2);
    ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";

    return ss.str();
}

}  // namespace Stockfish
