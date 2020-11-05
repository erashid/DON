#pragma once

#if !defined(NDEBUG)
    #include <string>
#endif

#include "type.h"

// Magic holds all magic relevant data for a single square
struct Magic {

    // Compute the attack's index using the 'magic bitboards' approach
    uint16_t index(Bitboard occ) const noexcept {

    #if defined(USE_BMI2)
        return uint16_t( PEXT(occ, mask) );
    #elif defined(IS_64BIT)
        return uint16_t( ((occ & mask) * magic) >> shift );
    #else
        return uint16_t( (uint32_t((uint32_t(occ >> 0x00) & uint32_t(mask >> 0x00)) * uint32_t(magic >> 0x00))
                        ^ uint32_t((uint32_t(occ >> 0x20) & uint32_t(mask >> 0x20)) * uint32_t(magic >> 0x20))) >> shift );
    #endif
    }

    // Return attacks
    Bitboard attacksBB(Bitboard occ) const noexcept {
        return attacks[index(occ)];
    }

    Bitboard *attacks;
    Bitboard  mask;

#if !defined(USE_BMI2)
    Bitboard  magic;
    uint8_t   shift;
#endif

};

constexpr Bitboard BoardBB{ U64(0xFFFFFFFFFFFFFFFF) };

constexpr Bitboard SquareBB[SQUARES]{
#define S_02(n)  U64(1)<<(2*(n)),  U64(1)<<(2*(n)+1)
#define S_04(n)      S_02(2*(n)),      S_02(2*(n)+1)
#define S_08(n)      S_04(2*(n)),      S_04(2*(n)+1)
#define S_16(n)      S_08(2*(n)),      S_08(2*(n)+1)
    S_16(0),
    S_16(1),
    S_16(2),
    S_16(3),
#undef S_16
#undef S_08
#undef S_04
#undef S_02
};
constexpr Bitboard squareBB(Square s) noexcept { assert(isOk(s)); return SquareBB[s]; }

constexpr Bitboard FileBB[FILES]{
    U64(0x0101010101010101),
    U64(0x0202020202020202),
    U64(0x0404040404040404),
    U64(0x0808080808080808),
    U64(0x1010101010101010),
    U64(0x2020202020202020),
    U64(0x4040404040404040),
    U64(0x8080808080808080)
};
constexpr Bitboard fileBB(File f) noexcept { assert(isOk(f)); return FileBB[f]; }
constexpr Bitboard fileBB(Square s) noexcept { return fileBB(sFile(s)); }

constexpr Bitboard RankBB[RANKS]{
    U64(0x00000000000000FF),
    U64(0x000000000000FF00),
    U64(0x0000000000FF0000),
    U64(0x00000000FF000000),
    U64(0x000000FF00000000),
    U64(0x0000FF0000000000),
    U64(0x00FF000000000000),
    U64(0xFF00000000000000)
};
constexpr Bitboard rankBB(Rank r) noexcept { assert(isOk(r)); return RankBB[r]; }
constexpr Bitboard rankBB(Square s) noexcept { return rankBB(sRank(s)); }

constexpr Bitboard ColorBB[COLORS]{
    U64(0x55AA55AA55AA55AA),
    U64(0xAA55AA55AA55AA55)
};
constexpr Bitboard colorBB(Color c) noexcept { assert(isOk(c)); return ColorBB[c]; }

constexpr Bitboard FrontRankBB[COLORS][RANKS]{
    {
        rankBB(RANK_8)|rankBB(RANK_7)|rankBB(RANK_6)|rankBB(RANK_5)|rankBB(RANK_4)|rankBB(RANK_3)|rankBB(RANK_2),
        rankBB(RANK_8)|rankBB(RANK_7)|rankBB(RANK_6)|rankBB(RANK_5)|rankBB(RANK_4)|rankBB(RANK_3),
        rankBB(RANK_8)|rankBB(RANK_7)|rankBB(RANK_6)|rankBB(RANK_5)|rankBB(RANK_4),
        rankBB(RANK_8)|rankBB(RANK_7)|rankBB(RANK_6)|rankBB(RANK_5),
        rankBB(RANK_8)|rankBB(RANK_7)|rankBB(RANK_6),
        rankBB(RANK_8)|rankBB(RANK_7),
        rankBB(RANK_8),
        0,
    },
    {
        0,
        rankBB(RANK_1),
        rankBB(RANK_1)|rankBB(RANK_2),
        rankBB(RANK_1)|rankBB(RANK_2)|rankBB(RANK_3),
        rankBB(RANK_1)|rankBB(RANK_2)|rankBB(RANK_3)|rankBB(RANK_4),
        rankBB(RANK_1)|rankBB(RANK_2)|rankBB(RANK_3)|rankBB(RANK_4)|rankBB(RANK_5),
        rankBB(RANK_1)|rankBB(RANK_2)|rankBB(RANK_3)|rankBB(RANK_4)|rankBB(RANK_5)|rankBB(RANK_6),
        rankBB(RANK_1)|rankBB(RANK_2)|rankBB(RANK_3)|rankBB(RANK_4)|rankBB(RANK_5)|rankBB(RANK_6)|rankBB(RANK_7),
    }
};
/// frontRanksBB() returns ranks in front of the given square
constexpr Bitboard frontRanksBB(Color c, Square s) noexcept { return FrontRankBB[c][sRank(s)]; }

constexpr Bitboard SlotFileBB[CASTLE_SIDES+1]{
    fileBB(FILE_E)|fileBB(FILE_F)|fileBB(FILE_G)|fileBB(FILE_H),    // K-File
    fileBB(FILE_A)|fileBB(FILE_B)|fileBB(FILE_C)|fileBB(FILE_D),    // Q-File
    fileBB(FILE_C)|fileBB(FILE_D)|fileBB(FILE_E)|fileBB(FILE_F)     // C-File
};
constexpr Bitboard slotFileBB(CastleSide cs) noexcept { return SlotFileBB[cs]; }

extern uint8_t Distance[SQUARES][SQUARES];

extern Bitboard LineBB[SQUARES][SQUARES];

extern Bitboard PawnAttacksBB[COLORS][SQUARES];
extern Bitboard PieceAttacksBB[PIECE_TYPES][SQUARES];

extern Magic BMagics[SQUARES];
extern Magic RMagics[SQUARES];

#if !defined(USE_POPCNT)
extern uint8_t PopCount[USHRT_MAX+1]; // 16-bit
#endif

constexpr Bitboard operator~(Square s) noexcept { return ~squareBB(s); }

constexpr Bitboard operator&(Square s, Bitboard bb) noexcept { return bb & squareBB(s); }
constexpr Bitboard operator|(Square s, Bitboard bb) noexcept { return bb | squareBB(s); }
constexpr Bitboard operator^(Square s, Bitboard bb) noexcept { return bb ^ squareBB(s); }

constexpr Bitboard operator&(Bitboard bb, Square s) noexcept { return bb & squareBB(s); }
constexpr Bitboard operator|(Bitboard bb, Square s) noexcept { return bb | squareBB(s); }
constexpr Bitboard operator^(Bitboard bb, Square s) noexcept { return bb ^ squareBB(s); }

inline Bitboard& operator&=(Bitboard &bb, Square s) noexcept { return bb &= squareBB(s); }
inline Bitboard& operator|=(Bitboard &bb, Square s) noexcept { return bb |= squareBB(s); }
inline Bitboard& operator^=(Bitboard &bb, Square s) noexcept { return bb ^= squareBB(s); }

constexpr Bitboard operator|(Square s1, Square s2) noexcept { return squareBB(s1) | s2; }

constexpr bool contains(Bitboard bb, Square s) noexcept { return (bb & squareBB(s)) != 0; }

constexpr bool moreThanOne(Bitboard bb) noexcept { return (bb & (bb - 1)) != 0; }

/// Shift the bitboard using delta
template<Direction> constexpr Bitboard shift(Bitboard) noexcept;
template<> constexpr Bitboard shift<NORTH     >(Bitboard bb) noexcept { return (bb) <<  8; }
template<> constexpr Bitboard shift<SOUTH     >(Bitboard bb) noexcept { return (bb) >>  8; }
template<> constexpr Bitboard shift<NORTH_2   >(Bitboard bb) noexcept { return (bb) << 16; }
template<> constexpr Bitboard shift<SOUTH_2   >(Bitboard bb) noexcept { return (bb) >> 16; }
// If (shifting & 7) != 0 then  bound clipping is done (~fileBB(FILE_A) or ~fileBB(FILE_H))
template<> constexpr Bitboard shift<EAST      >(Bitboard bb) noexcept { return (bb & ~fileBB(FILE_H)) << 1; }
template<> constexpr Bitboard shift<WEST      >(Bitboard bb) noexcept { return (bb & ~fileBB(FILE_A)) >> 1; }
template<> constexpr Bitboard shift<NORTH_EAST>(Bitboard bb) noexcept { return (bb & ~fileBB(FILE_H)) << 9; }
template<> constexpr Bitboard shift<NORTH_WEST>(Bitboard bb) noexcept { return (bb & ~fileBB(FILE_A)) << 7; }
template<> constexpr Bitboard shift<SOUTH_EAST>(Bitboard bb) noexcept { return (bb & ~fileBB(FILE_H)) >> 7; }
template<> constexpr Bitboard shift<SOUTH_WEST>(Bitboard bb) noexcept { return (bb & ~fileBB(FILE_A)) >> 9; }

constexpr Bitboard adjacentFilesBB(Square s) noexcept {
    return shift<EAST >(fileBB(s))
         | shift<WEST >(fileBB(s));
}
//constexpr Bitboard adjacentRanksBB(Square s) noexcept {
//    return shift<NORTH>(rankBB(s))
//         | shift<SOUTH>(rankBB(s));
//}

constexpr Bitboard frontSquaresBB(Color c, Square s) noexcept { return frontRanksBB(c, s) & fileBB(s); }
constexpr Bitboard pawnAttackSpan(Color c, Square s) noexcept { return frontRanksBB(c, s) & adjacentFilesBB(s); }
constexpr Bitboard pawnPassSpan  (Color c, Square s) noexcept { return frontSquaresBB(c, s) | pawnAttackSpan(c, s); }

/// lineBB() returns a Bitboard representing an entire line
/// (from board edge to board edge) that intersects the given squares.
/// If the given squares are not on a same file/rank/diagonal, return 0.
/// Ex. lineBB(SQ_C4, SQ_F7) returns a bitboard with the A2-G8 diagonal.
inline Bitboard lineBB(Square s1, Square s2) noexcept {
    return LineBB[s1][s2];
}
/// betweenBB() returns squares that are linearly between the given squares
/// If the given squares are not on a same file/rank/diagonal, return 0.
/// Ex. betweenBB(SQ_C4, SQ_F7) returns a bitboard with squares D5 and E6.
inline Bitboard betweenBB(Square s1, Square s2) noexcept {
    Bitboard const line{
        lineBB(s1, s2)
      & ((BoardBB << s1) ^ (BoardBB << s2)) };
    // Exclude LSB
    return line & (line - 1); //line & ~std::min(s1, s2);
}
/// aligned() Check the squares s1, s2 and s3 are aligned on a straight line.
inline bool aligned(Square s1, Square s2, Square s3) noexcept {
    return contains(lineBB(s1, s2), s3);
}


/// distance() functions return the distance between s1 and s2
/// defined as the number of steps for a king in s1 to reach s2.

template<typename T = Square> inline int32_t distance(Square, Square) noexcept;
template<> inline int32_t distance<File>(Square s1, Square s2) noexcept {
    return std::abs(sFile(s1) - sFile(s2));
}
template<> inline int32_t distance<Rank>(Square s1, Square s2) noexcept {
    return std::abs(sRank(s1) - sRank(s2));
}

template<> inline int32_t distance<Square>(Square s1, Square s2) noexcept {
    //return std::max(distance<File>(s1, s2), distance<Rank>(s1, s2));
    return Distance[s1][s2];
}

// Fold file [ABCDEFGH] to file [ABCDDCBA]
constexpr int32_t edgeDistance(File f) noexcept { return std::min(f - FILE_A, FILE_H - f); }
// Fold rank [12345678] to rank [12344321]
constexpr int32_t edgeDistance(Rank r) noexcept { return std::min(r - RANK_1, RANK_8 - r); }

constexpr Direction PawnPush[COLORS]{
    NORTH, SOUTH
};

template<Color C> constexpr Bitboard pawnSglPushBB(Bitboard bb) noexcept { return shift<PawnPush[C]>(bb); }
template<Color C> constexpr Bitboard pawnDblPushBB(Bitboard bb) noexcept { return shift<PawnPush[C] * 2>(bb); }

constexpr Direction PawnLAtt[COLORS]{
    NORTH_WEST, SOUTH_EAST
};
constexpr Direction PawnRAtt[COLORS]{
    NORTH_EAST, SOUTH_WEST
};

template<Color C> constexpr Bitboard pawnLAttackBB(Bitboard bb) noexcept { return shift<PawnLAtt[C]>(bb); }
template<Color C> constexpr Bitboard pawnRAttackBB(Bitboard bb) noexcept { return shift<PawnRAtt[C]>(bb); }
template<Color C> constexpr Bitboard pawnSglAttackBB(Bitboard bb) noexcept { return pawnLAttackBB<C>(bb) | pawnRAttackBB<C>(bb); }
template<Color C> constexpr Bitboard pawnDblAttackBB(Bitboard bb) noexcept { return pawnLAttackBB<C>(bb) & pawnRAttackBB<C>(bb); }

inline Bitboard pawnAttacksBB(Color c, Square s) noexcept {
    return PawnAttacksBB[c][s];
}

/// attacksBB() returns the pseudo-attacks by piece-type assuming an empty board
template<PieceType PT> inline Bitboard attacksBB(Square s) noexcept {
    assert(PT != PAWN);
    return PieceAttacksBB[PT][s];
}

/// attacksBB() returns attacks by piece-type from the square on occupancy
template<PieceType> Bitboard attacksBB(Square, Bitboard) noexcept;

template<> inline Bitboard attacksBB<NIHT>(Square s, Bitboard) noexcept {
    return PieceAttacksBB[NIHT][s];
}
template<> inline Bitboard attacksBB<BSHP>(Square s, Bitboard occ) noexcept {
    return BMagics[s].attacksBB(occ);
}
template<> inline Bitboard attacksBB<ROOK>(Square s, Bitboard occ) noexcept {
    return RMagics[s].attacksBB(occ);
}
template<> inline Bitboard attacksBB<QUEN>(Square s, Bitboard occ) noexcept {
    return attacksBB<BSHP>(s, occ)
         | attacksBB<ROOK>(s, occ);
}

/// attacksBB() returns attacks of the piece-type from the square on occupancy
inline Bitboard attacksBB(PieceType pt, Square s, Bitboard occ) noexcept {
    assert(NIHT <= pt && pt <= KING);
    return
        pt == NIHT ? attacksBB<NIHT>(s) :
        pt == BSHP ? attacksBB<BSHP>(s, occ) :
        pt == ROOK ? attacksBB<ROOK>(s, occ) :
        pt == QUEN ? attacksBB<QUEN>(s, occ) :
      /*pt == KING*/ attacksBB<KING>(s);
}

/// popCount() counts the number of ones in a bitboard
inline int32_t popCount(Bitboard bb) noexcept {

#if !defined(USE_POPCNT)
    //Bitboard x = bb;
    //x -= (x >> 1) & 0x5555555555555555;
    //x = ((x >> 0) & 0x3333333333333333)
    //  + ((x >> 2) & 0x3333333333333333);
    //x = ((x >> 0) + (x >> 4)) & 0x0F0F0F0F0F0F0F0F;
    //return (x * 0x0101010101010101) >> 56;

    union { Bitboard b; uint16_t u[4]; } v{ bb };
    return PopCount[v.u[0]]
         + PopCount[v.u[1]]
         + PopCount[v.u[2]]
         + PopCount[v.u[3]];

#elif defined(_MSC_VER) || defined(__INTEL_COMPILER)
    return int32_t( _mm_popcnt_u64(bb) );
#else // Assumed gcc or compatible compiler
    return int32_t( __builtin_popcountll(bb) );
#endif
}

/// scanLSq() return the least significant bit in a non-zero bitboard
inline Square scanLSq(Bitboard bb) noexcept {
    assert(bb != 0);

#if defined(__GNUC__) // GCC, Clang, ICC
    return Square(__builtin_ctzll(bb));
#elif defined(_MSC_VER) // MSVC
    unsigned long index;

    #if defined(IS_64BIT)
    _BitScanForward64(&index, bb);
    #else
    if (uint32_t(bb >> 0) != 0) {
        _BitScanForward(&index, uint32_t(bb >> 0x00));
    } else {
        _BitScanForward(&index, uint32_t(bb >> 0x20));
        index += 0x20;
    }
    #endif
    return Square(index);
#else // Compiler is neither GCC nor MSVC compatible
    // Assembly code by Heinz van Saanen
    Bitboard sq;
    __asm__("bsfq %1, %0": "=r"(sq) : "rm"(bb));
    return Square(sq);
#endif
}
/// scanLSq() return the most significant bit in a non-zero bitboard
inline Square scanMSq(Bitboard bb) noexcept {
    assert(bb != 0);

#if defined(__GNUC__) // GCC, Clang, ICC
    return Square(SQ_H8 - __builtin_clzll(bb));
#elif defined(_MSC_VER) // MSVC
    unsigned long index;

    #if defined(IS_64BIT)
    _BitScanReverse64(&index, bb);
    #else
    if (uint32_t(bb >> 0x20) != 0) {
        _BitScanReverse(&index, uint32_t(bb >> 0x20));
        index += 0x20;
    } else {
        _BitScanReverse(&index, uint32_t(bb >> 0x00));
    }
    #endif
    return Square(index);
#else // Compiler is neither GCC nor MSVC compatible
    // Assembly code by Heinz van Saanen
    Bitboard sq;
    __asm__("bsrq %1, %0": "=r"(sq) : "rm"(bb));
    return Square(sq);
#endif
}

// Find the most advanced square in the given bitboard relative to the given color.
template<Color C> inline Square scanFrontMostSq(Bitboard) noexcept;
template<> inline Square scanFrontMostSq<WHITE>(Bitboard bb) noexcept { assert(bb != 0); return scanMSq(bb); }
template<> inline Square scanFrontMostSq<BLACK>(Bitboard bb) noexcept { assert(bb != 0); return scanLSq(bb); }

inline Square popLSq(Bitboard &bb) noexcept {
    assert(bb != 0);
    Square const sq{ scanLSq(bb) };
    bb &= (bb - 1); // bb &= ~(U64(1) << sq);
    return sq;
}
//inline Square popMSq(Bitboard &bb) noexcept {
//    assert(bb != 0);
//    Square const sq{ scanMSq(bb) };
//    bb &= ~sq;
//    return sq;
//}

namespace Bitboards {

    extern void initialize() noexcept;

#if !defined(NDEBUG)
    extern std::string toString(Bitboard) noexcept;
#endif

}
