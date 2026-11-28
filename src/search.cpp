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

#include "search.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>

#include "bitboard.h"
#include "evaluate.h"
#include "history.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "position.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "types.h"
#include "uci.h"
#include "ucioption.h"

namespace Stockfish {

using namespace Search;

namespace {

constexpr u64 NODES_LIMIT_OUTPUT = 10'000'000;

constexpr int SEARCHEDLIST_CAPACITY = 256;
using SearchedList                  = ValueList<Move, SEARCHEDLIST_CAPACITY>;

// Add a small random component to draw evaluations to avoid 3-fold blindness
Value value_draw(usize nodes) { return VALUE_DRAW - 1 + Value(nodes & 0x2); }

// Reverse futility margin, wakwak's
Value futility_margin(Depth d, bool improving) {
    return Value((improving ? -42 : -5) + d * (35 + 6 * d));
}

// Late move reduction in 1/1024 ply for a quiet move, before adjustments, from
// the depth and the number of chess moves searched so far, wakwak's
int base_reduction(Depth d, int uniqueMoves) {
    auto log2 = [](int x) { return 31 - __builtin_clz(unsigned(x)); };
    return 1011 + 97 * log2(d) * log2(uniqueMoves + 1);
}

// Whether a piece of the opponent attacks the king of the side to move, with
// the duck lifted, as the side to move has to move it and may open the line.
// This is duck chess's counterpart of being in check.
bool king_attacked(const Position& pos) {

    Color    us       = pos.side_to_move();
    Bitboard occupied = pos.pieces();

    if (pos.duck_square() != SQ_NONE)
        occupied ^= pos.duck_square();

    return pos.attackers_to(pos.square<KING>(us), occupied) & pos.pieces(~us);
}

// Called after we made a move: returns the squares where our duck keeps our
// king safe from capture, given the chess move we played. The answer is the
// same for every duck square of that chess move, so the search computes it
// once per chess move.
Bitboard safe_ducks(const Position& pos) {

    Color    them     = pos.side_to_move();
    Square   ksq      = pos.square<KING>(~them);
    Bitboard occupied = pos.pieces() ^ pos.duck_square();  // Lift the duck
    Bitboard safe     = ~Bitboard(0);

    // If we captured their king, the game is over and every square is safe
    if (!pos.pieces(them, KING))
        return safe;

    // The duck must block every attacker, and it can only block a slider, by
    // standing between it and our king. between_bb() also includes the
    // attacker's square, where the duck can't go, so an attack by a knight,
    // pawn or king leaves no safe square.
    for (Bitboard attackers = pos.attackers_to(ksq, occupied) & pos.pieces(them); attackers;)
        safe &= Attacks::between_bb(ksq, pop_lsb(attackers));

    return safe;
}

// Returns the squares where a duck stops move m of color c: the squares its
// piece passes through and lands on (both the king's and the rook's when
// castling), and m's own duck square, since the duck can't stay where it is.
Bitboard blocking_ducks(Move m, Color c) {
    Square from = m.from_sq();
    Square to   = m.to_sq();

    if (m.type_of() == CASTLING)
    {
        // Castling is encoded as "king captures friendly rook"
        Square kto = relative_square(c, to > from ? SQ_G1 : SQ_C1);
        Square rto = relative_square(c, to > from ? SQ_F1 : SQ_D1);

        return Attacks::between_bb(from, kto) | Attacks::between_bb(to, rto) | m.duck_sq();
    }

    return Attacks::between_bb(from, to) | m.duck_sq();
}

// The key of the rooks, queens and kings, for the major piece correction history
Key major_key(const Position& pos) {
    return pos.non_pawn_key(WHITE) ^ pos.non_pawn_key(BLACK) ^ pos.minor_piece_key();
}

// Correction histories, as in wakwak: how far the search results were from
// the static evaluation in positions with the same pawns, minor pieces, major
// pieces, non-pawn pieces of each color, and last moves.
int correction_value(const Histories& h, const Position& pos, const Stack* ss) {

    constexpr int Size = CorrectionHistorySize;

    Color          us = pos.side_to_move();
    const PieceTo &p1 = (ss - 1)->pieceTo, &p2 = (ss - 2)->pieceTo, &p3 = (ss - 3)->pieceTo;

    int v = 64 * h.pawnCorr[us][pos.pawn_key() % Size]
          + 65 * h.minorCorr[us][pos.minor_piece_key() % Size]
          + 62 * h.majorCorr[us][major_key(pos) % Size]
          + 66 * h.nonPawnCorr[WHITE][us][pos.non_pawn_key(WHITE) % Size]
          + 66 * h.nonPawnCorr[BLACK][us][pos.non_pawn_key(BLACK) % Size];

    if (p1.ok() && p2.ok())
        v += 62 * h.contCorr[0][us][p2.piece][p2.to][p1.piece][p1.to];
    if (p1.ok() && p3.ok())
        v += 64 * h.contCorr[1][us][p3.piece][p3.to][p1.piece][p1.to];

    return v / MaxCorrection;
}

void update_correction_history(
  Histories& h, const Position& pos, const Stack* ss, Depth depth, int diff) {

    constexpr int Size = CorrectionHistorySize;

    Color          us    = pos.side_to_move();
    int            bonus = int(i64(diff) * depth * 129 / 1024);
    const PieceTo &p1 = (ss - 1)->pieceTo, &p2 = (ss - 2)->pieceTo, &p3 = (ss - 3)->pieceTo;

    auto update = [bonus](std::int16_t& e) { gravity<MaxCorrection / 4, MaxCorrection>(e, bonus); };

    update(h.pawnCorr[us][pos.pawn_key() % Size]);
    update(h.minorCorr[us][pos.minor_piece_key() % Size]);
    update(h.majorCorr[us][major_key(pos) % Size]);
    update(h.nonPawnCorr[WHITE][us][pos.non_pawn_key(WHITE) % Size]);
    update(h.nonPawnCorr[BLACK][us][pos.non_pawn_key(BLACK) % Size]);

    if (p1.ok() && p2.ok())
        update(h.contCorr[0][us][p2.piece][p2.to][p1.piece][p1.to]);
    if (p1.ok() && p3.ok())
        update(h.contCorr[1][us][p3.piece][p3.to][p1.piece][p1.to]);
}

Value corrected(Value v, int correction) {
    return std::clamp(v + correction, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

Value value_to_tt(Value v, int ply);
Value value_from_tt(Value v, int ply, int r50c);
void  update_all_stats(const Position& pos,
                       Search::Worker& workerThread,
                       Stack*          ss,
                       Move            bestMove,
                       SearchedList&   quietsSearched,
                       SearchedList&   capturesSearched,
                       const u8*       duckCounts,
                       Bitboard        threats,
                       Depth           depth);

}  // namespace

Search::Worker::Worker(SharedState&                   sharedState,
                       std::unique_ptr<SearchManager> sm,
                       usize                          threadId) :
    // Unpack the SharedState struct into member variables
    threadIdx(threadId),
    manager(std::move(sm)),
    options(sharedState.options),
    threads(sharedState.threads),
    tt(sharedState.tt) {
    clear();
}

void Search::Worker::start_searching() {

    // Non-main threads go directly to iterative_deepening()
    if (!is_mainthread())
    {
        iterative_deepening();
        return;
    }

    main_manager()->tm.init(limits, rootPos.side_to_move(), rootPos.game_ply(), options,
                            main_manager()->originalTimeAdjust);
    tt.new_search();
    main_manager()->updates.onStart();

    // Without a move, the side to move has won, unless its king was captured
    if (rootMoves.empty())
    {
        main_manager()->updates.onUpdateNoMoves(
          {0, {rootPos.pieces(rootPos.side_to_move(), KING) ? VALUE_MATE : -VALUE_MATE, rootPos}});
        main_manager()->updates.onBestmove(UCIEngine::move(Move::none()), "");
        return;
    }

    // Main thread starts non-main threads, and begins own search
    threads.start_searching();
    bool uciPvSent = iterative_deepening();

    // When we reach the maximum depth, we can arrive here without a raise of
    // threads.stop. However, if we are pondering or in an infinite search,
    // the UCI protocol states that we shouldn't print the best move before the
    // GUI sends a "stop" or "ponderhit" command. We therefore simply wait here
    // until the GUI sends one of those commands.
    while (!threads.stop && (main_manager()->ponder || limits.infinite))
    {}

    // Stop the threads if not already stopped (also raise the stop if "ponderhit"
    // just reset threads.ponder).
    threads.stop = true;

    // Wait until all threads have finished
    threads.wait_for_search_finished();

    // When playing in 'nodes as time' mode, subtract the searched nodes from
    // the available ones before exiting.
    if (limits.npmsec)
        main_manager()->tm.advance_nodes_time(threads.nodes_searched()
                                              - limits.inc[rootPos.side_to_move()]);

    Worker* bestThread = this;
    Skill   skill =
      Skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);

    if (!limits.depth && !skill.enabled())
        bestThread = threads.get_best_thread()->worker.get();

    main_manager()->bestPreviousScore        = bestThread->rootMoves[0].score;
    main_manager()->bestPreviousAverageScore = bestThread->rootMoves[0].averageScore;

    if (bestThread->rootMoves[0].pv.size() == 1
        && bestThread->rootMoves[0].extract_ponder_from_tt(tt, rootPos))
        uciPvSent = false;

    // Send PV info if it has changed since last output in iterative_deepening()
    if (!uciPvSent || bestThread != this)
        main_manager()->output_pv(*bestThread, threads, tt, bestThread->rootDepth);

    std::string ponder;
    if (bestThread->rootMoves[0].pv.size() > 1)
        ponder = UCIEngine::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());

    auto bestmove = UCIEngine::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());
    main_manager()->updates.onBestmove(bestmove, ponder);
}

// Main iterative deepening loop. It calls search() repeatedly with increasing
// depth until the allocated thinking time has been consumed, the user stops
// the search, or the maximum search depth is reached.
bool Search::Worker::iterative_deepening() {

    SearchManager* mainThread = (is_mainthread() ? main_manager() : nullptr);

    PVMoves pv;

    RootPVMoves lastBestMovePV;
    Depth       lastBestMoveDepth = 0;
    Value       lastBestMoveScore = -VALUE_INFINITE;

    // For the time management: the best move of the last iteration, and for
    // how many iterations it and its duck square have stayed the same
    Move lastIterationBest = Move::none();
    int  moveStability = 0, duckStability = 0;

    Value  bestValue     = -VALUE_INFINITE;
    double timeReduction = 1;
    int    iterIdx       = 0;

    // Allocate stack with extra size to allow access from (ss - 7) to (ss + 2)
    Stack  stack[MAX_PLY + 10] = {};
    Stack* ss                  = stack + 7;

    for (int i = 7; i > 0; --i)
        (ss - i)->staticEval = VALUE_NONE;

    for (int i = 0; i <= MAX_PLY + 2; ++i)
        (ss + i)->ply = i;

    accumulators.reset();

    ss->pv = &pv;

    if (mainThread)
    {
        if (mainThread->bestPreviousScore == VALUE_INFINITE)
            mainThread->iterValue.fill(VALUE_ZERO);
        else
            mainThread->iterValue.fill(mainThread->bestPreviousScore);
    }

    usize multiPV = usize(options["MultiPV"]);
    Skill skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);

    // When playing with strength handicap enable MultiPV search that we will
    // use behind-the-scenes to retrieve a set of possible moves.
    if (skill.enabled())
        multiPV = std::max(multiPV, usize(4));

    multiPV = std::min(multiPV, rootMoves.size());

    int  searchAgainCounter = 0;
    bool uciPvSent          = false;


    // Iterative deepening loop until requested to stop or the target depth is reached
    while (rootDepth + 1 < MAX_PLY && !threads.stop
           && !(limits.depth && mainThread && rootDepth >= limits.depth))
    {
        rootDepth++;

        // Signal the start of a new iteration
        if (mainThread)
            uciPvSent = false;

        // Save the last iteration's scores before the first PV line is searched and
        // all the move scores except the (new) PV are set to -VALUE_INFINITE.
        for (usize i = 0; i < rootMoves.size(); ++i)
        {
            rootMoves[i].previousScore      = rootMoves[i].score;
            rootMoves[i].previousPV         = rootMoves[i].pv;
            rootMoves[i].previousScoreExact = i < multiPV;
        }

        pvLast = rootMoves.size();

        if (!threads.increaseDepth)
            searchAgainCounter++;

        // MultiPV loop: we perform a full root search for each PV line
        for (pvIdx = 0; pvIdx < multiPV; ++pvIdx)
        {
            // Reset UCI info selDepth for each depth and each PV line
            selDepth = 0;

            // Reset aspiration window starting size, with wakwak's values
            Value avg   = rootMoves[pvIdx].averageScore;
            Value delta = 21;
            Value alpha = -VALUE_INFINITE, beta = VALUE_INFINITE;

            if (rootDepth >= 4 && avg != -VALUE_INFINITE)
            {
                alpha = std::max(avg - delta, -VALUE_INFINITE);
                beta  = std::min(avg + delta, VALUE_INFINITE);
            }

            // Adjust the effective depth searched, but ensure at least one
            // effective increment for every four searchAgain steps (see issue
            // #2717).
            Depth adjustedDepth = std::max(1, rootDepth - 3 * (searchAgainCounter + 1) / 4);

            // Start with a small aspiration window and, in the case of a fail
            // high/low, re-search with a bigger window until we don't fail
            // high/low anymore.
            while (true)
            {
                nmrPly    = -1;
                bestValue = search<Root>(rootPos, ss, alpha, beta, adjustedDepth);

                // Bring the best move to the front. It is critical that sorting
                // is done with a stable algorithm because all the values but the
                // first and eventually the new best one is set to -VALUE_INFINITE
                // and we want to keep the same order for all the moves except the
                // new PV that goes to the front. Note that in the case of MultiPV
                // search the already searched PV lines are preserved.
                std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

                // If search has been stopped, we break immediately. Sorting is
                // safe because RootMoves is still valid, although it refers to
                // the previous iteration.
                if (threads.stop)
                    break;

                // When failing high/low give some update before a re-search
                if (mainThread && multiPV == 1 && (bestValue <= alpha || bestValue >= beta)
                    && nodes > NODES_LIMIT_OUTPUT)
                    main_manager()->output_pv(*this, threads, tt, rootDepth);

                // In case of failing low/high increase aspiration window and re-search,
                // otherwise exit the loop.
                if (bestValue <= alpha)
                {
                    beta  = (alpha * 509 + beta * 515) / 1024;
                    alpha = std::max(bestValue - delta, -VALUE_INFINITE);
                }
                else if (bestValue >= beta)
                    beta = std::min(bestValue + delta, VALUE_INFINITE);
                else
                    break;

                delta += delta * 127 / 64;

                assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
            }

            if (threads.stop && pvIdx)
            {
                // In multiPV analysis we do not let aborted searches spoil
                // mated-in/TB loss scores from a completed search in an earlier
                // PV line. Hence we guard against an aborted pvIdx line overtaking
                // pvIdx - 1 when pvIdx - 1 is a proven loss. Moreover, we do not
                // trust an exact loss score from an aborted search.
                if ((is_loss(rootMoves[pvIdx - 1].score) && rootMoves[pvIdx] < rootMoves[pvIdx - 1])
                    || rootMoves[pvIdx].is_exact_loss())
                {
                    // If previousScore is exact and worse than pvIdx - 1, we
                    // can safely use it. If it is equal, we make sure it cannot
                    // overtake pvIdx - 1.
                    if (rootMoves[pvIdx].previousScore != -VALUE_INFINITE
                        && rootMoves[pvIdx].previousScoreExact
                        && rootMoves[pvIdx].previousScore <= rootMoves[pvIdx - 1].score)
                    {
                        rootMoves[pvIdx].score = rootMoves[pvIdx].uciScore =
                          rootMoves[pvIdx].previousScore;
                        rootMoves[pvIdx].previousScore = -VALUE_INFINITE;
                        rootMoves[pvIdx].pv            = rootMoves[pvIdx].previousPV;
                        rootMoves[pvIdx].unset_inexact();
                    }

                    // Otherwise, if we can, we cap the score to the best possible, and mark
                    // the score as inexact (also a valid excuse for the incomplete PV).
                    else
                    {
                        if (is_loss(rootMoves[pvIdx - 1].score))
                        {
                            rootMoves[pvIdx].score = rootMoves[pvIdx].uciScore =
                              rootMoves[pvIdx - 1].score;
                            rootMoves[pvIdx].previousScore = -VALUE_INFINITE;
                            rootMoves[pvIdx].pv.resize(1);
                            rootMoves[pvIdx].inexactUpper = true;
                        }
                        else
                            rootMoves[pvIdx].inexactUpper = false;

                        rootMoves[pvIdx].inexactLower = !rootMoves[pvIdx].inexactUpper;
                    }
                }

                // Finally, we mark all loss scores from partially searched moves as inexact.
                for (usize i = pvIdx + 1; i < multiPV; ++i)
                    if (rootMoves[i].is_exact_loss())
                        rootMoves[i].inexactLower = true;
            }

            // Sort the PV lines searched so far and update the GUI
            std::stable_sort(rootMoves.begin(), rootMoves.begin() + pvIdx + 1);

            if (mainThread && !threads.stop && (pvIdx + 1 == multiPV || nodes > NODES_LIMIT_OUTPUT))
            {
                main_manager()->output_pv(*this, threads, tt, rootDepth);
                uciPvSent = (pvIdx + 1 == multiPV);
            }

            if (threads.stop)
                break;
        }

        const bool forgottenMate = lastBestMoveScore != -VALUE_INFINITE
                                && is_mate_or_mated(lastBestMoveScore)
                                && (std::abs(rootMoves[0].score) < std::abs(lastBestMoveScore)
                                    || rootMoves[0].is_inexact());

        if (!threads.stop)
        {
            if (lastBestMovePV.empty() || lastBestMovePV[0] != rootMoves[0].pv[0])
                lastBestMoveDepth = rootDepth;

            // Do not replace (shorter) mate scores from a previous iteration
            if (!forgottenMate)
            {
                lastBestMovePV    = rootMoves[0].pv;
                lastBestMoveScore = rootMoves[0].score;
            }
        }

        const bool abortedLossSearch = threads.stop && !pvIdx && rootMoves[0].is_exact_loss();

        // An exact mated-in/TB-loss score from an aborted search cannot be
        // trusted: the loss could be delayed or refuted upon exploring the
        // remaining root-moves. Thus here we roll back to the score from the
        // previous iteration. We do the same if a search has failed to recover
        // a mate score that was found in a previous iteration.
        if (abortedLossSearch || (rootMoves[0].score != -VALUE_INFINITE && forgottenMate))
        {
            // Bring the last best move to the front for best thread selection
            if (!lastBestMovePV.empty())
            {
                Utility::move_to_front(rootMoves, [&lastPV = std::as_const(lastBestMovePV)](
                                                    const auto& rm) { return rm == lastPV[0]; });
                rootMoves[0].score = rootMoves[0].uciScore = lastBestMoveScore;
                rootMoves[0].pv                            = lastBestMovePV;
                rootMoves[0].unset_inexact();

                if (mainThread)
                    uciPvSent = false;
            }
            // For an aborted d1 search we label the loss score as inexact
            else if (abortedLossSearch)
                rootMoves[0].inexactLower = true;
        }

        // Have we found a "mate in x" after a completed iteration?
        if (limits.mate && !threads.stop && is_mate_or_mated(rootMoves[0].score)
            && VALUE_MATE - std::abs(rootMoves[0].score) <= 2 * limits.mate)
            threads.stop = true;

        if (!mainThread)
            continue;

        // If the skill level is enabled and time is up, pick a sub-optimal best move
        if (skill.enabled() && skill.time_to_pick(rootDepth))
            skill.pick_best(rootMoves, multiPV);

        // Do we have time for the next iteration? Can we stop searching now?
        if (limits.use_time_management() && !threads.stop && !mainThread->stopOnPonderhit)
        {
            // Time management as in wakwak: from depth 4, more time than the
            // optimum while the best move or its duck square keep changing
            // between iterations, less once they are stable.
            Move best         = rootMoves[0].pv[0];
            duckStability     = lastIterationBest && best.duck_sq() == lastIterationBest.duck_sq()
                                ? duckStability + 1
                                : 0;
            moveStability     = best == lastIterationBest ? moveStability + 1 : 0;
            lastIterationBest = best;

            for (auto&& th : threads)
                th->worker->bestMoveChanges = 0;

            double totalTime = double(mainThread->tm.optimum());

            if (rootDepth >= 4)
                totalTime = totalTime * std::max(5210 - 477 * duckStability, 2839) / 4096
                          * std::max(5470 - 505 * moveStability, 2881) / 4096;

            if (rootMoves.size() == 1)
                // Cap used time to 0.5s for a better viewer experience
                totalTime = std::min(500.0 * std::max(limits.npmsec, TimePoint(1)), totalTime);

            auto elapsedTime = elapsed();

            // Stop the search if we have exceeded totalTime or maximum time,
            // or if we know that there are no better moves in the analysed line(s).
            if (elapsedTime > std::min(totalTime, double(mainThread->tm.maximum()))
                || rootMoves[multiPV - 1].score >= mate_in(3) || rootMoves[0].score == mated_in(2))
            {
                // If we are allowed to ponder do not stop the search now but
                // keep pondering until the GUI sends "ponderhit" or "stop".
                if (mainThread->ponder)
                    mainThread->stopOnPonderhit = true;
                else
                    threads.stop = true;
            }
            else
                threads.increaseDepth = mainThread->ponder || elapsedTime <= totalTime * 0.50;
        }

        mainThread->iterValue[iterIdx] = bestValue;
        iterIdx                        = (iterIdx + 1) & 3;
    }

    if (!mainThread)
        return false;

    mainThread->previousTimeReduction = timeReduction;

    // If the skill level is enabled, swap the best PV line with the sub-optimal one
    if (skill.enabled())
        std::swap(rootMoves[0],
                  *std::find(rootMoves.begin(), rootMoves.end(),
                             skill.best ? skill.best : skill.pick_best(rootMoves, multiPV)));

    return uciPvSent;
}


void Search::Worker::do_move(Position& pos, const Move move, StateInfo& st, Stack* const ss) {
    // prefetch_key() does not model castling, en passant or promotion exactly.
    // For these rare moves the prefetch lands on an unused line.
    prefetch(tt.get_cluster(pos.prefetch_key(move)));

    ++nodes;

    ss->currentMove = move;
    ss->pieceTo     = {std::int8_t(piece_index(pos.moved_piece(move))), move.to_sq()};

    accumulators.push(pos, move);
    pos.do_move(move, st);
}

void Search::Worker::undo_move(Position& pos, const Move move) {
    pos.undo_move(move);
    accumulators.pop();
}


// Reset histories, usually before a new game
void Search::Worker::clear() { histories.clear(); }


// Main search function for both PV and non-PV nodes. The steps are numbered as
// in Stockfish; the missing steps are search features that this minimal search
// leaves out for now.
template<NodeType nodeType>
Value Search::Worker::search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {

    constexpr bool PvNode   = nodeType != NonPV;
    constexpr bool rootNode = nodeType == Root;

    // Dive into quiescence search when the depth reaches zero
    if (depth <= 0)
        return qsearch<PvNode ? PV : NonPV>(pos, ss, alpha, beta);

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);

    PVMoves   pv;
    StateInfo st;

    Key   posKey;
    Move  move, bestMove;
    Value bestValue, value, eval;
    bool  improving;
    bool  capture;


    // Step 1. Initialize node
    bestValue = -VALUE_INFINITE;
    ss->reply = Move::none();

    // Check for the available remaining time
    if (is_mainthread())
        main_manager()->check_time(*this);

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && selDepth < ss->ply + 1)
        selDepth = ss->ply + 1;

    if (!rootNode)
    {
        // Step 2. Check for a captured king, aborted search or immediate draw
        if (!pos.pieces(pos.side_to_move(), KING))
            return mated_in(ss->ply);

        if (threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply)
            || ss->ply >= MAX_PLY)
            return ss->ply >= MAX_PLY ? evaluate(pos) : value_draw(nodes);

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply + 1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Equal and opposite logic applies
        // when being mated. In either case, return a fail-high score.
        alpha = std::max(mated_in(ss->ply), alpha);
        beta  = std::min(mate_in(ss->ply + 1), beta);
        if (alpha >= beta)
            return alpha;
    }

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    bestMove = Move::none();

    // Step 4. Transposition table lookup
    posKey                         = pos.key();
    auto [ttHit, ttData, ttWriter] = tt.probe(posKey);

    ttData.move  = rootNode ? rootMoves[pvIdx].pv[0] : ttHit ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50_count()) : VALUE_NONE;

    Move excludedMove = ss->excludedMove;
    Move ttEntryMove  = ttData.move;  // Before duckless hashing may give another

    // Step 5. Static evaluation of the position, corrected by the correction
    // histories. The TT keeps the evaluation before the correction.
    Value rawEval;
    int   correction = correction_value(histories, pos, ss);

    if (ttHit)
    {
        // Never assume anything about values stored in TT
        rawEval = ttData.eval;
        if (!is_valid(rawEval))
            rawEval = evaluate(pos);

        ss->staticEval = eval = corrected(rawEval, correction);

        // ttValue can be used as a better position evaluation
        if (is_valid(ttData.value)
            && (ttData.bound & (ttData.value > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttData.value;
    }
    else
    {
        rawEval        = evaluate(pos);
        ss->staticEval = eval = corrected(rawEval, correction);
        ttWriter.write(posKey, VALUE_NONE, PvNode, BOUND_NONE, DEPTH_UNSEARCHED, Move::none(),
                       rawEval, tt.generation());
    }

    // Set up the improving flag, which is true if the current static evaluation
    // is bigger than the previous static evaluation at our turn (two plies ago,
    // or four if that one is missing), and true without either, as in wakwak.
    improving = is_valid((ss - 2)->staticEval) ? ss->staticEval > (ss - 2)->staticEval
              : is_valid((ss - 4)->staticEval) ? ss->staticEval > (ss - 4)->staticEval
                                               : true;

    ss->reduction = 0;

    // Step 6. At non-PV nodes we check for an early TT cutoff. Note that we
    //         always check the validity of the TT value because of access races.
    //         As a partial workaround for the graph history interaction problem,
    //         high rule50 counts don't produce transposition table cutoffs.
    if (!PvNode && !excludedMove && is_valid(ttData.value) && ttData.depth >= depth
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER))
        && pos.rule50_count() < 96)
    {
        ss->reply = ttData.move;
        return ttData.value;
    }

    // Duckless hashing, as in wakwak: the position with the duck elsewhere
    // often has the same best move. A second TT entry, keyed without the duck,
    // holds the best move of a fail high or PV node and its lower bound. Use
    // the move if it is legal here, and the bound for a cutoff at non-PV nodes.
    if (!rootNode && !excludedMove && (!PvNode || !ttData.move) && pos.duck_square() != SQ_NONE)
    {
        auto [dHit, dData, dWriter] = tt.probe(pos.duckless_key());
        Value dValue = dHit ? value_from_tt(dData.value, ss->ply, pos.rule50_count()) : VALUE_NONE;

        if (dHit && dData.bound == BOUND_LOWER && is_valid(dValue) && !is_decisive(dValue))
        {
            bool cutoff = !PvNode && dData.depth >= depth && dValue >= beta;

            if ((cutoff || !ttData.move) && dData.move && pos.legal(dData.move))
            {
                if (cutoff)
                {
                    ss->reply = dData.move;
                    return dValue;
                }

                ttData.move = dData.move;
            }
        }
    }

    if (!PvNode && !excludedMove)
    {
        // Hindsight extension, as in wakwak: if the parent reduced this node a
        // lot but the static evaluation got better for us since then, extend.
        if ((ss - 1)->reduction >= 3066 && is_valid((ss - 1)->staticEval)
            && ss->staticEval + (ss - 1)->staticEval < 0)
            depth = std::min(depth + 1, MAX_PLY - 1);

        // Step 9. Futility pruning: child node (reverse futility pruning). If
        // the evaluation is above beta by a margin that grows with depth,
        // assume that the search would fail high too. wakwak's margins, and the
        // result is halfway between the evaluation and beta.
        if (depth <= 8 && eval - futility_margin(depth, improving) - 243 * correction / 1024 >= beta
            && !is_win(eval) && !is_loss(beta))
            return (eval * 524 + beta * 500) / 1024;

        // Step 7. Razoring. If the static evaluation is far below alpha, check
        // with a quiescence search whether the node fails low.
        if (ss->staticEval + 317 + 253 * depth <= alpha)
        {
            value = qsearch<NonPV>(pos, ss, alpha, alpha + 1);
            if (value <= alpha)
                return value;
        }

        // Step 8. Null move reduction, as in wakwak. Passing is rarely the best
        // move, but duck chess has many zugzwangs, so a null move that fails
        // high only leads to a search at half the depth, which must fail high
        // too. Only inside such a search does a null move prune at once. The
        // duck leaves the board for the null move, so that the opponent can put
        // it anywhere.
        if (depth >= 4 && nmrPly != ss->ply && (ss - 1)->reply && eval >= beta + 19
            && (pos.pieces(WHITE) | pos.pieces(BLACK)) != pos.pieces(KING, PAWN))
        {
            Depth R = (5098 + 355 * depth) / 1024;

            ss->currentMove = Move::null();
            ss->pieceTo     = PieceTo{};

            accumulators.push_null();
            pos.do_null_move(st);

            Value nullValue = -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R);

            pos.undo_null_move();
            accumulators.pop();

            if (threads.stop.load(std::memory_order_relaxed))
                return VALUE_ZERO;

            if (nullValue >= beta && !is_win(nullValue))
            {
                if (nmrPly != -1)
                    return nullValue;

                nmrPly  = ss->ply;
                Value v = search<NonPV>(pos, ss, beta - 1, beta, depth / 2);
                nmrPly  = -1;

                if (v >= beta)
                    return v;
            }
        }
    }

    // Step 10. Internal iterative deepening, as in wakwak: a PV node without a
    // TT move first searches at a reduced depth to find one.
    if (!rootNode && PvNode && depth >= 5 && !excludedMove && !ttData.move)
    {
        search<PV>(pos, ss, alpha, beta, (733 * depth - 1539) / 1024);

        if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        [[maybe_unused]] auto [iidHit, iidData, iidWriter] = tt.probe(posKey);
        if (iidHit)
            ttData.move = iidData.move;
    }

    // Whether the king of the side to move is attacked, for the reductions
    bool kingAttacked = king_attacked(pos);

    Color             us      = pos.side_to_move();
    Bitboard          threats = pos.threats(~us);
    ContinuationMoves contMoves{(ss - 1)->pieceTo, (ss - 2)->pieceTo, (ss - 4)->pieceTo};
    bool pruneNeutrals = !PvNode && depth <= 8 && !is_decisive(alpha) && !is_decisive(beta);

    MovePicker mp(pos, ttData.move, moveStack, histories, contMoves, threats, pos.neutral_ducks(),
                  pruneNeutrals, pruneNeutrals, -28, false);

    value = bestValue;

    int moveCount = 0, searchedMoves = 0, uniqueMoves = 0, uniqueDucks = 0;

    // How many times each chess move and each duck square was searched
    u8 moveCounts[SQUARE_NB][SQUARE_NB] = {};
    u8 duckCounts[SQUARE_NB]            = {};

    // For duck refutations: by destination square, the last chess move to it
    // and the duck squares that leave a good reply to it unblocked
    struct Refutation {
        Move     chessMove;
        Bitboard ducks;
    } refutations[SQUARE_NB] = {};

    // For Duck or Die: by destination square, the last chess move to it and
    // the duck squares that keep our king safe after it
    struct DuckSafety {
        Move     chessMove;
        Bitboard safe;
    } duckSafety[SQUARE_NB] = {};

    SearchedList quietsSearched, capturesSearched;

    // Step 13. Loop through all legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move()) != Move::none())
    {
        assert(move.is_ok());

        // At root obey the "searchmoves" option and skip moves not listed in Root
        // Move List. In MultiPV mode we also skip PV moves that have been already
        // searched.
        if (rootNode && !std::count(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast, move))
            continue;

        if (move == excludedMove)
            continue;

        ++moveCount;

        if (rootNode && is_mainthread() && nodes > NODES_LIMIT_OUTPUT)
            main_manager()->updates.onIter(
              {depth, UCIEngine::move(move, pos.is_chess960()), moveCount + pvIdx});
        if (PvNode)
            (ss + 1)->pv = nullptr;

        Square from = move.from_sq(), to = move.to_sq(), duck = move.duck_sq();
        int    pc = piece_index(pos.moved_piece(move));

        capture = pos.capture_stage(move);

        int   reduction = base_reduction(depth, uniqueMoves);
        Depth lmrDepth  = std::max(0, depth - reduction / 1024);
        int   duckHist  = histories.duck[us][duck];

        // The histories of the move for the reductions and the pruning, with
        // wakwak's weights
        int lmrHistory = 0, futilityHistory = 0, pruningHistory = 0, seeHistory = 0;

        if (!capture)
        {
            int q  = histories.quiet[us][from][to][bool(threats & from)][bool(threats & to)];
            int c1 = contMoves.ply1.ok()
                     ? histories.cont[0][us][contMoves.ply1.piece][contMoves.ply1.to][pc][to]
                     : 0;
            int c2 = contMoves.ply2.ok()
                     ? histories.cont[1][us][contMoves.ply2.piece][contMoves.ply2.to][pc][to]
                     : 0;

            lmrHistory      = (q * 1057 + duckHist * 917 + c1 * 1001 + c2 * 1010) / 1024;
            futilityHistory = (q * 1087 + duckHist * 1125 + c1 * 980 + c2 * 1110) / 1024;
            pruningHistory  = (q * 1025 + duckHist * 949 + c1 * 980 + c2 * 1080) / 1024;
        }
        else
        {
            int victim = move.type_of() == EN_PASSANT ? 0 : int(type_of(pos.piece_on(to)));
            seeHistory = (histories.noisy[us][pc][to][victim] * 976 + duckHist * 975) / 1024;
        }

        // Step 14. Pruning at shallow depth, as in wakwak. Only once we have a
        // move that doesn't lose, and not at the root.
        if (!rootNode && !is_loss(bestValue))
        {
            // Duck refutations. If the opponent had a good reply to this chess
            // move with an earlier duck square, that reply is still there unless
            // this duck square blocks it, so skip the move if it doesn't.
            if (refutations[to].chessMove == move.chess_move() && (refutations[to].ducks & duck))
                continue;

            if (!capture)
            {
                // Futility pruning for quiet moves
                if (lmrDepth <= 8
                    && ss->staticEval + 99 + 93 * lmrDepth
                           + std::clamp((futilityHistory + 3956) / 61, -419, 359)
                         <= alpha)
                {
                    mp.skip_quiets();
                    continue;
                }

                // History pruning
                if (pruningHistory < -59 - 2508 * depth * depth)
                {
                    mp.skip_quiets();
                    continue;
                }

                // Duck count pruning: after a few moves with a duck square, the
                // others with it are unlikely to be better
                int dcp = (improving ? 2031 + 1040 * depth : 4132 + 2076 * depth)
                        + std::clamp((duckHist - 3844) * 1024 / 3973, -2211, 1923);
                if (!PvNode && duckCounts[duck] >= dcp / 1024)
                    continue;
            }

            // Move count pruning: the same for the duck squares of a chess move
            int mcp = capture   ? (improving ? 4111 + 4054 * lmrDepth : 4199 + 3938 * lmrDepth)
                    : improving ? 2110 + 2050 * lmrDepth
                                : 1040 + 1125 * lmrDepth;
            if (!capture)
                mcp += std::clamp((duckHist - 4003) * 1024 / 4114, -2058, 2043);
            if (moveCounts[from][to] >= mcp / 1024)
                continue;

            if (!capture)
            {
                // Unique duck pruning: after enough different duck squares, a
                // new one is unlikely to be better
                int udp =
                  10571 + 3980 * depth + std::clamp((duckHist - 4228) * 1024 / 4129, -2062, 2157);
                if (!duckCounts[duck] && uniqueDucks > udp / 1024)
                    continue;

                // Unique move pruning: the same for new chess moves
                if (!moveCounts[from][to] && uniqueMoves > (3994 + 1412 * depth * depth) / 1024)
                    continue;
            }

            // SEE pruning of the quiet moves and the captures losing material
            if (depth <= 10 && mp.stage_of() >= QUIET
                && !pos.see_ge_duck(
                  move, capture ? -7 - 73 * depth - std::clamp((seeHistory + 42) / 65, -521, 551)
                                : 10 - 73 * depth))
                continue;
        }

        // Step 15. Singular extension search, as in wakwak. If all moves but
        // the TT move fail low against a margin below the TT value, in a search
        // at about half depth, the TT move is extended, by two plies at non-PV
        // nodes when they fail low by a lot. If the TT value is already above
        // beta without it, the TT move is reduced instead.
        int extension = 0;

        if (!rootNode && depth >= 6 && !excludedMove && move == ttEntryMove && ttHit
            && ttData.depth + 3 >= depth && (ttData.bound & BOUND_LOWER) && is_valid(ttData.value)
            && !is_decisive(ttData.value))
        {
            Value singularBeta  = ttData.value - depth * 126 / 64;
            Depth singularDepth = depth * 528 / 1024;

            ss->excludedMove = move;
            value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth);
            ss->excludedMove = Move::none();

            if (threads.stop.load(std::memory_order_relaxed))
                return VALUE_ZERO;

            if (value < singularBeta)
                extension = !PvNode && value + 61 < singularBeta ? 2 : 1;
            else if (ttData.value >= beta)
                extension = -1;
        }

        uniqueMoves += !moveCounts[from][to]++;
        uniqueDucks += !duckCounts[duck]++;

        u64 nodeCount = rootNode ? u64(nodes) : 0;

        // Step 17. Make the move
        do_move(pos, move, st, ss);

        // Duck or Die. There is no check in duck chess: if our move leaves our
        // king attacked, the opponent captures it and wins. Score such a move
        // as that loss (mated in 2 plies) without searching it, unless the
        // position after it is already a draw by repetition or the 50-move rule.
        if (duckSafety[to].chessMove != move.chess_move())
            duckSafety[to] = {move.chess_move(), safe_ducks(pos)};

        if (!(duckSafety[to].safe & duck) && !pos.is_draw(ss->ply + 1))
        {
            if (PvNode)
            {
                (ss + 1)->pv = &pv;
                (ss + 1)->pv->clear();
            }

            (ss + 1)->reply = Move::none();
            value           = mated_in(ss->ply + 2);
        }
        else
        {
            Depth newDepth = std::min(depth - 1 + extension, MAX_PLY - 2);

            if (!PvNode || moveCount > 1)
            {
                // Step 18. Late moves reduction, as in wakwak: a quiet move
                // after the first six is searched at a reduced depth first,
                // then at full depth if it raises alpha.
                int r = 0;
                if (depth >= 3 && searchedMoves > 6 && !capture)
                    r = reduction + 1030 * !PvNode + 1053 * !improving + 1034 * bool(bestMove)
                      + 1024 * (duckCounts[duck] > 1 + histories.duckCount[us][duck] / 32)
                      - 1057 * kingAttacked - 67 * lmrHistory / 1024
                      - 3018 * std::abs(correction) / 1024;

                Depth d = std::min(std::max(newDepth - r / 1024, 1), newDepth);

                ss->reduction = r;
                value         = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, d);
                ss->reduction = 0;

                // Step 19. Full-depth search when the reduced search raises alpha
                if (value > alpha && d < newDepth)
                    value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth);
            }

            // Step 20. For PV nodes only, do a full PV search on the first move
            // or after a fail high, otherwise let the parent node fail low with
            // value <= alpha and try another move.
            if (PvNode && (moveCount == 1 || value > alpha))
            {
                (ss + 1)->pv = &pv;
                (ss + 1)->pv->clear();

                value = -search<PV>(pos, ss + 1, -beta, -alpha, newDepth);
            }
        }

        // Step 21. Undo move
        undo_move(pos, move);

        searchedMoves++;

        assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

        // Step 22. Check for a new best move
        // If a stop occurred, the value of the search cannot be trusted,
        // and we return immediately without updating the best move,
        // principal variation or transposition table.
        if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        // Duck refutations: if the opponent found a good reply to this move,
        // remember the duck squares that don't block it
        if (Move reply = (ss + 1)->reply)
        {
            Refutation& ref     = refutations[to];
            Bitboard    refuted = ~blocking_ducks(reply, ~us);

            if (ref.chessMove == move.chess_move())
                ref.ducks |= refuted;
            else
                ref = {move.chess_move(), refuted};
        }

        if (rootNode)
        {
            RootMove& rm = *std::find(rootMoves.begin(), rootMoves.end(), move);

            rm.effort += nodes - nodeCount;

            u64 N      = nodes - nodeCount;
            u64 E_prev = std::max(u64(1), rm.effort - N);

            // Exponential moving average parameters for root move
            constexpr u64 Scale          = 32;
            constexpr u64 ChiNumerator   = 3;
            constexpr u64 ChiDenominator = 2;   // Chi = 3/2 = 1.5
            constexpr u64 MinWeight      = 12;  // 37.5% minimum weight
            constexpr u64 MaxWeight      = 24;  // 75% maximum weight

            u64 w = std::clamp((Scale * N * ChiDenominator)
                                 / (N * ChiDenominator + ChiNumerator * E_prev),
                               MinWeight, MaxWeight);

            if (rm.averageScore == -VALUE_INFINITE)
                rm.averageScore = value;
            else
                rm.averageScore = Value((value * w + rm.averageScore * (Scale - w)) / Scale);

            // PV move or new best move?
            if (moveCount == 1 || value > alpha)
            {
                rm.score = rm.uciScore = value;
                rm.selDepth            = selDepth;
                rm.unset_inexact();

                if (value >= beta)
                {
                    rm.inexactLower = true;
                    rm.uciScore     = beta;
                }
                else if (value <= alpha)
                {
                    rm.inexactUpper = true;
                    rm.uciScore     = alpha;
                }

                rm.pv.resize(1);

                assert((ss + 1)->pv);

                for (Move pvMove : *(ss + 1)->pv)
                    rm.pv.push_back(pvMove);

                // We record how often the best move has been changed in each iteration.
                // This information is used for time management. In MultiPV mode,
                // we must take care to only do this for the first PV line.
                if (moveCount > 1 && !pvIdx)
                    ++bestMoveChanges;
            }
            else
                // All other moves but the PV are set to the lowest value: this
                // is not a problem when sorting because the sort is stable and the
                // move position in the list is preserved -- just the PV is pushed up.
                rm.score = -VALUE_INFINITE;
        }

        if (value > bestValue)
        {
            bestValue = value;

            if (value > alpha)
            {
                bestMove = ss->reply = move;

                // Update PV even in fail-high case
                if (PvNode && !rootNode)
                    ss->pv->update(move, (ss + 1)->pv);

                if (value >= beta)
                {
                    // Update the histories on a fail high, as wakwak does
                    update_all_stats(pos, *this, ss, bestMove, quietsSearched, capturesSearched,
                                     duckCounts, threats, depth);
                    break;
                }

                alpha = value;  // Update alpha! Always alpha < beta
            }
        }

        // Remember the moves that are worse than the best one, to update their
        // stats later
        if (move != bestMove)
        {
            SearchedList& list = capture ? capturesSearched : quietsSearched;
            if (list.size() < SEARCHEDLIST_CAPACITY)
                list.push_back(move);
        }
    }

    // Step 23. Check for a position without moves: without a chess move, the
    // side to move has won.
    if (!moveCount)
        bestValue = excludedMove ? alpha : mate_in(ss->ply);

    // A singular extension search leaves the TT and the histories alone
    if (excludedMove)
        return bestValue;

    // Step 24. Write gathered information in transposition table, and for
    // duckless hashing the best move of a fail high or PV node
    if (pos.duck_square() != SQ_NONE && bestMove && (bestValue >= beta || PvNode)
        && !is_decisive(bestValue))
    {
        [[maybe_unused]] auto [dHit, dData, dWriter] = tt.probe(pos.duckless_key());
        dWriter.write(pos.duckless_key(), value_to_tt(bestValue, ss->ply), PvNode, BOUND_LOWER,
                      depth, bestMove, VALUE_NONE, tt.generation());
    }

    if (!(rootNode && pvIdx))
        ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), PvNode,
                       bestValue >= beta    ? BOUND_LOWER
                       : PvNode && bestMove ? BOUND_EXACT
                                            : BOUND_UPPER,
                       moveCount != 0 ? depth : std::min(MAX_PLY - 1, depth + 6), bestMove, rawEval,
                       tt.generation());

    // Update the correction histories when the search result is on the side
    // of the static evaluation its bound allows, and the best move is quiet
    if ((!bestMove || !pos.capture_stage(bestMove))
        && !(bestValue >= beta && bestValue <= ss->staticEval)
        && !(!bestMove && bestValue >= ss->staticEval))
        update_correction_history(histories, pos, ss, depth, bestValue - ss->staticEval);

    // The search is now complete
    assert(-VALUE_INFINITE < bestValue && bestValue < VALUE_INFINITE);
    return bestValue;
}


// Quiescence search function, which is called by the main search function with
// depth zero, or recursively with further decreasing depth. With depth <= 0, we
// "should" be using static eval only, but tactical moves may confuse the static eval.
// To fight this horizon effect, we implement this qsearch of tactical moves.
// See https://www.chessprogramming.org/Horizon_Effect
// and https://www.chessprogramming.org/Quiescence_Search
template<NodeType nodeType>
Value Search::Worker::qsearch(Position& pos, Stack* ss, Value alpha, Value beta) {

    static_assert(nodeType != Root);
    constexpr bool PvNode = nodeType == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));

    PVMoves   pv;
    StateInfo st;

    Key   posKey;
    Move  move, bestMove;
    Value bestValue, value;

    // Step 1. Initialize node
    if (PvNode)
    {
        (ss + 1)->pv = &pv;
        ss->pv->clear();
    }

    bestMove = ss->reply = Move::none();

    // Check for the available remaining time. Unlike in chess, one qsearch can
    // take longer than a fast time control: each capture comes with every duck
    // square.
    if (is_mainthread())
        main_manager()->check_time(*this);

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && selDepth < ss->ply + 1)
        selDepth = ss->ply + 1;

    // Step 2. Check for a captured king, aborted search, immediate draw or
    // maximum ply reached
    if (!pos.pieces(pos.side_to_move(), KING))
        return mated_in(ss->ply);

    if (threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
        return ss->ply >= MAX_PLY ? evaluate(pos) : VALUE_DRAW;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Step 3. Transposition table lookup. Only a capture or promotion is used
    // as the TT move.
    posKey                         = pos.key();
    auto [ttHit, ttData, ttWriter] = tt.probe(posKey);

    ttData.move =
      ttHit && ttData.move && pos.capture_stage(ttData.move) ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50_count()) : VALUE_NONE;

    // A TT cutoff, at every node, as in wakwak
    if (is_valid(ttData.value)
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER)))
    {
        ss->reply = ttData.move;
        return ttData.value;
    }

    // Step 4. Static evaluation of the position
    Value rawEval    = ttHit && is_valid(ttData.eval) ? ttData.eval : evaluate(pos);
    Value staticEval = corrected(rawEval, correction_value(histories, pos, ss));

    // Stand pat. Return immediately if static value is at least beta
    bestValue = staticEval;

    if (bestValue >= beta)
    {
        if (!ttHit)
            ttWriter.write(posKey, VALUE_NONE, false, BOUND_LOWER, DEPTH_UNSEARCHED, Move::none(),
                           rawEval, tt.generation());
        return bestValue;
    }

    if (bestValue > alpha)
        alpha = bestValue;

    // The captures and promotions with a good SEE, each with every duck
    // square, but the neutral ones at non-PV nodes
    Color             us = pos.side_to_move();
    ContinuationMoves contMoves{(ss - 1)->pieceTo, (ss - 2)->pieceTo, (ss - 4)->pieceTo};
    bool              pruneNeutrals = !PvNode && !is_decisive(alpha) && !is_decisive(beta);

    MovePicker mp(pos, ttData.move, moveStack, histories, contMoves, 0, pos.neutral_ducks(), false,
                  pruneNeutrals, 9, true);

    // How many times each chess move and each duck square was searched: at
    // most twice each, as in wakwak
    struct MoveCount {
        Move chessMove;
        int  count;
    } moveCounts[32];
    int moveCountSize         = 0;
    u8  duckCounts[SQUARE_NB] = {};

    Bitboard refutedDucks[SQUARE_NB] = {};

    struct DuckSafety {
        Move     chessMove;
        Bitboard safe;
    } duckSafety[SQUARE_NB] = {};

    // Step 5. Loop through all moves until no moves remain or a beta cutoff
    // occurs.
    while ((move = mp.next_move()) != Move::none())
    {
        assert(move.is_ok());

        Square to = move.to_sq(), duck = move.duck_sq();

        // Duck refutations: skip a capture whose duck square doesn't block a
        // good reply found to an earlier capture on the same square
        if (refutedDucks[to] & duck)
            continue;

        if (duckCounts[duck] >= 2)
            continue;

        MoveCount* mc =
          std::find_if(moveCounts, moveCounts + moveCountSize,
                       [&](const MoveCount& c) { return c.chessMove == move.chess_move(); });

        if (mc == moveCounts + moveCountSize)
        {
            if (moveCountSize == 32)
                continue;
            *mc = {move.chess_move(), 0};
            moveCountSize++;
        }

        if (mc->count >= 2)
            continue;

        mc->count++;
        duckCounts[duck]++;

        // Step 7. Make and search the move. As in the main search (Duck or
        // Die), a capture that leaves our king attacked is scored as a loss
        // without searching it.
        do_move(pos, move, st, ss);

        if (duckSafety[to].chessMove != move.chess_move())
            duckSafety[to] = {move.chess_move(), safe_ducks(pos)};

        if (!(duckSafety[to].safe & duck) && !pos.is_draw(ss->ply + 1))
        {
            if (PvNode)
                (ss + 1)->pv->clear();

            (ss + 1)->reply = Move::none();
            value           = mated_in(ss->ply + 2);
        }
        else
            value = -qsearch<nodeType>(pos, ss + 1, -beta, -alpha);

        undo_move(pos, move);

        assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

        // If the search was stopped, the value cannot be trusted: return at
        // once, without updating the best move, the PV or the TT
        if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        // Duck refutations
        if (Move reply = (ss + 1)->reply)
            refutedDucks[to] |= ~blocking_ducks(reply, ~us);

        // Step 8. Check for a new best move
        if (value > bestValue)
        {
            bestValue = value;

            if (value > alpha)
            {
                bestMove = ss->reply = move;

                // Update pv even in fail-high case
                if (PvNode)
                    ss->pv->update(move, (ss + 1)->pv);

                if (value < beta)  // Update alpha here!
                    alpha = value;
                else
                    break;  // Fail high
            }
        }
    }

    // Step 9. Check for a position without moves, which the side to move has
    // won. Only check it under specific conditions.
    if (bestValue == staticEval && !bestMove)
    {
        if (!(pawn_single_push_bb(us, pos.pieces(us, PAWN)) & ~pos.pieces())
            && !pos.non_pawn_material(us) && !MoveList<ALL>(pos).size())
            bestValue = mate_in(ss->ply);
    }

    // Step 10. Save gathered info in transposition table
    ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), PvNode,
                   bestValue >= beta ? BOUND_LOWER : BOUND_UPPER, DEPTH_QS, bestMove, rawEval,
                   tt.generation());

    // The search is now complete
    assert(-VALUE_INFINITE < bestValue && bestValue < VALUE_INFINITE);
    return bestValue;
}

// elapsed() returns the time elapsed since the search started. If the
// 'nodestime' option is enabled, it will return the count of nodes searched
// instead. This function is called to check whether the search should be
// stopped based on predefined thresholds like time limits or nodes searched.
TimePoint Search::Worker::elapsed() const {
    return main_manager()->tm.elapsed([this]() { return threads.nodes_searched(); });
}


// Evaluate the current position of the game tree, from the point of view of
// the side to move.
Value Search::Worker::evaluate(const Position& pos) { return accumulators.evaluate(pos); }

namespace {

// Adjusts a mate or TB score from "plies to mate from the root" to
// "plies to mate from the current position". Standard scores are unchanged.
// The function is called before storing a value in the transposition table.
Value value_to_tt(Value v, int ply) { return is_win(v) ? v + ply : is_loss(v) ? v - ply : v; }


// Inverse of value_to_tt(): it adjusts a mate or TB score from the transposition
// table (which refers to the plies to mate/be mated from current position) to
// "plies to mate/be mated (TB win/loss) from the root". However, to avoid
// potentially false mate or TB scores related to the 50 moves rule and the
// graph history interaction, we return the highest non-TB score instead.
Value value_from_tt(Value v, int ply, int r50c) {

    if (!is_valid(v))
        return VALUE_NONE;

    // Handle TB win or better
    if (is_win(v))
    {
        // Downgrade a potentially false mate score
        if (is_mate(v) && VALUE_MATE - v > 100 - r50c)
            return VALUE_TB_WIN_IN_MAX_PLY - 1;

        // Downgrade a potentially false TB score
        if (VALUE_TB - v > 100 - r50c)
            return VALUE_TB_WIN_IN_MAX_PLY - 1;

        return v - ply;
    }

    // Handle TB loss or worse
    if (is_loss(v))
    {
        // Downgrade a potentially false mate score
        if (is_mated(v) && VALUE_MATE + v > 100 - r50c)
            return VALUE_TB_LOSS_IN_MAX_PLY + 1;

        // Downgrade a potentially false TB score
        if (VALUE_TB + v > 100 - r50c)
            return VALUE_TB_LOSS_IN_MAX_PLY + 1;

        return v + ply;
    }

    return v;
}


// Updates the histories after a fail high, as in wakwak. A quiet best move
// gets a bonus in the quiet, pawn, quiet-duck and continuation histories, and
// the other quiet chess moves searched a malus; a capture gets a bonus in the
// capture history. The other captures searched always get a malus. The duck
// square of the best move gets a bonus in the duck history, those of the other
// moves a malus, and the duck count history learns how many moves with that
// duck square it took to find the best move.
void update_all_stats(const Position& pos,
                      Search::Worker& workerThread,
                      Stack*          ss,
                      Move            bestMove,
                      SearchedList&   quietsSearched,
                      SearchedList&   capturesSearched,
                      const u8*       duckCounts,
                      Bitboard        threats,
                      Depth           depth) {

    Histories& h  = workerThread.histories;
    Color      us = pos.side_to_move();

    auto bonus = [depth](int base, int scale, int max) {
        return std::min(base + scale * depth, max);
    };

    const PieceTo prev[3] = {(ss - 1)->pieceTo, (ss - 2)->pieceTo, (ss - 4)->pieceTo};

    auto update_quiet = [&](Move m, bool good) {
        Square from = m.from_sq(), to = m.to_sq();
        int    pc = piece_index(pos.moved_piece(m));

        gravity<MaxHistory, MaxHistory>(
          h.quiet[us][from][to][bool(threats & from)][bool(threats & to)],
          good ? bonus(128, 126, 2152) : -bonus(132, 130, 2076));
        gravity<MaxHistory, MaxHistory>(h.pawn[us][pos.pawn_key() % PawnHistorySize][pc][to],
                                        good ? bonus(125, 128, 2095) : -bonus(127, 135, 2063));
        gravity<MaxHistory, MaxHistory>(h.quietDuck[us][pc][to][m.duck_sq()],
                                        good ? bonus(123, 127, 2053) : -bonus(128, 125, 2082));

        // The continuation histories decay by the sum of their entries
        std::int16_t* entries[3] = {};
        int           total      = 0;
        for (int i = 0; i < 3; ++i)
            if (prev[i].ok())
            {
                entries[i] = &h.cont[i != 0][us][prev[i].piece][prev[i].to][pc][to];
                total += *entries[i];
            }

        const int amount[3] = {good ? bonus(131, 129, 2013) : -bonus(139, 126, 2024),
                               good ? bonus(127, 120, 2031) : -bonus(127, 123, 2123),
                               good ? bonus(128, 127, 2026) : -bonus(124, 131, 2003)};

        for (int i = 0; i < 3; ++i)
            if (entries[i])
                gravity<MaxHistory, MaxHistory>(*entries[i], total, amount[i]);
    };

    auto update_noisy = [&](Move m, bool good) {
        Square to     = m.to_sq();
        int    victim = m.type_of() == EN_PASSANT ? 0 : int(type_of(pos.piece_on(to)));
        gravity<MaxHistory, MaxHistory>(h.noisy[us][piece_index(pos.moved_piece(m))][to][victim],
                                        good ? bonus(127, 131, 2136) : -bonus(126, 120, 2069));
    };

    // Each chess move and duck square is updated once
    Bitboard seen[SQUARE_NB] = {};
    auto     first_time      = [&](Move m) {
        bool first = !(seen[m.from_sq()] & m.to_sq());
        seen[m.from_sq()] |= m.to_sq();
        return first;
    };

    if (pos.capture_stage(bestMove))
    {
        first_time(bestMove);
        update_noisy(bestMove, true);
    }
    else
    {
        first_time(bestMove);
        update_quiet(bestMove, true);

        for (Move m : quietsSearched)
            if (first_time(m))
                update_quiet(m, false);
    }

    // The duck count of the best move's duck square, times 32
    std::int16_t& dc = h.duckCount[us][bestMove.duck_sq()];
    gravity<MaxHistory / 4, MaxHistory>(dc,
                                        (duckCounts[bestMove.duck_sq()] * 32 - dc) * depth / 10);

    for (Move m : capturesSearched)
        if (first_time(m))
            update_noisy(m, false);

    Bitboard ducks = square_bb(bestMove.duck_sq());
    gravity<MaxHistory, MaxHistory>(h.duck[us][bestMove.duck_sq()], bonus(129, 128, 2012));

    for (const SearchedList* list : {&quietsSearched, &capturesSearched})
        for (Move m : *list)
            if (!(ducks & m.duck_sq()))
            {
                ducks |= m.duck_sq();
                gravity<MaxHistory, MaxHistory>(h.duck[us][m.duck_sq()], -bonus(127, 125, 2143));
            }
}
}  // namespace

// When playing with strength handicap, choose the best move among a set of
// RootMoves using a statistical rule dependent on 'level'.
// Idea by Heinz van Saanen.
Move Skill::pick_best(const RootMoves& rootMoves, usize multiPV) {
    static PRNG rng(now());  // PRNG sequence should be non-deterministic

    Value topScore = rootMoves[0].score;
    Value minScore = rootMoves[0].score;
    for (usize i = 1; i < multiPV; ++i)
    {
        topScore = std::max(topScore, rootMoves[i].score);
        minScore = std::min(minScore, rootMoves[i].score);
    }
    int    delta    = std::min(topScore - minScore, int(PawnValue));
    int    maxScore = -VALUE_INFINITE;
    double weakness = 120 - 2 * level;

    // Choose best move. For each move score we add two terms dependent on
    // weakness. One is deterministic and bigger for weaker levels, and one
    // is random. Then we choose the move with the resulting highest score.
    for (usize i = 0; i < multiPV; ++i)
    {
        // This is our magic formula
        int push = int(weakness * int(topScore - rootMoves[i].score)
                       + delta * (rng.rand<unsigned>() % int(weakness)))
                 / 128;

        if (rootMoves[i].score + push >= maxScore)
        {
            maxScore = rootMoves[i].score + push;
            best     = rootMoves[i].pv[0];
        }
    }

    return best;
}

// Function to detect when we are out of available time and stop the search,
// and to print debug info.
void SearchManager::check_time(Search::Worker& worker) {

    if (--callsCnt > 0)
        return;

    // When using nodes, ensure checking rate is not lower than 0.1% of nodes
    callsCnt = worker.limits.nodes ? std::min(512, int(worker.limits.nodes / 1024)) : 512;

    static TimePoint lastInfoTime = now();

    TimePoint elapsed = tm.elapsed([&worker]() { return worker.threads.nodes_searched(); });
    TimePoint tick    = worker.limits.startTime + elapsed;

    if (tick - lastInfoTime >= 1000)
    {
        lastInfoTime = tick;
        dbg_print();
    }

    // We should not stop pondering until told so by the GUI
    if (ponder)
        return;

    if ((worker.limits.use_time_management() && (elapsed > tm.maximum() || stopOnPonderhit))
        || (worker.limits.movetime && elapsed >= worker.limits.movetime)
        || (worker.limits.nodes && worker.threads.nodes_searched() >= worker.limits.nodes))
        worker.threads.stop = true;
}

void SearchManager::output_pv(Search::Worker&           worker,
                              const ThreadPool&         threads,
                              const TranspositionTable& tt,
                              Depth                     depth) {

    const auto nodes     = threads.nodes_searched();
    auto&      rootMoves = worker.rootMoves;
    auto&      pos       = worker.rootPos;
    usize      multiPV   = std::min(usize(worker.options["MultiPV"]), rootMoves.size());

    for (usize i = 0; i < multiPV; ++i)
    {
        bool usePreviousScore = rootMoves[i].score == -VALUE_INFINITE;

        if (depth == 1 && usePreviousScore && i > 0)
            continue;

        Depth d = usePreviousScore ? std::max(1, depth - 1) : depth;
        Value v = usePreviousScore ? rootMoves[i].previousScore : rootMoves[i].uciScore;

        if (v == -VALUE_INFINITE)
            v = VALUE_ZERO;

        std::string pv;
        for (Move m : usePreviousScore ? rootMoves[i].previousPV : rootMoves[i].pv)
            pv += UCIEngine::move(m, pos.is_chess960()) + " ";

        // Remove last whitespace
        if (!pv.empty())
            pv.pop_back();

        auto wdl = worker.options["UCI_ShowWDL"] ? UCIEngine::wdl(v, pos) : "";

        // Scores cannot be both exact and inexact
        assert(!(rootMoves[i].inexactLower && rootMoves[i].inexactUpper));
        auto bound = rootMoves[i].inexactLower ? "lowerbound"
                   : rootMoves[i].inexactUpper ? "upperbound"
                                               : "";

        InfoFull info;

        info.depth    = d;
        info.selDepth = rootMoves[i].selDepth;
        info.multiPV  = i + 1;
        info.score    = {v, pos};
        info.wdl      = wdl;

        // Previous scores are exact, even though their flags may say otherwise
        if (!usePreviousScore)
            info.bound = bound;

        TimePoint time = std::max(TimePoint(1), tm.elapsed_time());
        info.timeMs    = time;
        info.nodes     = nodes;
        info.nps       = nodes * 1000 / time;
        info.pv        = pv;
        info.hashfull  = tt.hashfull();

        updates.onUpdateFull(info);
    }
}

// Called in case we have no ponder move before exiting the search,
// for instance, in case we stop the search during a fail high at root.
// We try hard to have a ponder move to return to the GUI, otherwise
// in case of 'ponder on' we have nothing to think about.
bool RootMove::extract_ponder_from_tt(const TranspositionTable& tt, Position& pos) {

    assert(pv.size() == 1 && pv[0] != Move::none());

    StateInfo st;
    pos.do_move(pv[0], st);

    if (!pos.is_draw(1))
    {
        auto [ttHit, ttData, ttWriter] = tt.probe(pos.key());
        if (ttHit && ttData.move && pos.legal(ttData.move))
            pv.push_back(ttData.move);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}


}  // namespace Stockfish
