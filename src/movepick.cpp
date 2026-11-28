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

#include "movepick.h"

#include <algorithm>
#include <cassert>

#include "bitboard.h"
#include "position.h"

namespace Stockfish {

namespace {

// Victim values for ordering the captures, wakwak's
constexpr int MvvValue[PIECE_TYPE_NB] = {0, 94, 328, 333, 501, 906, 20000, 0};

int mvv(const Position& pos, Move m) {
    int v = m.type_of() == EN_PASSANT ? MvvValue[PAWN] : MvvValue[type_of(pos.piece_on(m.to_sq()))];

    if (m.type_of() == PROMOTION)
        v += MvvValue[m.promotion_type()] - MvvValue[PAWN];

    return v;
}

// Moves with equal scores are ordered by the move itself, so that the order
// is the same with every standard library (std::sort is not stable)
bool greater(const ExtMove& a, const ExtMove& b) {
    return a.value > b.value || (a.value == b.value && a.raw() > b.raw());
}

}  // namespace

int quiet_piece_history(
  const Histories& h, const Position& pos, const ContinuationMoves& cm, Bitboard threats, Move m) {

    Color  us   = pos.side_to_move();
    Square from = m.from_sq(), to = m.to_sq();
    int    pc = piece_index(pos.piece_on(from));

    int v = h.quiet[us][from][to][bool(threats & from)][bool(threats & to)] * 986
          + h.pawn[us][pos.pawn_key() % PawnHistorySize][pc][to] * 1024;

    if (cm.ply1.ok())
        v += h.cont[0][us][cm.ply1.piece][cm.ply1.to][pc][to] * 1012;
    if (cm.ply2.ok())
        v += h.cont[1][us][cm.ply2.piece][cm.ply2.to][pc][to] * 1055;
    if (cm.ply4.ok())
        v += h.cont[1][us][cm.ply4.piece][cm.ply4.to][pc][to] * 1035;

    return v;
}


MovePicker::MovePicker(const Position&          p,
                       Move                     ttm,
                       MoveStack&               ms,
                       const Histories&         h,
                       const ContinuationMoves& cm,
                       Bitboard                 th,
                       Bitboard                 neutral,
                       bool                     pruneQuiet,
                       bool                     pruneNoisy,
                       int                      see,
                       bool                     qsearch) :
    pos(p),
    stack(ms),
    histories(h),
    contMoves(cm),
    ttMove(ttm && p.legal(ttm) ? ttm : Move::none()),
    threats(th),
    neutralDucks(neutral),
    pruneQuietNeutrals(pruneQuiet),
    pruneNoisyNeutrals(pruneNoisy),
    seeThreshold(see),
    skipQuiets(qsearch),
    skipBadNoisy(qsearch),
    qsearchMode(qsearch),
    stage(TT),
    moves(ms.moves.get() + ms.top),
    start(ms.top) {}

// Generates the chess moves of a kind, each with every duck square, and
// scores them. A capture or promotion is scored by its victim and capture
// history, a quiet move by its histories, and the duck square by its own.
template<GenType Type>
void MovePicker::generate(ExtMove*) {

    Color us    = pos.side_to_move();
    bool  prune = Type == CAPTURES ? pruneNoisyNeutrals : pruneQuietNeutrals;

    for (Move m : MoveList<Type>(pos))
    {
        // Leave room for the moves of the nodes below
        if (start + size + SQUARE_NB >= MoveStack::Size - (1 << 15))
            break;

        Bitboard ducks = pos.duck_targets(m);

        // Of the ducks that restrict no opponent's move, keep the best one
        if (prune && (ducks & neutralDucks))
        {
            Bitboard n    = ducks & neutralDucks;
            Square   best = lsb(n);

            for (Bitboard b = n; b;)
            {
                Square s = pop_lsb(b);
                if (histories.duck[us][s] >= histories.duck[us][best])
                    best = s;
            }

            ducks &= ~neutralDucks | square_bb(best);
        }

        // The quiescence search tries at most two duck squares per chess move,
        // the best ones by duck history, so generate only a few of them
        if (qsearchMode && popcount(ducks) > 4)
        {
            Square best[4];
            int    n = 0;

            for (Bitboard b = ducks; b;)
            {
                Square s = pop_lsb(b);
                int    i = std::min(n, 3);

                if (n == 4 && histories.duck[us][s] <= histories.duck[us][best[3]])
                    continue;

                for (; i > 0 && histories.duck[us][s] > histories.duck[us][best[i - 1]]; --i)
                    best[i] = best[i - 1];

                best[i] = s;
                n       = std::min(n + 1, 4);
            }

            ducks = square_bb(best[0]) | best[1] | best[2] | best[3];
        }

        Square to = m.to_sq();
        int    pc = piece_index(pos.piece_on(m.from_sq()));
        int    pieceScore;

        if constexpr (Type == CAPTURES)
        {
            int victim = m.type_of() == EN_PASSANT ? 0 : int(type_of(pos.piece_on(to)));
            pieceScore = mvv(pos, m) * 8 + histories.noisy[us][pc][to][victim] * 144 / 1024;
        }
        else
            pieceScore = quiet_piece_history(histories, pos, contMoves, threats, m);

        while (ducks)
        {
            Square s  = pop_lsb(ducks);
            Move   dm = m.with_duck(s);

            if (dm == ttMove)
                continue;

            ExtMove& e = moves[size++];
            e          = dm;

            if constexpr (Type == CAPTURES)
                e.value = pieceScore + histories.duck[us][s] * 146 / 1024;
            else
                e.value = (pieceScore + histories.duck[us][s] * 982
                           + histories.quietDuck[us][pc][to][s] * 946)
                          / 1024
                        - 5079 * bool(neutralDucks & s);
        }
    }

    stack.top = start + size;
}

// Sorts the moves still to be picked, a part at a time. The first call of a
// stage spreads the moves in buckets by score, best first, in linear time;
// each bucket is sorted when the picker reaches it. The search prunes most
// moves of a node, but picks them in order, so a full sort would be wasted.
void MovePicker::sort_next(int end) {

    if (cursor != sortedEnd)
        return;

    ExtMove* first = moves + cursor;
    int      n     = end - cursor;

    if (cursor == sortStart)
    {
        bucketIdx = bucketCount = 0;

        if (n > 64)
        {
            int lo = first[0].value, hi = first[0].value;
            for (int i = 1; i < n; ++i)
            {
                lo = std::min(lo, first[i].value);
                hi = std::max(hi, first[i].value);
            }

            // Bucket 0 holds the best moves
            constexpr int Buckets = 256;
            auto          bucket  = [&](int v) {
                return int(i64(hi - v) * (Buckets - 1) / std::max(1, hi - lo));
            };

            int count[Buckets + 1] = {};
            for (int i = 0; i < n; ++i)
                count[bucket(first[i].value) + 1]++;
            for (int b = 0; b < Buckets; ++b)
                count[b + 1] += count[b];

            // The scratch space past our moves on the stack
            ExtMove* tmp = moves + size;
            for (int i = 0; i < n; ++i)
                tmp[count[bucket(first[i].value)]++] = first[i];
            std::copy(tmp, tmp + n, first);

            // count[b] is now the end of bucket b
            for (int b = 0; b < Buckets; ++b)
                if (bucketCount == 0 || count[b] != bucketEnds[bucketCount - 1])
                    bucketEnds[bucketCount++] = cursor + count[b];
        }
    }

    if (bucketIdx < bucketCount)
    {
        int bEnd = bucketEnds[bucketIdx++];
        std::sort(first, moves + bEnd, greater);
        sortedEnd = bEnd;
    }
    else
    {
        std::sort(first, moves + end, greater);
        sortedEnd = end;
    }
}

// Returns a new move every time it is called, until there are no more moves
// left, when Move::none() is returned
Move MovePicker::next_move() {

    switch (stage)
    {
    case TT :
        stage = GENERATE_NOISY;
        if (ttMove)
            return ttMove;
        [[fallthrough]];

    case GENERATE_NOISY :
        generate<CAPTURES>(moves);
        stage  = GOOD_NOISY;
        cursor = sortStart = sortedEnd = 0;
        [[fallthrough]];

    case GOOD_NOISY :
        while (cursor < size)
        {
            sort_next(size);
            Move m = moves[cursor++];

            if (pos.see_ge_duck(m, seeThreshold))
                return m;

            // Keep the bad ones at the front, for later
            std::swap(moves[badCount++], moves[cursor - 1]);
        }
        stage = GENERATE_QUIET;
        [[fallthrough]];

    case GENERATE_QUIET :
        if (!skipQuiets)
        {
            cursor = sortStart = sortedEnd = size;
            generate<QUIETS>(moves);
        }
        stage = QUIET;
        [[fallthrough]];

    case QUIET :
        if (!skipQuiets && cursor < size)
        {
            sort_next(size);
            return moves[cursor++];
        }
        stage  = BAD_NOISY;
        cursor = 0;
        [[fallthrough]];

    case BAD_NOISY :
        if (!skipBadNoisy && cursor < badCount)
            return moves[cursor++];

        stage = FINISHED;
        [[fallthrough]];

    case FINISHED :
        return Move::none();
    }

    assert(false);
    return Move::none();
}

}  // namespace Stockfish
