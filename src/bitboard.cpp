#include "bitboard.h"

#include <memory>
#include <sstream>
#include <vector>

#include "notation.h"
#include "helper/prng.h"

uint8_t Distance[SQUARES][SQUARES];

Bitboard LineBB[SQUARES][SQUARES];
Bitboard BetweenBB[SQUARES][SQUARES];

Bitboard PawnAttacksBB[COLORS][SQUARES];
Bitboard PieceAttacksBB[PIECE_TYPES][SQUARES];

Magic BMagics[SQUARES];
Magic RMagics[SQUARES];

#if !defined(USE_POPCNT)
#include <bitset>

uint8_t PopCount[USHRT_MAX+1];

//// Counts the non-zero bits using SWAR-Popcount algorithm
//uint8_t popCount16(uint16_t u) {
//    u -= (u >> 1) & 0x5555U;
//    u = ((u >> 2) & 0x3333U) + (u & 0x3333U);
//    u = ((u >> 4) + u) & 0x0F0FU;
//    return( (u * 0x0101U) >> 8 );
//}
#endif

namespace {

    /// safeDestiny() returns the bitboard of target square for the given step
    /// from the given square. If the step is off the board, returns empty bitboard.
    Bitboard safeDestiny(Square s, Direction dir, int32_t d = 1) noexcept {
        Square const dst{ s + dir };
        return isOk(dst)
            && distance(s, dst) <= d ?
                squareBB(dst) : 0;
    }

    Bitboard slideAttacks(Square s,  Bitboard occ, Direction const directions[]) noexcept {
        Bitboard attacks{ 0 };
        for (int8_t i = 0; i < 4; ++i) {
            auto const dir{ directions[i] };
            Square sq = s;
            while (safeDestiny(sq, dir) != 0
                && !contains(occ, sq)) {
                attacks |= (sq += dir);
            }
        }
        return attacks;
    }

    template<PieceType>
    Bitboard slideAttacks(Square, Bitboard = 0) noexcept;

    Direction const BDirections[4]{ SOUTH_WEST, SOUTH_EAST, NORTH_WEST, NORTH_EAST };
    template<> Bitboard slideAttacks<BSHP>(Square s, Bitboard occ) noexcept {
        return slideAttacks(s, occ, BDirections);
    }

    Direction const RDirections[4]{ SOUTH, WEST, EAST, NORTH };
    template<> Bitboard slideAttacks<ROOK>(Square s, Bitboard occ) noexcept {
        return slideAttacks(s, occ, RDirections);
    }

    // Max Bishop Table Size
    // 4 * 2^6 + 12 * 2^7 + 44 * 2^5 + 4 * 2^9
    // 4 *  64 + 12 * 128 + 44 *  32 + 4 * 512
    //     256 +     1536 +     1408 +    2048 = 5248
    Bitboard BAttacks[0x1480];

    // Max Rook Table Size
    // 4 * 2^12 + 24 * 2^11 + 36 * 2^10
    // 4 * 4096 + 24 * 2048 + 36 * 1024
    //    16384 +     49152 +     36864 = 102400
    Bitboard RAttacks[0x19000];

    /// Initialize all bishop and rook attacks at startup.
    /// Magic bitboards are used to look up attacks of sliding pieces.
    /// In particular, here we use the so called "fancy" approach.
    template<PieceType PT>
    void initializeMagic(Bitboard attacks[], Magic magics[]) noexcept {

#if !defined(USE_BMI2)
        constexpr uint16_t MaxIndex{ 0x1000 };
        Bitboard occupancy[MaxIndex];
        Bitboard reference[MaxIndex];

        constexpr uint32_t Seeds[RANKS]{
    #if defined(IS_64BIT)
        0x002D8, 0x0284C, 0x0D6E5, 0x08023, 0x02FF9, 0x03AFC, 0x04105, 0x000FF
    #else
        0x02311, 0x0AE10, 0x0D447, 0x09856, 0x01663, 0x173E5, 0x199D0, 0x0427C
    #endif
        };
#endif

        uint16_t size{ 0 };
        //uint16_t cnt{ 0 };
        for (Square s = SQ_A1; s <= SQ_H8; ++s) {

            Magic &magic{ magics[s] };

            // Board edges are not considered in the relevant occupancies
            Bitboard const edge{ ((fileBB(FILE_A)|fileBB(FILE_H)) & ~fileBB(s))
                               | ((rankBB(RANK_1)|rankBB(RANK_8)) & ~rankBB(s)) };

            // Given a square, the mask is the bitboard of sliding attacks from
            // computed on an empty board. The index must be big enough to contain
            // all the attacks for each possible subset of the mask and so is 2 power
            // the number of 1s of the mask. Hence deduce the size of the shift to
            // apply to the 64 or 32 bits word to get the index.
            magic.mask = slideAttacks<PT>(s, 0) & ~edge;
            assert(popCount(magic.mask) <= 3*PT);

            // magics[s].attacks is a pointer to the beginning of the attacks table for the square
            // Set the offset for the attacks table of the square.
            // For each square got individual table sizes with "Fancy Magic Bitboards".
            // new Bitboard[1 << popCount(magic.mask)];
            magic.attacks = (s == SQ_A1) ? attacks : magics[s - 1].attacks + size;

#if !defined(USE_BMI2)
    #if defined(IS_64BIT)
            magic.shift = 64 - popCount(magic.mask);
    #else
            magic.shift = 32 - popCount(magic.mask);
    #endif
#endif

            size = 0;
            // Use Carry-Rippler trick to enumerate all subsets of magic.mask
            // Store the corresponding slide attack bitboard in reference[].
            Bitboard occ{ 0 };
            do {

#if !defined(USE_BMI2)
                occupancy[size] = occ;
                reference[size] = slideAttacks<PT>(s, occ);
#else
                magic.attacks[PEXT(occ, magic.mask)] = slideAttacks<PT>(s, occ);
#endif
                ++size;
                occ = (occ - magic.mask) & magic.mask;
            } while (occ != 0);

            assert(size == (1 << popCount(magic.mask)));

#if !defined(USE_BMI2)
            PRNG prng(Seeds[sRank(s)]);
            // Find a magic for square picking up an (almost) random number
            // until found the one that passes the verification test.
            uint16_t i{ 0 };
            while (i < size) {

                magic.magic = 0;
                while (popCount((magic.mask * magic.magic) >> 56) < 6) {
                    magic.magic = prng.sparseRand<Bitboard>();
                }

                // A good magic must map every possible occupancy to an index that
                // looks up the correct slide attack in the magics[s].attacks database.
                // Note that build up the database for square as a side effect of verifying the magic.
                std::vector<bool> epoch(size, false);
                //++cnt;
                for (i = 0; i < size; ++i) {

                    uint16_t idx{ magic.index(occupancy[i]) };
                    assert(idx < size);

                    if (epoch[idx]) {
                        if (magic.attacks[idx] != reference[i]) {
                            break;
                        }
                    } else {
                        epoch[idx] = true;
                        magic.attacks[idx] = reference[i];
                    }
                }
            }
#endif
        }
    }
}

namespace Bitboards {

    void initialize() noexcept {

        for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1) {
            //SquareBB[s1] = Bitboard(1) << s1;
            for (Square s2 = SQ_A1; s2 <= SQ_H8; ++s2) {
                Distance[s1][s2] = uint8_t(std::max(distance<File>(s1, s2), distance<Rank>(s1, s2)));
                assert(Distance[s1][s2] >= 0
                    && Distance[s1][s2] <= 7);
            }
        }

#if !defined(USE_POPCNT)
        for (uint32_t i = 0; i <= USHRT_MAX; ++i) {
            PopCount[i] = //popCount16(i);
                          uint8_t(std::bitset<16>(i).count());
        }
#endif

        // Initialize Magic Table
        initializeMagic<BSHP>(BAttacks, BMagics);
        initializeMagic<ROOK>(RAttacks, RMagics);

        // Pawn and Pieces Attack Table
        for (Square s = SQ_A1; s <= SQ_H8; ++s) {

            PawnAttacksBB[WHITE][s] = pawnSglAttackBB<WHITE>(squareBB(s));
            PawnAttacksBB[BLACK][s] = pawnSglAttackBB<BLACK>(squareBB(s));
            assert(popCount(PawnAttacksBB[WHITE][s]) <= 2
                && popCount(PawnAttacksBB[BLACK][s]) <= 2);

            for (auto dir : { SOUTH_2 + WEST, SOUTH_2 + EAST, WEST_2 + SOUTH, EAST_2 + SOUTH,
                              WEST_2 + NORTH, EAST_2 + NORTH, NORTH_2 + WEST, NORTH_2 + EAST }) {
                PieceAttacksBB[NIHT][s] |= safeDestiny(s, dir, 2);
            }
            for (auto dir : { SOUTH_WEST, SOUTH, SOUTH_EAST, WEST,
                              EAST, NORTH_WEST, NORTH, NORTH_EAST }) {
                PieceAttacksBB[KING][s] |= safeDestiny(s, dir);
            }

            // NOTE:: must be after initialize magic Bishop & Rook Table
            PieceAttacksBB[BSHP][s] = attacksBB<BSHP>(s, 0);
            PieceAttacksBB[ROOK][s] = attacksBB<ROOK>(s, 0);
            PieceAttacksBB[QUEN][s] = PieceAttacksBB[BSHP][s]
                                    | PieceAttacksBB[ROOK][s];
        }

        for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1) {
            for (Square s2 = SQ_A1; s2 <= SQ_H8; ++s2) {
                for (PieceType const pt : { BSHP, ROOK }) {
                    if (contains(attacksBB(pt, s1), s2)) {
                        LineBB[s1][s2] = (attacksBB(pt, s1) & attacksBB(pt, s2)) | s1 | s2;
                        BetweenBB[s1][s2] = attacksBB(pt, s1, squareBB(s2)) & attacksBB(pt, s2, squareBB(s1));
                    }
                }
            }
        }

    }

#if !defined(NDEBUG)
    /// Returns an ASCII representation of a bitboard to print on console output
    /// Bitboard in an easily readable format. This is sometimes useful for debugging.
    std::string toString(Bitboard bb) noexcept {
        std::ostringstream oss;

        oss << " /---------------\\\n";
        for (Rank r = RANK_8; r >= RANK_1; --r) {
            oss << r << '|';
            for (File f = FILE_A; f <= FILE_H; ++f) {
                oss << (contains(bb, makeSquare(f, r)) ? '+' : '-');
                if (f < FILE_H) {
                    oss << ' ';
                }
            }
            oss << "|\n";
        }
        oss << " \\---------------/\n ";
        for (File f = FILE_A; f <= FILE_H; ++f) {
            oss << ' ' << toChar(f, false);
        }
        oss << '\n';

        return oss.str();
    }
#endif

}
