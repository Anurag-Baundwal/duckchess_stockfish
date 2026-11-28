/*
  duckchess_stockfish, a UCI duck chess engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "nnue.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "bitboard.h"
#include "position.h"

#if defined(USE_AVX2)
    #include <immintrin.h>
#elif defined(USE_NEON)
    #include <arm_neon.h>
#endif

#include "incbin/incbin.h"

// The network file is downloaded by the Makefile (target "net")
INCBIN(WakwakNet, "shotis.nnue");

namespace Stockfish::NNUE {

namespace {

constexpr int Q0         = 255;  // Feature transformer clipping
constexpr int Q          = 64;
constexpr int EvalScale  = 400;
constexpr int DuckInput  = 768;
constexpr int ClockInput = 832;

// Same layout as wakwak's file: the network is the raw file
struct alignas(64) Network {
    std::int16_t l0w[Inputs][L1];
    std::int16_t l0b[L1];
    std::int8_t  l1w[L1 / 4][L2 * 4];  // For 4 inputs, the 4 weights of each output
    std::int32_t l1b[L2];
    std::int32_t l2w[(L2 - 1) * 2][L3];
    std::int32_t l2b[L3];
    std::int32_t l3w[L3];
    std::int32_t l3b;
};

static_assert(sizeof(Network) == 2621568);

Network* net;

bool mirrored(Square ksq) { return file_of(ksq) > FILE_D; }

// The input square of s for a perspective
Square input_square(Square s, Color perspective, bool mirror) {
    if (perspective == BLACK)
        s = flip_rank(s);
    return mirror ? flip_file(s) : s;
}

int piece_input(Piece pc, Square s, Color perspective, bool mirror) {
    return (color_of(pc) != perspective) * 384 + (type_of(pc) - 1) * 64
         + input_square(s, perspective, mirror);
}

void add_row(std::int16_t* acc, int input) {
    const std::int16_t* w = net->l0w[input];
    for (int i = 0; i < L1; ++i)
        acc[i] += w[i];
}

void refresh(const Position& pos, Color perspective, std::int16_t* acc) {

    bool mirror = mirrored(pos.square<KING>(perspective));

    std::memcpy(acc, net->l0b, sizeof(net->l0b));

    for (Bitboard b = pos.pieces(WHITE) | pos.pieces(BLACK); b;)
    {
        Square s = pop_lsb(b);
        add_row(acc, piece_input(pos.piece_on(s), s, perspective, mirror));
    }
}

void apply(const DirtyPieces&  d,
           const std::int16_t* from,
           std::int16_t*       to,
           Color               perspective,
           bool                mirror) {

    const std::int16_t* add[2];
    const std::int16_t* sub[2];

    for (int i = 0; i < d.added; ++i)
        add[i] = net->l0w[piece_input(d.addedPc[i], d.addedSq[i], perspective, mirror)];
    for (int i = 0; i < d.removed; ++i)
        sub[i] = net->l0w[piece_input(d.removedPc[i], d.removedSq[i], perspective, mirror)];

    if (d.added == 1 && d.removed == 1)
        for (int i = 0; i < L1; ++i)
            to[i] = from[i] + add[0][i] - sub[0][i];

    else if (d.added == 1 && d.removed == 2)
        for (int i = 0; i < L1; ++i)
            to[i] = from[i] + add[0][i] - sub[0][i] - sub[1][i];

    else
    {
        assert(d.added == 2 && d.removed == 2);
        for (int i = 0; i < L1; ++i)
            to[i] = from[i] + add[0][i] + add[1][i] - sub[0][i] - sub[1][i];
    }
}

// The feature transformer output: for each perspective (side to move first),
// the first half of the 1536 values times the second half, each clipped to
// [0, 255] (the first half only from above), divided by 512.
void transform(const std::int16_t* us,
               const std::int16_t* them,
               const std::int16_t* clock,
               const std::int16_t* usDuck,
               const std::int16_t* themDuck,
               std::uint8_t*       out) {

    constexpr int Half = L1 / 2;

    const std::int16_t* acc[2]  = {us, them};
    const std::int16_t* duck[2] = {usDuck, themDuck};

    for (int p = 0; p < 2; ++p)
    {
        const std::int16_t* a = acc[p];
        const std::int16_t* d = duck[p];
        std::uint8_t*       o = out + p * Half;

#if defined(USE_AVX2)
        const __m256i max = _mm256_set1_epi16(Q0);
        const __m256i min = _mm256_setzero_si256();

        auto load = [&](int i) {
            __m256i v = _mm256_load_si256(reinterpret_cast<const __m256i*>(a + i));
            v = _mm256_add_epi16(v, _mm256_load_si256(reinterpret_cast<const __m256i*>(clock + i)));
            return _mm256_add_epi16(v, _mm256_load_si256(reinterpret_cast<const __m256i*>(d + i)));
        };

        for (int i = 0; i < Half; i += 32)
        {
            __m256i x0 = _mm256_min_epi16(load(i), max);
            __m256i x1 = _mm256_min_epi16(load(i + 16), max);
            __m256i y0 = _mm256_max_epi16(_mm256_min_epi16(load(i + Half), max), min);
            __m256i y1 = _mm256_max_epi16(_mm256_min_epi16(load(i + Half + 16), max), min);

            __m256i p0 = _mm256_mulhi_epi16(x0, _mm256_slli_epi16(y0, 7));
            __m256i p1 = _mm256_mulhi_epi16(x1, _mm256_slli_epi16(y1, 7));

            __m256i packed = _mm256_permute4x64_epi64(_mm256_packus_epi16(p0, p1), 0xd8);
            _mm256_store_si256(reinterpret_cast<__m256i*>(o + i), packed);
        }
#elif defined(USE_NEON)
        const int16x8_t max = vdupq_n_s16(Q0);
        const int16x8_t min = vdupq_n_s16(0);

        for (int i = 0; i < Half; i += 8)
        {
            int16x8_t x =
              vaddq_s16(vaddq_s16(vld1q_s16(a + i), vld1q_s16(clock + i)), vld1q_s16(d + i));
            int16x8_t y = vaddq_s16(vaddq_s16(vld1q_s16(a + i + Half), vld1q_s16(clock + i + Half)),
                                    vld1q_s16(d + i + Half));

            x = vminq_s16(x, max);
            y = vmaxq_s16(vminq_s16(y, max), min);

            // (x * y) >> 9, as wakwak's mulhi(x, y << 7) on x86
            int32x4_t lo = vshrq_n_s32(vmull_s16(vget_low_s16(x), vget_low_s16(y)), 9);
            int32x4_t hi = vshrq_n_s32(vmull_s16(vget_high_s16(x), vget_high_s16(y)), 9);

            vst1_u8(o + i, vqmovun_s16(vcombine_s16(vmovn_s32(lo), vmovn_s32(hi))));
        }
#else
        for (int i = 0; i < Half; ++i)
        {
            int x = std::int16_t(a[i] + clock[i] + d[i]);
            int y = std::int16_t(a[i + Half] + clock[i + Half] + d[i + Half]);

            x = std::min(x, Q0);
            y = std::clamp(y, 0, Q0);

            o[i] = std::uint8_t(std::clamp((x * y) >> 9, 0, 255));
        }
#endif
    }
}

// First hidden layer: only the non-zero groups of 4 inputs are multiplied
void propagate_l1(const std::uint8_t* in, std::int32_t* out) {

    const std::int32_t* in32 = reinterpret_cast<const std::int32_t*>(in);

#if defined(USE_AVX2)
    std::uint16_t nnz[L1 / 4];
    int           count = 0;

    for (int i = 0; i < L1 / 4; i += 8)
    {
        __m256i  v    = _mm256_load_si256(reinterpret_cast<const __m256i*>(in32 + i));
        unsigned mask = unsigned(
          _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(v, _mm256_setzero_si256()))));

        while (mask)
        {
            nnz[count++] = std::uint16_t(i + __builtin_ctz(mask));
            mask &= mask - 1;
        }
    }

    __m256i acc0 = _mm256_setzero_si256(), acc1 = _mm256_setzero_si256();

    auto dpbusd = [](__m256i acc, __m256i a, __m256i b) {
    #if defined(USE_AVXVNNI)
        return _mm256_dpbusd_avx_epi32(acc, a, b);
    #else
        __m256i product = _mm256_madd_epi16(_mm256_maddubs_epi16(a, b), _mm256_set1_epi16(1));
        return _mm256_add_epi32(acc, product);
    #endif
    };

    for (int k = 0; k < count; ++k)
    {
        const int      i = nnz[k];
        const __m256i  a = _mm256_set1_epi32(in32[i]);
        const __m256i* w = reinterpret_cast<const __m256i*>(net->l1w[i]);

        acc0 = dpbusd(acc0, a, _mm256_load_si256(w));
        acc1 = dpbusd(acc1, a, _mm256_load_si256(w + 1));
    }

    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out), acc0);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 8), acc1);

    for (int o = 0; o < L2; ++o)
        out[o] += net->l1b[o];

#elif defined(USE_NEON_DOTPROD)
    int32x4_t acc[4] = {vdupq_n_s32(0), vdupq_n_s32(0), vdupq_n_s32(0), vdupq_n_s32(0)};

    // The inputs are at most 127, so a signed dot product gives the same sums
    for (int i = 0; i < L1 / 4; ++i)
        if (in32[i])
        {
            const int8x16_t a = vreinterpretq_s8_s32(vdupq_n_s32(in32[i]));
            for (int j = 0; j < 4; ++j)
                acc[j] = vdotq_s32(acc[j], a, vld1q_s8(net->l1w[i] + 16 * j));
        }

    for (int j = 0; j < 4; ++j)
        vst1q_s32(out + 4 * j, vaddq_s32(acc[j], vld1q_s32(net->l1b + 4 * j)));

#else
    for (int o = 0; o < L2; ++o)
        out[o] = net->l1b[o];

    for (int i = 0; i < L1 / 4; ++i)
        if (in32[i])
            for (int o = 0; o < L2; ++o)
                for (int j = 0; j < 4; ++j)
                    out[o] += int(in[4 * i + j]) * net->l1w[i][4 * o + j];
#endif
}

std::int32_t forward(const std::int16_t* us,
                     const std::int16_t* them,
                     const std::int16_t* clock,
                     const std::int16_t* usDuck,
                     const std::int16_t* themDuck) {

    alignas(64) std::uint8_t ft[L1];
    alignas(64) std::int32_t l1[L2];

    transform(us, them, clock, usDuck, themDuck, ft);
    propagate_l1(ft, l1);

    // The last output is a skip connection to the result. The other 15 go
    // to the next layer twice: clipped to [0, Q^2] and squared.
    std::int32_t skip = l1[L2 - 1];
    std::int32_t act1[(L2 - 1) * 2];

    for (int o = 0; o < L2 - 1; ++o)
    {
        std::int32_t x  = l1[o] >> 2;
        std::int32_t sq = std::int32_t(std::uint32_t(x) * std::uint32_t(x));  // Wraps as in wakwak

        act1[o]          = std::clamp(x, 0, Q * Q);
        act1[o + L2 - 1] = std::min(sq, Q * Q * Q * Q) >> 12;
    }

    std::int32_t l2[L3];
    for (int o = 0; o < L3; ++o)
        l2[o] = net->l2b[o];

    for (int i = 0; i < (L2 - 1) * 2; ++i)
        for (int o = 0; o < L3; ++o)
            l2[o] += act1[i] * net->l2w[i][o];

    std::int32_t out = 0;
    for (int o = 0; o < L3; ++o)
        out += std::clamp(l2[o], 0, Q * Q * Q) * net->l3w[o];

    out += net->l3b;

    std::int64_t scaled = (std::int64_t(out) + std::int64_t(skip) * (Q * Q * Q / 256)) * EvalScale;

    return std::int32_t(scaled / (std::int64_t(Q) * Q * Q * Q));
}

Value evaluate_accumulators(const Position&     pos,
                            const std::int16_t* white,
                            const std::int16_t* black) {

    Color stm   = pos.side_to_move();
    int   r50   = std::min(pos.rule50_count(), 100);
    int   clock = r50 <= 5 ? 0 : (r50 + 9) / 10;

    alignas(64) static const std::int16_t NoDuck[L1] = {};

    const std::int16_t* duck[COLOR_NB] = {NoDuck, NoDuck};

    if (pos.duck_square() != SQ_NONE)
        for (Color c : {WHITE, BLACK})
        {
            bool mirror = mirrored(pos.square<KING>(c));
            duck[c]     = net->l0w[DuckInput + input_square(pos.duck_square(), c, mirror)];
        }

    const std::int16_t* acc[COLOR_NB] = {white, black};

    Value v = forward(acc[stm], acc[~stm], net->l0w[ClockInput + clock], duck[stm], duck[~stm]);

    return std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

}  // namespace


void init() {

    if (gWakwakNetSize < sizeof(Network))
    {
        std::cerr << "info string The embedded network is too small" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    static Network network;
    std::memcpy(&network, gWakwakNetData, sizeof(Network));
    net = &network;
}

Value evaluate(const Position& pos) {

    alignas(64) std::int16_t acc[COLOR_NB][L1];

    refresh(pos, WHITE, acc[WHITE]);
    refresh(pos, BLACK, acc[BLACK]);

    return evaluate_accumulators(pos, acc[WHITE], acc[BLACK]);
}


AccumulatorStack::AccumulatorStack() :
    stack(new Accumulator[MAX_PLY + 10]) {
    reset();
}

void AccumulatorStack::reset() {
    size                     = 1;
    stack[0].computed[WHITE] = stack[0].computed[BLACK] = false;
}

void AccumulatorStack::push(const Position& pos, Move m) {

    assert(size < MAX_PLY + 10);

    Accumulator& acc = stack[size++];
    DirtyPieces& d   = acc.dirty;

    acc.computed[WHITE] = acc.computed[BLACK] = false;
    d.removed = d.added = 0;
    d.rebuild[WHITE] = d.rebuild[BLACK] = false;

    Color  us   = pos.side_to_move();
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = pos.piece_on(from);

    auto remove = [&](Piece p, Square s) {
        d.removedPc[d.removed]   = p;
        d.removedSq[d.removed++] = s;
    };
    auto add = [&](Piece p, Square s) {
        d.addedPc[d.added]   = p;
        d.addedSq[d.added++] = s;
    };

    if (m.type_of() == CASTLING)
    {
        // Castling is encoded as "king captures friendly rook"
        Square kto = relative_square(us, to > from ? SQ_G1 : SQ_C1);
        Square rto = relative_square(us, to > from ? SQ_F1 : SQ_D1);

        remove(pc, from);
        remove(pos.piece_on(to), to);
        add(pc, kto);
        add(pos.piece_on(to), rto);

        d.rebuild[us] = mirrored(from) != mirrored(kto);
        return;
    }

    remove(pc, from);
    add(m.type_of() == PROMOTION ? make_piece(us, m.promotion_type()) : pc, to);

    if (m.type_of() == EN_PASSANT)
        remove(make_piece(~us, PAWN), to - pawn_push(us));
    else if (pos.piece_on(to) != NO_PIECE)
        remove(pos.piece_on(to), to);

    if (type_of(pc) == KING)
        d.rebuild[us] = mirrored(from) != mirrored(to);
}

// A move without a chess move: the pieces stay, so the accumulators too
void AccumulatorStack::push_null() {

    assert(size < MAX_PLY + 10);

    Accumulator& acc  = stack[size++];
    Accumulator& prev = stack[size - 2];

    for (Color c : {WHITE, BLACK})
        if ((acc.computed[c] = prev.computed[c]))
            std::memcpy(acc.values[c], prev.values[c], sizeof(acc.values[c]));

    // When the previous one isn't computed, update() rebuilds this one
    acc.dirty.removed = acc.dirty.added = 0;
    acc.dirty.rebuild[WHITE] = acc.dirty.rebuild[BLACK] = true;
}

void AccumulatorStack::update(const Position& pos, Color perspective) {

    const int top  = size - 1;
    int       last = top;

    // Find the last computed accumulator we can update from. If a king move
    // changed the mirroring, or the walk is long, rebuild from scratch.
    while (!stack[last].computed[perspective])
    {
        if (last == 0 || stack[last].dirty.rebuild[perspective] || top - last >= 8)
        {
            refresh(pos, perspective, stack[top].values[perspective]);
            stack[top].computed[perspective] = true;
            return;
        }
        --last;
    }

    bool mirror = mirrored(pos.square<KING>(perspective));

    for (int i = last + 1; i <= top; ++i)
    {
        apply(stack[i].dirty, stack[i - 1].values[perspective], stack[i].values[perspective],
              perspective, mirror);
        stack[i].computed[perspective] = true;
    }
}

Value AccumulatorStack::evaluate(const Position& pos) {

    Accumulator& acc = stack[size - 1];

    for (Color c : {WHITE, BLACK})
        if (!acc.computed[c])
            update(pos, c);

    return evaluate_accumulators(pos, acc.values[WHITE], acc.values[BLACK]);
}

}  // namespace Stockfish::NNUE
