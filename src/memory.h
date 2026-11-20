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

#ifndef MEMORY_H_INCLUDED
#define MEMORY_H_INCLUDED

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "types.h"
#include "misc.h"


namespace Stockfish {

[[noreturn]] inline void report_failed_allocation(usize bytes) {
    std::cerr << "Failed to allocate " << bytes << " bytes." << std::endl;
    std::exit(EXIT_FAILURE);
}

void* std_aligned_alloc(usize alignment, usize size);
void  std_aligned_free(void* ptr);

// Frees memory which was placed there with placement new.
// Works for both single objects and arrays of unknown bound.
template<typename T, typename FREE_FUNC>
void memory_deleter(T* ptr, FREE_FUNC free_func) {
    if (!ptr)
        return;

    // Explicitly needed to call the destructor
    if constexpr (!std::is_trivially_destructible_v<T>)
        ptr->~T();

    free_func(ptr);
}

// Frees memory which was placed there with placement new.
// Works for both single objects and arrays of unknown bound.
template<typename T, typename FREE_FUNC>
void memory_deleter_array(T* ptr, FREE_FUNC free_func) {
    if (!ptr)
        return;


    // Move back on the pointer to where the size is allocated
    const usize array_offset = std::max(sizeof(usize), alignof(T));
    char*       raw_memory   = reinterpret_cast<char*>(ptr) - array_offset;

    if constexpr (!std::is_trivially_destructible_v<T>)
    {
        const usize size = *reinterpret_cast<usize*>(raw_memory);

        // Explicitly call the destructor for each element in reverse order
        for (usize i = size; i-- > 0;)
            ptr[i].~T();
    }

    free_func(raw_memory);
}

// Allocates memory for a single object and places it there with placement new
template<typename T, typename ALLOC_FUNC, typename... Args>
inline std::enable_if_t<!std::is_array_v<T>, T*> memory_allocator(ALLOC_FUNC alloc_func,
                                                                  Args&&... args) {
    void* raw_memory = alloc_func(sizeof(T));
    if (raw_memory == nullptr)
        report_failed_allocation(sizeof(T));
    ASSERT_ALIGNED(raw_memory, alignof(T));
    return new (raw_memory) T(std::forward<Args>(args)...);
}

// Allocates memory for an array of unknown bound and places it there with placement new
template<typename T, typename ALLOC_FUNC>
inline std::enable_if_t<std::is_array_v<T>, std::remove_extent_t<T>*>
memory_allocator(ALLOC_FUNC alloc_func, usize num) {
    using ElementType = std::remove_extent_t<T>;

    const usize array_offset = std::max(sizeof(usize), alignof(ElementType));

    // Save the array size in the memory location
    const usize bytes      = array_offset + num * sizeof(ElementType);
    char*       raw_memory = reinterpret_cast<char*>(alloc_func(bytes));
    if (raw_memory == nullptr)
        report_failed_allocation(bytes);
    ASSERT_ALIGNED(raw_memory, alignof(T));

    new (raw_memory) usize(num);

    for (usize i = 0; i < num; ++i)
        new (raw_memory + array_offset + i * sizeof(ElementType)) ElementType();

    // Need to return the pointer at the start of the array so that
    // the indexing in unique_ptr<T[]> works.
    return reinterpret_cast<ElementType*>(raw_memory + array_offset);
}

//
//
// aligned unique ptr
//
//

template<typename T>
struct AlignedDeleter {
    void operator()(T* ptr) const { return memory_deleter<T>(ptr, std_aligned_free); }
};

template<typename T>
struct AlignedArrayDeleter {
    void operator()(T* ptr) const { return memory_deleter_array<T>(ptr, std_aligned_free); }
};

template<typename T>
using AlignedPtr =
  std::conditional_t<std::is_array_v<T>,
                     std::unique_ptr<T, AlignedArrayDeleter<std::remove_extent_t<T>>>,
                     std::unique_ptr<T, AlignedDeleter<T>>>;

// make_unique_aligned for single objects
template<typename T, typename... Args>
std::enable_if_t<!std::is_array_v<T>, AlignedPtr<T>> make_unique_aligned(Args&&... args) {
    const auto func = [](usize size) { return std_aligned_alloc(alignof(T), size); };
    T*         obj  = memory_allocator<T>(func, std::forward<Args>(args)...);

    return AlignedPtr<T>(obj);
}

// make_unique_aligned for arrays of unknown bound
template<typename T>
std::enable_if_t<std::is_array_v<T>, AlignedPtr<T>> make_unique_aligned(usize num) {
    using ElementType = std::remove_extent_t<T>;

    const auto   func   = [](usize size) { return std_aligned_alloc(alignof(ElementType), size); };
    ElementType* memory = memory_allocator<T>(func, num);

    return AlignedPtr<T>(memory);
}


// Get the first aligned element of an array.
// ptr must point to an array of size at least `sizeof(T) * N + alignment` bytes,
// where N is the number of elements in the array.
template<uintptr_t Alignment, typename T>
T* align_ptr_up(T* ptr) {
    static_assert(alignof(T) < Alignment);

    const uintptr_t ptrint = reinterpret_cast<uintptr_t>(reinterpret_cast<char*>(ptr));
    return reinterpret_cast<T*>(
      reinterpret_cast<char*>((ptrint + (Alignment - 1)) / Alignment * Alignment));
}

template<typename T, typename ByteT>
T load_as(const ByteT* buffer) {
    static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
    static_assert(sizeof(ByteT) == 1);

    if (reinterpret_cast<uintptr_t>(buffer) % alignof(T) != 0)
    {
        assert(false);
#ifdef __GNUC__
        __builtin_unreachable();
#endif
    }

    T value;
    std::memcpy(&value, buffer, sizeof(T));

    return value;
}

}  // namespace Stockfish

#endif  // #ifndef MEMORY_H_INCLUDED
