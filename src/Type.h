#ifndef _TYPE_H_INC_
#define _TYPE_H_INC_

#include <cctype>
#include <climits>
#include <cmath>
#include <string>
#include <cstring>
#include <vector>
#include <algorithm>
#include <functional>
#include <cassert>
#include <iosfwd>
#include <chrono>
#include <iomanip>
#include <sstream>
//#include <iterator>
//#include <locale>
//#include <utility>

// Compiling:
// With Makefile (e.g. for Linux and OSX), configuration is done automatically, to get started type 'make help'.
// Without Makefile (e.g. with Microsoft Visual Studio) some switches need to be set manually:
//
// -DNDEBUG    | Disable debugging mode. Always use this.
// -DPREFETCH  | Enable use of prefetch asm-instruction.
//             | Don't enable it if want the executable to run on some very old machines.
// -DPOP       | Enable use of internal pop count table. Works in both 32-bit & 64-bit mode.
//             | For compiling requires hardware without ABM support.
// -DABM       | Add runtime support for use of ABM asm-instruction. Works only in 64-bit mode.
//             | For compiling requires hardware with ABM support.
// -DBM2       | Add runtime support for use of BM2 asm-instruction. Works only in 64-bit mode.
//             | For compiling requires hardware with BM2 support.
// -DLPAGES    | Add runtime support for large pages.

// Predefined macros hell:
//
// __GNUC__           Compiler is gcc, Clang or Intel on Linux
// __INTEL_COMPILER   Compiler is Intel
// _MSC_VER           Compiler is MSVC or Intel on Windows
// _WIN32             Building on Windows (any)
// _WIN64             Building on Windows 64 bit

// Windows or MinGW
#if defined(_WIN32)

// Auto make 64-bit compiles
#   if defined(_WIN64) && defined(_MSC_VER) // No Makefile used
#       if !defined(BIT64)
#           define BIT64
#       endif
#   endif

#endif

#if defined(BM2)
#   include <immintrin.h>   // Header for BMI2 instructions
// BEXTR = Bit field extract (with register)
// PDEP  = Parallel bits deposit
// PEXT  = Parallel bits extract
// BLSR  = Reset lowest set bit
#   if defined(BIT64)
#       define BEXTR(b, m, l)   _bextr_u64 (b, m, l)
#       define PDEP(b, m)       _pdep_u64 (b, m)
#       define PEXT(b, m)       _pext_u64 (b, m)
#       define BLSR(b)          _blsr_u64 (b)
#   else
//#       define BEXTR(b, m, l)   _bextr_u32 (b, m, l)
//#       define PDEP(b, m)       _pdep_u32 (b, m)
//#       define PEXT(b, m)       _pext_u32 (b, m)
//#       define BLSR(b)          _blsr_u32 (b)
#   endif
#endif

#undef S32
#undef U32
#undef S64
#undef U64

#if defined(_MSC_VER)
// Disable some silly and noisy warning from MSVC compiler
#   pragma warning (disable: 4127) // Conditional expression is constant
#   pragma warning (disable: 4146) // Unary minus operator applied to unsigned type
#   pragma warning (disable: 4800) // Forcing value to bool 'true' or 'false'

// MSVC does not support <inttypes.h>
//#   include <stdint.h>
//typedef         int8_t     i08;
//typedef        uint8_t     u08;
//typedef         int16_t    i16;
//typedef        uint16_t    u16;
//typedef         int32_t    i32;
//typedef        uint32_t    u32;
//typedef         int64_t    i64;
//typedef        uint64_t    u64;

typedef   signed __int8     i08;
typedef unsigned __int8     u08;
typedef   signed __int16    i16;
typedef unsigned __int16    u16;
typedef   signed __int32    i32;
typedef unsigned __int32    u32;
typedef   signed __int64    i64;
typedef unsigned __int64    u64;

#   define  S32(X) (X ##  i32)
#   define  U32(X) (X ## ui32)
#   define  S64(X) (X ##  i64)
#   define  U64(X) (X ## ui64)

#else

#   include <inttypes.h>

typedef          int8_t    i08;
typedef         uint8_t    u08;
typedef         int16_t    i16;
typedef        uint16_t    u16;
typedef         int32_t    i32;
typedef        uint32_t    u32;
typedef         int64_t    i64;
typedef        uint64_t    u64;

#   define S32(X) (X ##   L)
#   define U32(X) (X ##  UL)
#   define S64(X) (X ##  LL)
#   define U64(X) (X ## ULL)

#endif

typedef u64 Key;
typedef u64 Bitboard;

const u16 MaxPlies   = 128; // Maximum Plies

enum File : i08
{
    F_A,
    F_B,
    F_C,
    F_D,
    F_E,
    F_F,
    F_G,
    F_H,
    F_NO,
};

enum Rank : i08
{
    R_1,
    R_2,
    R_3,
    R_4,
    R_5,
    R_6,
    R_7,
    R_8,
    R_NO,
};

enum Color : i08
{
    WHITE,
    BLACK,
    CLR_NO,
};

// Square needs 6-bits to be stored
// bit 0-2: File
// bit 3-5: Rank
enum Square : i08
{
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NO,
};

enum Delta : i08
{
    DEL_O =  0,

    DEL_E =  1,
    DEL_N =  8,
    
    DEL_W = -i08(DEL_E),
    DEL_S = -i08(DEL_N),

    DEL_NN = i08(DEL_N) + i08(DEL_N),
    DEL_EE = i08(DEL_E) + i08(DEL_E),
    DEL_SS = i08(DEL_S) + i08(DEL_S),
    DEL_WW = i08(DEL_W) + i08(DEL_W),

    DEL_NE = i08(DEL_N) + i08(DEL_E),
    DEL_SE = i08(DEL_S) + i08(DEL_E),
    DEL_SW = i08(DEL_S) + i08(DEL_W),
    DEL_NW = i08(DEL_N) + i08(DEL_W),

    DEL_NNE = i08(DEL_NN) + i08(DEL_E),
    DEL_NNW = i08(DEL_NN) + i08(DEL_W),

    DEL_EEN = i08(DEL_EE) + i08(DEL_N),
    DEL_EES = i08(DEL_EE) + i08(DEL_S),

    DEL_SSE = i08(DEL_SS) + i08(DEL_E),
    DEL_SSW = i08(DEL_SS) + i08(DEL_W),

    DEL_WWN = i08(DEL_WW) + i08(DEL_N),
    DEL_WWS = i08(DEL_WW) + i08(DEL_S),

};

enum CastleSide : i08
{
    CS_KING,    // King  Side (Short Castle)
    CS_QUEN,    // Queen Side (Long  Castle)
    CS_NO,
};

// Castle Right defined as in Polyglot book hash key
enum CastleRight : u08
{
    CR_NONE  = 0,                   // 0000
    CR_WKING = 1,                   // 0001
    CR_WQUEN = u08(CR_WKING) << 1,  // 0010
    CR_BKING = u08(CR_WKING) << 2,  // 0100
    CR_BQUEN = u08(CR_WKING) << 3,  // 1000

    CR_WHITE = u08(CR_WKING) | u08(CR_WQUEN),   // 0011
    CR_BLACK = u08(CR_BKING) | u08(CR_BQUEN),   // 1100
    CR_KING  = u08(CR_WKING) | u08(CR_BKING),   // 0101
    CR_QUEN  = u08(CR_WQUEN) | u08(CR_BQUEN),   // 1010
    CR_ANY   = u08(CR_WHITE) | u08(CR_BLACK),   // 1111
    CR_NO,
};

enum PieceType : i08
{
    PAWN     , // 000
    NIHT     , // 001
    BSHP     , // 010
    ROOK     , // 011
    QUEN     , // 100
    KING     , // 101
    NONE     , // 110
    MAX_PTYPE, // 111
    NONPAWN  ,
};

// Piece needs 4-bits to be stored
// bit 0-2: Type of piece
// bit   3: Color of piece { White = 0..., Black = 1... }
enum Piece : u08
{
    W_PAWN   = 0, //  0000
    W_NIHT      , //  0001
    W_BSHP      , //  0010
    W_ROOK      , //  0011
    W_QUEN      , //  0100
    W_KING      , //  0101

    NO_PIECE = 6, //  0110

    B_PAWN   = 8, //  1000
    B_NIHT      , //  1001
    B_BSHP      , //  1010
    B_ROOK      , //  1011
    B_QUEN      , //  1100
    B_KING      , //  1101

    MAX_PIECE   , //  1110
};

enum MoveType : u16
{
    NORMAL    = 0x0000, // 0000
    CASTLE    = 0x4000, // 0100
    ENPASSANT = 0x8000, // 1000
    PROMOTE   = 0xC000, // 11xx
};

// Move needs 16-bits to be stored
//
// bit 00-05: Destiny square: (0...63)
// bit 06-11: Origin square: (0...63)
// bit 12-13: Promotion piece: (Knight...Queen) - 1
// bit 14-15: Move flag: (0) Normal (1) Castle (2) En-Passant (3) Promotion
//
// Special cases are MOVE_NONE and MOVE_NULL.
enum Move : u16
{
    MOVE_NONE = 0x00,
    MOVE_NULL = 0x41,
};

enum Value : i32
{
    VALUE_ZERO      = 0,
    VALUE_DRAW      = 0,

    VALUE_NONE      = (1 << 15) - 1,
    VALUE_INFINITE  = +i16(VALUE_NONE) - 1,

    VALUE_MATE      = +i16(VALUE_INFINITE) - 1,
    VALUE_KNOWN_WIN = +i16(VALUE_MATE) / 3,

    VALUE_MATE_IN_MAX_PLY = +i16(VALUE_MATE) - 2 * MaxPlies,

    VALUE_MG_PAWN =  198,  VALUE_EG_PAWN =  258,
    VALUE_MG_NIHT =  817,  VALUE_EG_NIHT =  896,
    VALUE_MG_BSHP =  836,  VALUE_EG_BSHP =  907,
    VALUE_MG_ROOK = 1270,  VALUE_EG_ROOK = 1356,
    VALUE_MG_QUEN = 2521,  VALUE_EG_QUEN = 2658,

    VALUE_SPACE   = 12222,
    VALUE_MIDGAME = 15581, VALUE_ENDGAME = 3998,
};

// Score needs 32-bits to be stored
// the lower 16-bits are used to store the endgame value
// the upper 16-bits are used to store the midgame value
enum Score : u32
{
    SCORE_ZERO = 0,
};

enum Bound : u08
{
    // NONE BOUND           - NO_NODE
    BOUND_NONE  = 0,

    // UPPER (BETA) BOUND   - ALL_NODE
    // BETA evaluation, when do not reach up to ALPHA the move is 'Fail-Low' 
    // All moves were searched, but none improved ALPHA.
    // A fail-low indicates that this position was not good enough.
    // because there are some other means of reaching a position that is better.
    // Engine will not make the move that allowed the opponent to put in this position.
    // What the actual evaluation of the position was?
    // It was atmost ALPHA (or lower).
    BOUND_UPPER = 1,

    // LOWER (ALPHA) BOUND  - CUT_NODE
    // ALPHA evaluation, when exceed BETA the move is too good.
    // 'Fail-High' or 'BETA-Cutoff' and cut off the rest of the search.
    // Since some of the search is cut off, What the actual evaluation of the position was?
    // It was atleast BETA or higher.
    BOUND_LOWER = 2,

    // EXACT (-) BOUND      - PV_NODE
    // EXACT evaluation, when receive a definite evaluation,
    // that is searched all possible moves and received a new best move
    // (or received an evaluation from quiescent search that was between ALPHA and BETA).
    // if score for max-player was improved (score > alfa), alfa the max so far,
    // while the min-player improved his score as well (score < beta), beta the min so far.
    // The current node searched was an expected PV-Node,
    // which was confirmed by the search in finding and collecting a principal variation.
    BOUND_EXACT = BOUND_LOWER | BOUND_UPPER,

};

enum PhaseType : u08
{
    MG      = 0,
    EG      = 1,
    PH_NO   = 2,
};

enum Phase : u08
{
    PHASE_ENDGAME   =   0,
    PHASE_MIDGAME   = 128,
};

enum Scale : u08
{
    SCALE_DRAW    =   0,
    SCALE_ONEPAWN =  48,
    SCALE_NORMAL  =  64,
    SCALE_MAX     = 128,
    SCALE_NONE    = 255,
};

#undef BASIC_OPERATORS
#undef ARTHMAT_OPERATORS
#undef INC_DEC_OPERATORS

#define BASIC_OPERATORS(T)                                                       \
    inline T  operator+  (T  t, i32 i) { return T(i32(t) + i); }                 \
    inline T  operator-  (T  t, i32 i) { return T(i32(t) - i); }                 \
    inline T& operator+= (T &t, i32 i) { t = T(i32(t) + i); return t; }          \
    inline T& operator-= (T &t, i32 i) { t = T(i32(t) - i); return t; }

#define ARTHMAT_OPERATORS(T)                                                     \
    BASIC_OPERATORS(T)                                                           \
    inline T  operator+  (T  t1, T t2) { return T(i32(t1) + i32(t2)); }          \
    inline T  operator-  (T  t1, T t2) { return T(i32(t1) - i32(t2)); }          \
    inline T  operator*  (T  t, i32 i) { return T(i32(t) * i); }                 \
    inline T  operator+  (T  t       ) { return T(+i32(t)); }                    \
    inline T  operator-  (T  t       ) { return T(-i32(t)); }                    \
    inline T& operator+= (T &t1, T t2) { t1 = T(i32(t1) + i32(t2)); return t1; } \
    inline T& operator-= (T &t1, T t2) { t1 = T(i32(t1) - i32(t2)); return t1; } \
    inline T& operator*= (T &t, i32 i) { t = T(i32(t) * i); return t; }

#define INC_DEC_OPERATORS(T)                                                     \
    inline T& operator++ (T &t) { t = T(i32(t) + 1); return t; }                 \
    inline T& operator-- (T &t) { t = T(i32(t) - 1); return t; }

BASIC_OPERATORS(File)
INC_DEC_OPERATORS(File)

BASIC_OPERATORS(Rank)
INC_DEC_OPERATORS(Rank)

INC_DEC_OPERATORS(Color)

inline Delta  operator+  (Delta  d1, Delta d2) { return Delta(i32(d1) + i32(d2)); }
inline Delta  operator-  (Delta  d1, Delta d2) { return Delta(i32(d1) - i32(d2)); }
inline Delta  operator*  (Delta   d, i32    i) { return Delta(i32(d) * i); }
inline Delta  operator/  (Delta   d, i32    i) { return Delta(i32(d) / i); }

inline Square  operator+  (Square  s, Delta d) { return Square(i32(s) + i32(d)); }
inline Square  operator-  (Square  s, Delta d) { return Square(i32(s) - i32(d)); }
inline Square& operator+= (Square &s, Delta d) { s = Square(i32(s) + i32(d)); return s; }
inline Square& operator-= (Square &s, Delta d) { s = Square(i32(s) - i32(d)); return s; }
inline Delta   operator-  (Square s1, Square s2) { return Delta(i32(s1) - i32(s2)); }
INC_DEC_OPERATORS(Square)

INC_DEC_OPERATORS(CastleSide)

inline CastleRight  operator|  (CastleRight  cr, i32 i) { return CastleRight(i32(cr) | i); }
inline CastleRight  operator&  (CastleRight  cr, i32 i) { return CastleRight(i32(cr) & i); }
inline CastleRight  operator^  (CastleRight  cr, i32 i) { return CastleRight(i32(cr) ^ i); }
inline CastleRight& operator|= (CastleRight &cr, i32 i) { cr = CastleRight(i32(cr) | i); return cr; }
inline CastleRight& operator&= (CastleRight &cr, i32 i) { cr = CastleRight(i32(cr) & i); return cr; }
inline CastleRight& operator^= (CastleRight &cr, i32 i) { cr = CastleRight(i32(cr) ^ i); return cr; }

INC_DEC_OPERATORS(PieceType)

inline Move& operator|= (Move &m, i32 i) { m = Move(i32(m) | i); return m; }
inline Move& operator&= (Move &m, i32 i) { m = Move(i32(m) & i); return m; }

ARTHMAT_OPERATORS(Value)
INC_DEC_OPERATORS(Value)
inline Value  operator*  (Value  v, double d) { return Value(i32(i32(v) * d)); }
inline Value& operator*= (Value &v, double d) { v = Value(i32(i32(v) * d)); return v; }
inline Value  operator/  (Value  v, i32    i) { return Value(i32(v) / i); }
inline Value& operator/= (Value &v, i32    i) { v = Value(i32(v) / i); return v; }
inline i32    operator/  (Value v1, Value v2) { return i32(v1)/i32(v2); }

inline Value mg_value (u32 s)
{
    union ScoreSplit
    {
        u32 s;
        i16 v[2];
    };
    return Value(ScoreSplit{ s }.v[0]);
}
inline Value eg_value (u32 s)
{
    union ScoreSplit
    {
        u32 s;
        i16 v[2];
    };
    return Value(ScoreSplit{ s }.v[1]);
}

inline Score mk_score (i32 mg, i32 eg)
{
    union ValueUnion
    {
        i16 v[2];
        u32 s;
    };
    return Score(ValueUnion{ { i16(mg), i16(eg) } }.s);
}

ARTHMAT_OPERATORS(Score)
// Multiplication & Division of a Score must be handled separately for each term
inline Score  operator*  (Score  s, double d) { return mk_score (mg_value (s) * d, eg_value (s) * d); }
inline Score& operator*= (Score &s, double d) { s = mk_score (mg_value (s) * d, eg_value (s) * d); return s; }
inline Score  operator/  (Score  s, i32    i) { return mk_score (mg_value (s) / i, eg_value (s) / i); }
inline Score& operator/= (Score &s, i32    i) { s = mk_score (mg_value (s) / i, eg_value (s) / i); return s; }

#undef INC_DEC_OPERATORS
#undef ARTHMAT_OPERATORS
#undef BASIC_OPERATORS

inline Color operator~ (Color c) { return Color(c ^ i08(BLACK)); }

inline File operator~ (File f) { return File(f ^ i08(F_H)); }
inline File to_file   (char f) { return File(f - 'a'); }

inline Rank operator~ (Rank r) { return Rank(r ^ i08(R_8)); }
inline Rank to_rank   (char r) { return Rank(r - '1'); }

inline Square operator| (File f, Rank r) { return Square(( r << 3) | f); }
inline Square operator| (Rank r, File f) { return Square((~r << 3) | f); }
inline Square to_square (char f, char r) { return to_file (f) | to_rank (r); }

inline bool _ok    (Square s) { return (s & ~i08(SQ_H8)) == 0; }
inline File _file  (Square s) { return File(s & i08(F_H)); }
inline Rank _rank  (Square s) { return Rank(s >> 3); }
inline Color color (Square s) { return Color(((s ^ (s >> 3)) & 1) != 1); }

inline Square operator~ (Square s) { return Square(s ^ i08(SQ_A8)); }
inline Square operator! (Square s) { return Square(s ^ i08(SQ_H1)); }

inline Rank rel_rank (Color c, Rank   r) { return   Rank(r ^ (c*i08(R_8))); }
inline Rank rel_rank (Color c, Square s) { return rel_rank (c, _rank (s)); }
inline Square rel_sq (Color c, Square s) { return Square(s ^ (c*i08(SQ_A8))); }

inline bool opposite_colors (Square s1, Square s2)
{
    i08 s = i08(s1) ^ i08(s2);
    return ((s ^ (s >> 3)) & 1) == 1;
}

inline Delta pawn_push (Color c)
{
    return Delta(i08(DEL_N) - i08(DEL_NN)*c);
}

inline CastleRight mk_castle_right (Color c)                { return CastleRight(CR_WHITE << (2*c)); }
inline CastleRight mk_castle_right (Color c, CastleSide cs) { return CastleRight(CR_WKING << (2*c + 1*cs)); }

template<Color C, CastleSide CS>
struct Castling
{
    static const CastleRight Right = CastleRight(CR_WKING << (2*C + 1*CS));
};

inline Piece operator| (Color c, PieceType pt) { return Piece(8*c | pt); }

inline bool _ok (Piece p)
{
    return (W_PAWN <= p && p <= W_KING)
        || (B_PAWN <= p && p <= B_KING);
}
inline PieceType ptype (Piece p) { return PieceType(p & MAX_PTYPE); }
inline Color     color (Piece p) { return Color(p >> 3); }
inline Piece operator~ (Piece p) { return Piece(p ^ 8); }

inline Square    org_sq  (Move m) { return Square((m >> 6) & i08(SQ_H8)); }
inline Square    dst_sq  (Move m) { return Square((m >> 0) & i08(SQ_H8)); }
inline PieceType promote (Move m) { return PieceType(((m >> 12) & 3) + 1); }
inline MoveType  mtype   (Move m) { return MoveType(PROMOTE & m); }
inline Square fix_dst_sq (Move m, bool chess960 = false)
{
    return !chess960
        && mtype (m) == CASTLE ?
            (dst_sq (m) > org_sq (m) ? F_G : F_C) | _rank (dst_sq (m)) :
            dst_sq (m);
}
inline void promote (Move &m, PieceType pt) { m &= 0x0FFF; m |= (pt - 1) << 12 | PROMOTE; }

template<MoveType MT=NORMAL>
inline Move mk_move (Square org, Square dst, PieceType pt=NIHT) { return Move(dst | (org | ((pt - 1) << 6)) << 6 | MT); }

inline double value_to_cp (Value   v) { return double(v)/i32(VALUE_EG_PAWN); }
inline Value  cp_to_value (double cp) { return Value(i32(std::round (cp*i32(VALUE_EG_PAWN)))); }

inline Value mates_in (i32 ply) { return +VALUE_MATE - ply; }
inline Value mated_in (i32 ply) { return -VALUE_MATE + ply; }

typedef std::vector<Square> SquareVector;
typedef std::vector<Move> MoveVector;

typedef std::chrono::milliseconds::rep TimePoint; // Time in milliseconds

const u32 MilliSec       = 1000;
const u32 MinuteMilliSec = 60*MilliSec;
const u32 HourMilliSec   = 60*MinuteMilliSec;

inline TimePoint now ()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>
               (std::chrono::steady_clock::now ().time_since_epoch ()).count ();
}

struct ValMove
{
public:
    Move  move  = MOVE_NONE;
    Value value = VALUE_ZERO;

    operator Move () const { return move; }
    void operator= (Move m) { move = m; }

    // Ascending sort
    bool operator<  (const ValMove &vm) const { return value <  vm.value; }
    bool operator>  (const ValMove &vm) const { return value >  vm.value; }
    bool operator<= (const ValMove &vm) const { return value <= vm.value; }
    bool operator>= (const ValMove &vm) const { return value >= vm.value; }
    bool operator== (const ValMove &vm) const { return value == vm.value; }
    bool operator!= (const ValMove &vm) const { return value != vm.value; }
};

const Value PieceValues[PH_NO][MAX_PTYPE] =
{
    { VALUE_MG_PAWN, VALUE_MG_NIHT, VALUE_MG_BSHP, VALUE_MG_ROOK, VALUE_MG_QUEN, VALUE_ZERO, VALUE_ZERO },
    { VALUE_EG_PAWN, VALUE_EG_NIHT, VALUE_EG_BSHP, VALUE_EG_ROOK, VALUE_EG_QUEN, VALUE_ZERO, VALUE_ZERO }
};

template<class Entry, u32 Size>
struct HashTable
{
private:
    Entry _table[Size];
public:
    Entry* operator[] (Key k)
    {
        return &_table[u32(k) & (Size - 1)];
    }

    void clear ()
    {
        std::memset (&_table, 0x00, sizeof (_table));
    }
};

template<class T>
i32 sign (T val)
{
    return (T(0) < val) - (val < T(0));
}

const std::string Empty = "<empty>";

inline bool white_spaces (const std::string &str)
{
    return str.empty () || str.find_first_not_of (" \t\n") == std::string::npos;
}

inline void to_lower (std::string &str)
{
    std::transform (str.begin (), str.end (), str.begin (), ::tolower);
}
inline void to_upper (std::string &str)
{
    std::transform (str.begin (), str.end (), str.begin (), ::toupper);
}
inline void toggle (std::string &str)
{
    std::transform (str.begin (), str.end (), str.begin (),
        [](char c) { return char (islower (c) ? ::toupper (c) : ::tolower (c)); });
}

inline std::string& trim_beg (std::string &str)
{
    str.erase (str.begin (), 
                std::find_if (str.begin (), str.end (), 
                    //[](char c) { return !std::isspace (c, std::locale ()); }
                    std::not1 (std::ptr_fun<i32, i32> (std::isspace))));
    return str;
}
inline std::string& trim_end (std::string &str)
{
    str.erase (std::find_if (str.rbegin (), str.rend (), 
                //[](char c) { return !std::isspace (c, std::locale ()); }).base (), 
                std::not1 (std::ptr_fun<i32, i32> (std::isspace))).base (),
                    str.end ());
    return str;
}
inline std::string& trim (std::string &str)
{
    /*
    size_t beg = str.find_first_not_of (" \t\n");
    size_t end = str.find_last_not_of (" \t\n");
    beg  = beg == std::string::npos ?  0 : beg;
    end  = end == std::string::npos ? beg : end - beg + 1;
    str = str.substr (beg, end);
    return str;
    */
    return trim_beg (trim_end (str));
}

inline std::vector<std::string> split (const std::string str, char delimiter = ' ', bool keep_empty = true, bool do_trim = false)
{
    std::vector<std::string> tokens;
    
    /*
    size_t beg = 0;
    do
    {
        // Find next non-delimiter.
        size_t end = str.find_first_of (delimiter, beg);
        std::string token = str.substr (beg, end != std::string::npos ? end - beg : std::string::npos);
        if (do_trim)
        {
            token = trim (token);
        }
        if (!token.empty () || keep_empty)
        {
            tokens.push_back (token);
        }
        // Skip delimiter.
        beg = str.find_first_not_of (delimiter, end);
    }
    while (beg != std::string::npos);
    */
    /*
    size_t beg = 0;
    do
    {
        size_t end = str.find (delimiter, beg);
        std::string token = str.substr (beg, end != std::string::npos ? end - beg : std::string::npos);
        if (do_trim)
        {
            token = trim (token);
        }
        if (!token.empty () || keep_empty)
        {
            tokens.push_back (token);
        }
        if (end == std::string::npos)
        {
            break;
        }
        beg = end + 1;
    }
    while (true);
    */
    /*
    size_t end = 0;
    while (end <= str.length ())
    {
        size_t beg = keep_empty ? end : str.find_first_not_of (delimiter, end);
        if (beg == std::string::npos)
        {
            break;
        }
        end = str.find_first_of (delimiter, beg);
        auto token = str.substr (beg, end != std::string::npos ? end - beg : std::string::npos);
        if (do_trim)
        {
            token = trim (token);
        }
        if (!token.empty () || keep_empty)
        {
            tokens.emplace_back (token);
        }
        if (std::string::npos == end)
        {
            break;
        }
        ++end;
    }
    */

    std::istringstream iss (str);
    do
    {
        std::string token;
        bool fail = !std::getline (iss, token, delimiter);
        if (do_trim)
        {
            token = trim (token);
        }
        if (   keep_empty
            || !token.empty ())
        {
            tokens.push_back (token);
        }
        if (fail)
        {
            break;
        }
    }
    while (iss.good ());

    return tokens;
}

inline void remove_substring (std::string &str, const std::string &sub)
{
    size_t pos = str.find (sub);
    while (pos != std::string::npos)
    {
        str.erase (pos, sub.length ());
        pos = str.find (sub, pos);
    }
}

inline void remove_extension (std::string &filename)
{
    size_t pos = filename.find_last_of ('.');
    if (pos != std::string::npos)
    {
        filename = filename.substr (0, pos);
    }
}

inline std::string append_path (const std::string &base_path, const std::string &file_path)
{
    static const char Separator = '/';
    return base_path[base_path.length ()] != Separator ?
        base_path + Separator + file_path :
        base_path + file_path;
}

inline void convert_path (std::string &path)
{
    std::replace (path.begin (), path.end (), '\\', '/'); // Replace all '\' to '/'
}

//#include <unordered_set>
//inline std::string remove_dup (const std::string &str)
//{
//    // unique char set
//    std::unordered_set<char> chars_set (str.begin (), str.end ());
//    std::string unique_str (chars_set.begin (), chars_set.end ());
//    return unique_str;
//}

//template<class Iterator>
//inline auto slide (Iterator beg, Iterator end, Iterator pos) -> std::pair<Iterator, Iterator>
//{
//    if (pos < beg) return { pos, std::rotate (pos, beg, end) };
//    if (end < pos) return { std::rotate (beg, end, pos), pos };
//    return { beg, end };
//}

#endif // _TYPE_H_INC_
