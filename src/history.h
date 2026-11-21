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
#include <array>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <type_traits>  // IWYU pragma: keep

#include "misc.h"
#include "types.h"

namespace Stockfish {

constexpr int UINT_16_HISTORY_SIZE = std::numeric_limits<u16>::max() + 1;

// StatsEntry is the container of various numerical statistics. We use a class
// instead of a naked value to directly call history update operator<<() on
// the entry. The first template parameter T is the base type of the array,
// and the second template parameter D limits the range of updates in [-D, D]
// when we update values with the << operator
template<typename T, int D, bool Shared = false>
struct StatsEntry {
    static_assert(std::is_integral_v<T> && std::is_signed_v<T>, "Not a signed integer type");
    static_assert(D > 0 && D <= std::numeric_limits<T>::max()
                    && D <= std::numeric_limits<int>::max() / D,
                  "D can lead to overflows");

   private:
    std::conditional_t<Shared, RelaxedAtomic<T>, T> entry;

   public:
    void operator=(const T& v) { entry = v; }

    operator T() const { return entry; }

    void operator<<(int bonus) {
        // Make sure that bonus is in range [-D, D]
        int clampedBonus = std::clamp(bonus, -D, D);
        T   val          = *this;
        *this            = val + clampedBonus - val * std::abs(clampedBonus) / D;

        assert(std::abs(T(*this)) <= D);
    }
};

template<typename T, int D, usize... Sizes>
using Stats = MultiArray<StatsEntry<T, D>, Sizes...>;

// ButterflyHistory records how often quiet moves have been successful or unsuccessful
// during the current search, and is used for move ordering decisions.
// It uses 2 tables (one for each color) indexed by the move's from and to squares,
// see https://www.chessprogramming.org/Butterfly_Boards
using ButterflyHistory = Stats<i16, 7183, COLOR_NB, UINT_16_HISTORY_SIZE>;

// DuckHistory records how often each square the side to move put the duck on was
// part of a successful or unsuccessful move, and orders the duck squares of each
// chess move. It also keeps each color's 64 squares sorted by their entries, best
// first, moving a square only as far as an update requires, so that the move
// picker never sorts duck squares itself.
class DuckHistory {
    using Entry = StatsEntry<i16, 7183>;

   public:
    using Order = std::array<Square, SQUARE_NB>;

    void clear() {
        for (Color c : {WHITE, BLACK})
            for (Square s = SQ_A1; s <= SQ_H8; ++s)
            {
                entries[c][s] = 0;
                order[c][s]   = s;
                index[c][s]   = u8(s);
            }
    }

    int operator()(Color c, Square s) const { return entries[c][s]; }

    const Order& sorted(Color c) const { return order[c]; }

    void update(Color c, Square s, int bonus) {
        entries[c][s] << bonus;

        int v = entries[c][s];
        int i = index[c][s];

        for (; i > 0 && entries[c][order[c][i - 1]] < v; --i)
            swap(c, i, i - 1);

        for (; i < SQUARE_NB - 1 && entries[c][order[c][i + 1]] > v; ++i)
            swap(c, i, i + 1);
    }

    // Scale every entry down. This keeps their order, up to rounding.
    void age(int num, int den) {
        for (Color c : {WHITE, BLACK})
            for (Square s = SQ_A1; s <= SQ_H8; ++s)
                entries[c][s] = i16(entries[c][s] * num / den);
    }

   private:
    void swap(Color c, int i, int j) {
        std::swap(order[c][i], order[c][j]);
        index[c][order[c][i]] = u8(i);
        index[c][order[c][j]] = u8(j);
    }

    Entry entries[COLOR_NB][SQUARE_NB];
    Order order[COLOR_NB];
    u8    index[COLOR_NB][SQUARE_NB];
};

}  // namespace Stockfish

#endif  // #ifndef HISTORY_H_INCLUDED
