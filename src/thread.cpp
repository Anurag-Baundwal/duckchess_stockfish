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

#include "thread.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "bitboard.h"
#include "movegen.h"
#include "search.h"
#include "timeman.h"
#include "types.h"
#include "uci.h"
#include "ucioption.h"

namespace Stockfish {

// Constructor launches the thread and waits until it goes to sleep
// in idle_loop(). Note that 'searching' and 'exit' should be already set.
Thread::Thread(Search::SharedState&                   sharedState,
               std::unique_ptr<Search::SearchManager> sm,
               usize                                  n) :
    worker(std::make_unique<Search::Worker>(sharedState, std::move(sm), n)),
    idx(n),
    stdThread(
      create_native_thread(NativeThreadOptions{}.setLargeStack(true), &Thread::idle_loop, this)) {

    if (!stdThread.joinable())
    {
        std::cerr << "Failed to create search thread\n";
        std::exit(EXIT_FAILURE);
    }

    wait_for_search_finished();
}


// Destructor wakes up the thread in idle_loop() and waits
// for its termination. Thread should be already waiting.
Thread::~Thread() {

    assert(!searching);

    exit = true;
    start_searching();
    stdThread.join();
}

// Wakes up the thread that will start the search
void Thread::start_searching() {
    assert(worker != nullptr);
    run_custom_job([this]() { worker->start_searching(); });
}

// Clears the histories for the thread worker (usually before a new game)
void Thread::clear_worker() {
    assert(worker != nullptr);
    run_custom_job([this]() { worker->clear(); });
}

// Blocks on the condition variable until the thread has finished searching
void Thread::wait_for_search_finished() {

    std::unique_lock<std::mutex> lk(mutex);
    cv.wait(lk, [&] { return !searching; });
}

// Launching a function in the thread
void Thread::run_custom_job(std::function<void()> f) {
    {
        std::unique_lock<std::mutex> lk(mutex);
        cv.wait(lk, [&] { return !searching; });
        jobFunc   = std::move(f);
        searching = true;
    }
    cv.notify_one();
}

// Thread gets parked here, blocked on the condition variable
// when the thread has no work to do.

void Thread::idle_loop() {
    while (true)
    {
        std::unique_lock<std::mutex> lk(mutex);
        searching = false;
        cv.notify_one();  // Wake up anyone waiting for search finished
        cv.wait(lk, [&] { return searching; });

        if (exit)
            return;

        std::function<void()> job = std::move(jobFunc);
        jobFunc                   = nullptr;

        lk.unlock();

        if (job)
            job();
    }
}

Search::SearchManager* ThreadPool::main_manager() { return main_thread()->worker->main_manager(); }

u64 ThreadPool::nodes_searched() const { return accumulate(&Search::Worker::nodes); }

// Creates/destroys threads to match the requested number.
// Created and launched threads will immediately go to sleep in idle_loop.
// Upon resizing, threads are recreated.
void ThreadPool::set(Search::SharedState                         sharedState,
                     const Search::SearchManager::UpdateContext& updateContext) {

    if (threads.size() > 0)  // destroy any existing thread(s)
    {
        main_thread()->wait_for_search_finished();

        threads.clear();
    }

    const usize requested = sharedState.options["Threads"];

    if (requested > 0)  // create new thread(s)
    {
        while (threads.size() < requested)
        {
            const usize threadId = threads.size();
            auto        manager =
              threadId == 0 ? std::make_unique<Search::SearchManager>(updateContext) : nullptr;

            threads.emplace_back(
              std::make_unique<Thread>(sharedState, std::move(manager), threadId));
        }

        clear();

        main_thread()->wait_for_search_finished();
    }
}


// Sets threadPool data to initial values
void ThreadPool::clear() {
    if (threads.empty())
        return;

    for (auto&& th : threads)
        th->clear_worker();

    for (auto&& th : threads)
        th->wait_for_search_finished();

    // These two affect the time taken on the first move of a game:
    main_manager()->bestPreviousAverageScore = VALUE_INFINITE;
    main_manager()->previousTimeReduction    = 0.85;

    main_manager()->callsCnt           = 0;
    main_manager()->bestPreviousScore  = VALUE_INFINITE;
    main_manager()->originalTimeAdjust = -1;
    main_manager()->tm.clear();
}

void ThreadPool::run_on_thread(usize threadId, std::function<void()> f) {
    assert(threads.size() > threadId);
    threads[threadId]->run_custom_job(std::move(f));
}

void ThreadPool::wait_on_thread(usize threadId) {
    assert(threads.size() > threadId);
    threads[threadId]->wait_for_search_finished();
}

usize ThreadPool::num_threads() const { return threads.size(); }


// Wakes up main thread waiting in idle_loop() and returns immediately.
// Main thread will wake up other threads and start the search.
void ThreadPool::start_thinking(Position& pos, StateListPtr& states, Search::LimitsType limits) {

    main_thread()->wait_for_search_finished();

    main_manager()->stopOnPonderhit = stop = false;
    main_manager()->ponder                 = limits.ponderMode;

    increaseDepth = true;

    Search::RootMoves rootMoves;

    // A side whose king was captured has no moves
    if (pos.pieces(pos.side_to_move(), KING))
    {
        for (const auto& uciMove : limits.searchmoves)
        {
            auto move = UCIEngine::to_move(pos, uciMove);

            if (move != Move::none())
                rootMoves.emplace_back(move);
        }

        // Every chess move, with each square the duck can go to after it
        if (rootMoves.empty())
            for (const auto& m : MoveList<ALL>(pos))
                for (Bitboard b = pos.duck_targets(m); b;)
                    rootMoves.emplace_back(m.with_duck(pop_lsb(b)));
    }

    // After ownership transfer 'states' becomes empty, so if we stop the search
    // and call 'go' again without setting a new position states.get() == nullptr.
    assert(states.get() || setupStates.get());

    if (states.get())
        setupStates = std::move(states);  // Ownership transfer, states is now empty

    // We use Position::set() to set root position across threads. But there are
    // some StateInfo fields (previous, pliesFromNull, capturedPiece) that cannot
    // be deduced from a fen string, so set() clears them and they are set from
    // setupStates->back() later. The rootState is per thread, earlier states are
    // shared since they are read-only.
    for (auto&& th : threads)
    {
        th->run_custom_job([&]() {
            th->worker->limits = limits;
            th->worker->nodes = th->worker->bestMoveChanges = 0;
            th->worker->rootDepth                           = 0;
            th->worker->rootMoves                           = rootMoves;
            th->worker->rootPos.set(pos.fen(), pos.is_chess960(), &th->worker->rootState);
            th->worker->rootState = setupStates->back();
        });
    }

    for (auto&& th : threads)
        th->wait_for_search_finished();

    main_thread()->start_searching();
}

Thread* ThreadPool::get_best_thread() const {

    Thread* bestThread = threads.front().get();
    Value   minScore   = VALUE_INFINITE;

    std::unordered_map<Move, i64, Move::MoveHash> votes(
      2 * std::min(size(), bestThread->worker->rootMoves.size()));

    for (auto&& th : threads)
        minScore = std::min(minScore, th->worker->rootMoves[0].score);

    // Vote according to score, and select the best thread
    for (auto&& th : threads)
        votes[th->worker->rootMoves[0].pv[0]] += th->worker->rootMoves[0].score - minScore + 14;

    for (auto&& th : threads)
    {
        const auto& bestThreadMove = bestThread->worker->rootMoves[0];
        const auto& newThreadMove  = th->worker->rootMoves[0];

        const auto bestThreadMoveVote = votes[bestThreadMove.pv[0]];
        const auto newThreadMoveVote  = votes[newThreadMove.pv[0]];

        // Aborted (d1) searches may lead to inexact win (or loss) scores.
        const bool bestThreadDecisive = bestThreadMove.score != -VALUE_INFINITE
                                     && is_decisive(bestThreadMove.score)
                                     && !bestThreadMove.is_inexact();
        const bool newThreadDecisive = newThreadMove.score != -VALUE_INFINITE
                                    && is_decisive(newThreadMove.score)
                                    && !newThreadMove.is_inexact();

        if (bestThreadDecisive)
        {
            // Make sure we pick the shortest mate / TB conversion.
            if (newThreadDecisive && std::abs(newThreadMove.score) > std::abs(bestThreadMove.score))
            {
                assert((is_win(bestThreadMove.score) && is_win(newThreadMove.score))
                       || (is_loss(bestThreadMove.score) && is_loss(newThreadMove.score)));

                bestThread = th.get();
            }
        }
        else if (newThreadDecisive
                 || (!is_loss(newThreadMove.score)
                     && (newThreadMoveVote > bestThreadMoveVote
                         || (newThreadMoveVote == bestThreadMoveVote
                             && newThreadMove.pv.size() > bestThreadMove.pv.size()))))
            bestThread = th.get();
    }

    return bestThread;
}


// Start non-main threads.
// Will be invoked by main thread after it has started searching.
void ThreadPool::start_searching() {

    for (auto&& th : threads)
        if (th != threads.front())
            th->start_searching();
}


// Wait for non-main threads
void ThreadPool::wait_for_search_finished() const {

    for (auto&& th : threads)
        if (th != threads.front())
            th->wait_for_search_finished();
}

}  // namespace Stockfish
