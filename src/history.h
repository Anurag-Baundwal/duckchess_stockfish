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

}  // namespace Stockfish

#endif  // #ifndef HISTORY_H_INCLUDED
