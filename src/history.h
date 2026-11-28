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

#ifndef HISTORY_H_INCLUDED
#define HISTORY_H_INCLUDED

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "types.h"

namespace Stockfish {

// The move histories, as in wakwak: every table holds i16 values in
// [-MaxHistory, MaxHistory], updated with the "gravity" formula, so that a
// value moves toward the bonus by an amount that shrinks near the limit.
constexpr int MaxHistory            = 16384;
constexpr int MaxCorrection         = 1024;
constexpr int PawnHistorySize       = 4096;
constexpr int CorrectionHistorySize = 16384;

// Adds a bonus to an entry; the decay is usually the entry itself, but the
// continuation histories use the sum of the entries a move has.
template<int MaxBonus, int MaxValue>
inline void gravity(std::int16_t& entry, int decay, int bonus) {
    bonus = std::clamp(bonus, -MaxBonus, MaxBonus);
    entry += std::int16_t(bonus - decay * std::abs(bonus) / MaxValue);
}

template<int MaxBonus, int MaxValue>
inline void gravity(std::int16_t& entry, int bonus) {
    gravity<MaxBonus, MaxValue>(entry, entry, bonus);
}

// Index of a piece type in the history tables
inline int piece_index(Piece pc) { return type_of(pc) - 1; }

// A move as continuation histories see it: the piece and its destination
struct PieceTo {
    std::int8_t piece = -1;  // -1 if there is no such move (null move, before the root)
    Square      to    = SQ_A1;

    bool ok() const { return piece >= 0; }
};

struct Histories {

    void clear() {
        std::memset(this, 0, sizeof(*this));
        for (auto& c : duckCount)
            for (auto& e : c)
                e = 2;
    }

    // [side to move][from][to][from attacked][to attacked]
    std::int16_t quiet[COLOR_NB][SQUARE_NB][SQUARE_NB][2][2];
    // [side to move][piece][to][captured piece + 1, or 0]
    std::int16_t noisy[COLOR_NB][6][SQUARE_NB][7];
    // [side to move][pawn key][piece][to]
    std::int16_t pawn[COLOR_NB][PawnHistorySize][6][SQUARE_NB];
    // [side to move][duck square]
    std::int16_t duck[COLOR_NB][SQUARE_NB];
    // [side to move][piece][to][duck square]
    std::int16_t quietDuck[COLOR_NB][6][SQUARE_NB][SQUARE_NB];
    // [side to move][duck square]: how many moves with this duck square the
    // best moves needed, times 32
    std::int16_t duckCount[COLOR_NB][SQUARE_NB];
    // [odd/even plies back][side to move][previous piece][previous to][piece][to]
    std::int16_t cont[2][COLOR_NB][6][SQUARE_NB][6][SQUARE_NB];

    // Correction histories: [side to move][key % size]
    std::int16_t pawnCorr[COLOR_NB][CorrectionHistorySize];
    std::int16_t minorCorr[COLOR_NB][CorrectionHistorySize];
    std::int16_t majorCorr[COLOR_NB][CorrectionHistorySize];
    std::int16_t nonPawnCorr[COLOR_NB][COLOR_NB][CorrectionHistorySize];
    // [odd/even][side to move][previous piece][previous to][piece][to]
    std::int16_t contCorr[2][COLOR_NB][6][SQUARE_NB][6][SQUARE_NB];
};

}  // namespace Stockfish

#endif  // #ifndef HISTORY_H_INCLUDED
