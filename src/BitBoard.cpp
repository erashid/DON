#include "BitBoard.h"

#include "PRNG.h"
#include "Notation.h"

namespace BitBoard {

    using namespace std;

    u08      SquareDist[+Square::NO][+Square::NO];

    Bitboard FrontLine_bb[+Color::NO][+Square::NO];

    Bitboard Between_bb[+Square::NO][+Square::NO];
    Bitboard StrLine_bb[+Square::NO][+Square::NO];

    Bitboard DistRings_bb[+Square::NO][8];

    Bitboard PawnAttackSpan[+Color::NO][+Square::NO];
    Bitboard PawnPassSpan[+Color::NO][+Square::NO];

    Bitboard PawnAttacks[+Color::NO][+Square::NO];
    Bitboard PieceAttacks[+PieceType::NONE][+Square::NO];

    Magic BMagics[+Square::NO]
        , RMagics[+Square::NO];

#if !defined(ABM)
    u08 PopCount16[1 << 16];
#endif

    namespace {

        const Delta PawnDeltas[+Color::NO][3] =
        {
            { Delta::NORTHWEST, Delta::NORTHEAST, Delta::NONE },
            { Delta::SOUTHEAST, Delta::SOUTHWEST, Delta::NONE },
        };
        const Delta PieceDeltas[+PieceType::NONE][9] =
        {
            { Delta::NONE },
            { Delta::S2W, Delta::S2E, Delta::W2S, Delta::E2S, Delta::W2N, Delta::E2N, Delta::N2W, Delta::N2E, Delta::NONE },
            { Delta::SOUTHWEST, Delta::SOUTHEAST, Delta::NORTHWEST, Delta::NORTHEAST, Delta::NONE },
            { Delta::SOUTH, Delta::WEST, Delta::EAST, Delta::NORTH, Delta::NONE },
            { Delta::SOUTHWEST, Delta::SOUTH, Delta::SOUTHEAST, Delta::WEST, Delta::EAST, Delta::NORTHWEST, Delta::NORTH, Delta::NORTHEAST, Delta::NONE },
            { Delta::SOUTHWEST, Delta::SOUTH, Delta::SOUTHEAST, Delta::WEST, Delta::EAST, Delta::NORTHWEST, Delta::NORTH, Delta::NORTHEAST, Delta::NONE },
        };

//        // De Bruijn sequences. See chessprogramming.wikispaces.com/BitScan
//#   if defined(BIT64)
//        const u64 DeBruijn_64 = U64(0x3F79D71B4CB0A89);
//#   else
//        const u32 DeBruijn_32 = U32(0x783A9B23);
//#   endif
//
//        Square BSF_Table[+Square::NO];
//        unsigned bsf_index (Bitboard bb)
//        {
//            assert(0 != bb);
//            bb ^= (bb - 1);
//            return
//#       if defined(BIT64)
//            // Use Kim Walisch extending trick for 64-bit
//            (bb * DeBruijn_64) >> 58;
//#       else
//            // Use Matt Taylor's folding trick for 32-bit
//            (u32 ((bb >> 0) ^ (bb >> 32)) * DeBruijn_32) >> 26;
//#       endif
//        }
//
//        u08 MSB_Table[(1 << 8)];

    #if !defined(ABM)
        // Counts the non-zero bits using SWAR-Popcount algorithm
        u08 pop_count16 (u32 u)
        {
            u -= (u >> 1) & 0x5555U;
            u = ((u >> 2) & 0x3333U) + (u & 0x3333U);
            u = ((u >> 4) + u) & 0x0F0FU;
            return u08((u * 0x0101U) >> 8);
        }
    #endif

        // Max Bishop Table Size
        // 4 * 2^9 + 4 * 2^6 + 12 * 2^7 + 44 * 2^5
        // 4 * 512 + 4 *  64 + 12 * 128 + 44 *  32 = 4 * 0x200 + 4 * 0x40 + 12 * 0x80 + 32 * 0x20
        //    2048 +     256 +     1536 +     1408 =     0x800 +    0x100 +     0x600 +     0x580
        //                                    5248 =                                       0x1480
        const u32 MaxBTSize = U32(0x1480);
        Bitboard BTable[MaxBTSize];

        // Max Rook Table Size
        // 4 * 2^12 + 24 * 2^11 + 36 * 2^10
        // 4 * 4096 + 24 * 2048 + 36 * 1024 = 4 * 0x1000 + 24 * 0x800 + 36 * 0x400
        //    16384 +     49152 +     36864 =     0x4000 +     0xC000 +     0x9000
        //                           102400 =                              0x19000
        const u32 MaxRTSize = U32(0x19000);
        Bitboard RTable[MaxRTSize];

        /// Initialize all bishop and rook attacks at startup.
        /// Magic bitboards are used to look up attacks of sliding pieces.
        /// As a reference see chessprogramming.wikispaces.com/Magic+Bitboards.
        /// In particular, here we use the so called "fancy" approach.
        void initialize_table (Bitboard *const table, Magic *const magics, const Delta *const deltas)
        {

#       if !defined(BM2)
            const i16 MaxIndex = 0x1000;
            Bitboard occupancy[MaxIndex]
                ,    reference[MaxIndex];

            const u32 Seeds[+Rank::NO] =
#           if defined(BIT64)
                { 0x002D8, 0x0284C, 0x0D6E5, 0x08023, 0x02FF9, 0x03AFC, 0x04105, 0x000FF };
#           else
                { 0x02311, 0x0AE10, 0x0D447, 0x09856, 0x01663, 0x173E5, 0x199D0, 0x0427C };
#           endif

#       endif

            u32 offset = 0;
            for (auto s : SQ)
            {
                auto &magic = magics[+s];

                // attacks_bb[+s] is a pointer to the beginning of the attacks table for square
                magic.attacks = &table[offset];

                // Given a square, the mask is the bitboard of sliding attacks from
                // computed on an empty board. The index must be big enough to contain
                // all the attacks for each possible subset of the mask and so is 2 power
                // the number of 1s of the mask. Hence deduce the size of the shift to
                // apply to the 64 or 32 bits word to get the index.
                magic.mask = sliding_attacks (deltas, s)
                            // Board edges are not considered in the relevant occupancies
                           & ~(((FA_bb|FH_bb) & ~file_bb (s)) | ((R1_bb|R8_bb) & ~rank_bb (s)));

#           if !defined(BM2)
                magic.shift =
#               if defined(BIT64)
                    64
#               else
                    32
#               endif
                    - u08(pop_count (magic.mask));
#           endif

                // Use Carry-Rippler trick to enumerate all subsets of masks_bb[+s] and
                // store the corresponding sliding attack bitboard in reference[].
                // Have individual table sizes for each square with "Fancy Magic Bitboards".
                u32 size = 0;
                Bitboard occ = 0;
                do
                {
#               if defined(BM2)
                    magic.attacks[PEXT(occ, magic.mask)] = sliding_attacks (deltas, s, occ);
#               else
                    occupancy[size] = occ;
                    reference[size] = sliding_attacks (deltas, s, occ);
#               endif

                    ++size;
                    occ = (occ - magic.mask) & magic.mask;
                }
                while (0 != occ);

#           if !defined(BM2)
                
                PRNG rng (Seeds[+_rank (s)]);
                
                u32 i = 0;
                // Find a magic for square picking up an (almost) random number
                // until found the one that passes the verification test.
                while (i < size)
                {
                    magic.number = 0;
                    while (pop_count ((magic.mask * magic.number) >> 0x38) < 6)
                    {
                        magic.number = rng.sparse_rand<Bitboard> ();
                    }

                    // A good magic must map every possible occupancy to an index that
                    // looks up the correct sliding attack in the attacks_bb[+s] database.
                    // Note that build up the database for square as a side
                    // effect of verifying the magic.
                    bool used[MaxIndex] = {false};
                    for (i = 0; i < size; ++i)
                    {
                        u16 idx = magic.index (occupancy[i]);
                        assert(idx < size);
                        if (used[idx])
                        {
                            if (magic.attacks[idx] != reference[i])
                            {
                                break;
                            }
                            continue;
                        }
                        used[idx] = true;
                        magic.attacks[idx] = reference[i];
                    }
                }
#           endif
                // Set the offset of the table for the next square.
                offset += size;
            }
        }

    }

    void initialize ()
    {
        assert((Color_bb[+Color::WHITE] & Color_bb[+Color::BLACK]) == 0
            && (Color_bb[+Color::WHITE] | Color_bb[+Color::BLACK]) == (Color_bb[+Color::WHITE] ^ Color_bb[+Color::BLACK]));

        //for (auto s : SQ)
        //{
        //    BSF_Table[bsf_index (Square_bb[+s] = 1ULL << s)] = s;
        //    BSF_Table[bsf_index (Square_bb[+s])] = s;
        //}
        //for (u32 b = 2; b < (1 << 8); ++b)
        //{
        //    MSB_Table[b] =  MSB_Table[b - 1] + !more_than_one (b);
        //}

    #if !defined(ABM)
        for (u32 i = 0; i < (1 << 16); ++i)
        {
            PopCount16[i] = pop_count16 (i);
        }
    #endif

        for (auto s1 : SQ)
        {
            for (auto s2 : SQ)
            {
                if (s1 != s2)
                {
                    SquareDist[+s1][+s2] = u08(std::max (dist<File> (s1, s2), dist<Rank> (s1, s2)));
                    DistRings_bb[+s1][SquareDist[+s1][+s2] - 1] |= s2;
                }
            }
        }

        for (auto c : { Color::WHITE, Color::BLACK })
        {
            for (auto s : SQ)
            {
                FrontLine_bb  [+c][+s] = FrontRank_bb[+c][+_rank (s)] &    File_bb[+_file (s)];
                PawnAttackSpan[+c][+s] = FrontRank_bb[+c][+_rank (s)] & AdjFile_bb[+_file (s)];
                PawnPassSpan  [+c][+s] = FrontLine_bb[+c][+s] | PawnAttackSpan[+c][+s];
            }
        }

        for (auto s : SQ)
        {
            u08 k;
            Delta del;

            for (auto c : { Color::WHITE, Color::BLACK })
            {
                k = 0;
                while (Delta::NONE != (del = PawnDeltas[+c][k++]))
                {
                    auto sq = s + del;
                    if (   _ok (sq)
                        && dist (s, sq) == 1)
                    {
                        PawnAttacks[+c][+s] |= sq;
                    }
                }
            }

            PieceType pt;

            pt = PieceType::NIHT;
            k = 0;
            while (Delta::NONE != (del = PieceDeltas[+pt][k++]))
            {
                auto sq = s + del;
                if (   _ok (sq)
                    && dist (s, sq) == 2)
                {
                    PieceAttacks[+pt][+s] |= sq;
                }
            }

            pt = PieceType::KING;
            k = 0;
            while (Delta::NONE != (del = PieceDeltas[+pt][k++]))
            {
                auto sq = s + del;
                if (   _ok (sq)
                    && dist (s, sq) == 1)
                {
                    PieceAttacks[+pt][+s] |= sq;
                }
            }

            PieceAttacks[+PieceType::BSHP][+s] = sliding_attacks (PieceDeltas[+PieceType::BSHP], s);
            PieceAttacks[+PieceType::ROOK][+s] = sliding_attacks (PieceDeltas[+PieceType::ROOK], s);
            PieceAttacks[+PieceType::QUEN][+s] = PieceAttacks[+PieceType::BSHP][+s]
                                               | PieceAttacks[+PieceType::ROOK][+s];
        }

        // Initialize Sliding
        initialize_table (BTable, BMagics, PieceDeltas[+PieceType::BSHP]);
        initialize_table (RTable, RMagics, PieceDeltas[+PieceType::ROOK]);

        // NOTE:: must be after Initialize Sliding
        for (auto s1 : SQ)
        {
            for (auto s2 : SQ)
            {
                for (auto pt : { PieceType::BSHP, PieceType::ROOK })
                {
                    if (contains (PieceAttacks[+pt][+s1], s2))
                    {
                        Between_bb[+s1][+s2] = (PieceType::BSHP == pt ?
                                                    attacks_bb<PieceType::BSHP> (s1, Square_bb[+s2]) :
                                                    attacks_bb<PieceType::ROOK> (s1, Square_bb[+s2]))
                                             & (PieceType::BSHP == pt ?
                                                    attacks_bb<PieceType::BSHP> (s2, Square_bb[+s1]) :
                                                    attacks_bb<PieceType::ROOK> (s2, Square_bb[+s1]));

                        StrLine_bb[+s1][+s2] = (PieceAttacks[+pt][+s1] & PieceAttacks[+pt][+s2]) | s1 | s2;
                    }
                }
            }
        }

    }

#if !defined(NDEBUG)

    /// Returns an ASCII representation of a bitboard to print on console output
    /// Bitboard in an easily readable format. This is sometimes useful for debugging.
    string pretty (Bitboard bb)
    {
        ostringstream oss;
        oss << " /---------------\\\n";
        for (auto r : { Rank::r8, Rank::r7, Rank::r6, Rank::r5, Rank::r4, Rank::r3, Rank::r2, Rank::r1 })
        {
            oss << to_char (r) << "|";
            for (auto f : { File::fA, File::fB, File::fC, File::fD, File::fE, File::fF, File::fG, File::fH })
            {
                oss << (contains (bb, f|r) ? "+" : "-");
                if (f < File::fH)
                {
                    oss << " ";
                }
            }
            oss << "|\n";
        }
        oss << " \\---------------/\n ";
        for (auto f : { File::fA, File::fB, File::fC, File::fD, File::fE, File::fF, File::fG, File::fH })
        {
            oss << " " << to_char (f, false);
        }
        oss << "\n";
        return oss.str ();
    }

#endif

}
