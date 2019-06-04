#include "Evaluator.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include "BitBoard.h"
#include "Material.h"
#include "Notation.h"
#include "Option.h"
#include "Pawns.h"
#include "Thread.h"

using namespace std;
using namespace BitBoard;

namespace {

    enum Term : u08
    {
        // The first 6 entries are for PieceType
        MATERIAL = NONE,
        IMBALANCE,
        MOBILITY,
        THREAT,
        PASSER,
        SPACE,
        INITIATIVE,
        TOTAL,
    };

    Score Scores[TOTAL + 1][CLR_NO];

    void clear ()
    {
        std::memset (Scores, SCORE_ZERO, sizeof (Scores));
    }

    void write (Term term, Color c, Score score)
    {
        Scores[term][c] = score;
    }
    void write (Term term, Score wscore, Score bscore = SCORE_ZERO)
    {
        write (term, WHITE, wscore);
        write (term, BLACK, bscore);
    }

    ostream& operator<< (ostream &os, Term term)
    {
        auto *score = Scores[term];
        switch (term)
        {
        case Term::MATERIAL:
        case Term::IMBALANCE:
        case Term::INITIATIVE:
        case Term::TOTAL:
            os << " | ----- -----"
               << " | ----- -----";
            break;
        default:
            os << " | " << score[WHITE]
               << " | " << score[BLACK];
            break;
        }
        os << " | " << score[WHITE] - score[BLACK] << std::endl;
        return os;
    }


#define S(mg, eg) mk_score (mg, eg)

    constexpr Score Mobility[4][28] =
    {
        { // Knight
            S(-62,-81), S(-53,-56), S(-12,-30), S( -4,-14), S(  3,  8), S( 13, 15),
            S( 22, 23), S( 28, 27), S( 33, 33)
        },
        { // Bishop
            S(-48,-59), S(-20,-23), S( 16, -3), S( 26, 13), S( 38, 24), S( 51, 42),
            S( 55, 54), S( 63, 57), S( 63, 65), S( 68, 73), S( 81, 78), S( 81, 86),
            S( 91, 88), S( 98, 97)
        },
        { // Rook
            S(-58,-76), S(-27,-18), S(-15, 28), S(-10, 55), S( -5, 69), S( -2, 82),
            S(  9,112), S( 16,118), S( 30,132), S( 29,142), S( 32,155), S( 38,165),
            S( 46,166), S( 48,169), S( 58,171)
        },
        { // Queen
            S(-39,-36), S(-21,-15), S(  3,  8), S(  3, 18), S( 14, 34), S( 22, 54),
            S( 28, 61), S( 41, 73), S( 43, 79), S( 48, 92), S( 56, 94), S( 60,104),
            S( 60,113), S( 66,120), S( 67,123), S( 70,126), S( 71,133), S( 73,136),
            S( 79,140), S( 88,143), S( 88,148), S( 99,166), S(102,170), S(102,175),
            S(106,184), S(109,191), S(113,206), S(116,212)
        }
    };

    constexpr Score RookOnFile[2] =
    {
        S(18, 7), S(44,20)
    };

    constexpr Score MinorThreat[NONE] =
    {
        S( 0,31), S(39,42), S(57,44), S(68,112), S(62,120), S( 0, 0)
    };
    constexpr Score MajorThreat[NONE] =
    {
        S( 0,24), S(38,71), S(38,61), S( 0,38), S(51,38), S( 0, 0)
    };

    constexpr Score PasserFile[F_NO/2] =
    {
        S(-1, 7), S( 0, 9), S(-9,-8), S(-30,-14)
    };
    constexpr Score PasserRank[R_NO] =
    {
        S( 0, 0), S( 5,18), S(12,23), S(10,31), S(57,62), S(163,167), S(271,250), S( 0, 0)
    };

    constexpr Score MinorBehindPawn =   S( 18,  3);
    constexpr Score Outpost =           S(  9,  3);
    constexpr Score MinorKingProtect =  S(  7,  8);
    constexpr Score BishopOnDiagonal =  S( 45,  0);
    constexpr Score BishopPawns =       S(  3,  7);
    constexpr Score BishopTrapped =     S( 50, 50);
    constexpr Score RookOnPawns =       S( 10, 32);
    constexpr Score RookTrapped =       S( 47,  4);
    constexpr Score QueenWeaken =       S( 49, 15);
    constexpr Score PawnLessFlank =     S( 17, 95);
    constexpr Score KingTropism =       S(  8,  0);
    constexpr Score PieceRestricted =   S(  7,  6);
    constexpr Score PawnWeakUnopposed = S( 12, 23);
    constexpr Score PieceHanged =       S( 69, 36);
    constexpr Score PawnThreat =        S(173, 94);
    constexpr Score PawnPushThreat =    S( 48, 39);
    constexpr Score RankThreat =        S( 13,  0);
    constexpr Score KingThreat =        S( 24, 89);
    constexpr Score KnightOnQueen =     S( 16, 12);
    constexpr Score SliderOnQueen =     S( 59, 18);

#undef S

    constexpr i32 SafeCheckWeight[NONE] =
    {
        30, 790, 635, 880, 980, 0
    };

    constexpr i32 KingAttackerWeight[NONE] =
    {
        0, 77, 55, 44, 10, 0
    };

    // Evaluator class contains various evaluation functions.
    template<bool Trace>
    class Evaluator
    {
    private:

        const Position &pos;

        Pawns::Entry *pe;
        Material::Entry *me;

        Bitboard mob_area[CLR_NO];
        Score    mobility[CLR_NO];

        // Contains all squares attacked by the color and piece type.
        Bitboard ful_attacks[CLR_NO];
        // Contains all squares attacked by the color and piece type with pinned removed.
        Bitboard sgl_attacks[CLR_NO][PT_NO];
        // Contains all squares attacked by more than one pieces of a color, possibly via x-ray or by one pawn and one piece.
        Bitboard dbl_attacks[CLR_NO];

        Bitboard queen_attacks[CLR_NO][3];

        // Zone around the king which is considered by the king safety evaluation.
        // The squares directly adjacent to the king, , plus (only for a king on its first rank) the squares two ranks in front.
        // For instance, if black king is on g8, king_ring[BLACK] is f8, h8, f7, g7, h7, f6, g6 and h6.
        Bitboard king_ring[CLR_NO];
        // Number of pieces of the color, which attack a square in the king_ring of the enemy king.
        u08 king_attackers_count[CLR_NO];
        // Sum of the "weight" of the pieces of the color which attack a square in the king_ring of the enemy king.
        // The weights of the individual piece types are given by the KingAttackerWeight[piece-type]
        i32 king_attackers_weight[CLR_NO];
        // Number of attacks by the color to squares directly adjacent to the enemy king.
        // Pieces which attack more than one square are counted multiple times.
        // For instance, if there is a white knight on g5 and black's king is on g8, this white knight adds 2 to king_attacks_count[WHITE]
        u08 king_attacks_count[CLR_NO];

        template<Color> void initialize ();
        template<Color, PieceType> Score pieces ();
        template<Color> Score king () const;
        template<Color> Score threats () const;
        template<Color> Score passers () const;
        template<Color> Score space () const;

        Score initiative (Value) const;
        Scale scale (Value) const;

    public:
        Evaluator () = delete;
        Evaluator (const Evaluator&) = delete;
        Evaluator& operator= (const Evaluator&) = delete;

        Evaluator (const Position &p)
            : pos (p)
        {}

        Value value ();
    };

    /// initialize() computes king and pawn attacks, and the king ring bitboard of the color.
    template<bool Trace> template<Color Own>
    void Evaluator<Trace>::initialize ()
    {
        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;

        auto own_k_sq = pos.square (Own|KING);

        Bitboard own_pawns = pos.pieces (Own, PAWN);
        sgl_attacks[Own][PAWN] =  pawn_sgl_attacks_bb (Own, own_pawns & ~pos.si->king_blockers[Own])
                               | (pawn_sgl_attacks_bb (Own, own_pawns &  pos.si->king_blockers[Own]) & PieceAttacks[BSHP][own_k_sq]);
        sgl_attacks[Own][KING] = PieceAttacks[KING][own_k_sq];

        ful_attacks[Own] = sgl_attacks[Own][KING]
                         | pawn_sgl_attacks_bb (Own, own_pawns);
        sgl_attacks[Own][NONE] = sgl_attacks[Own][KING]
                               | sgl_attacks[Own][PAWN];
        dbl_attacks[Own] = sgl_attacks[Own][PAWN]
                         & (  pawn_dbl_attacks_bb (Own, own_pawns)
                            | sgl_attacks[Own][KING]);

        // Do not include in mobility area
        // - squares protected by enemy pawns
        // - square occupied by friend Queen and King
        // - squares occupied by block pawns (pawns blocked or on ranks 2-3)
        mob_area[Opp] = ~(  sgl_attacks[Own][PAWN]
                          | pos.pieces (Opp, QUEN, KING)
                          | (  pos.pieces (Opp, PAWN)
                             & (  LowRanks_bb[Opp]
                                | pawn_sgl_pushes_bb (Own, pos.pieces ()))));
        mobility[Opp] = SCORE_ZERO;

        auto opp_k_sq = pos.square (Opp|KING);
        king_ring[Opp] = PieceAttacks[KING][opp_k_sq];
        if (R_1 == rel_rank (Opp, opp_k_sq))
        {
            king_ring[Opp] |= pawn_sgl_pushes_bb (Opp, king_ring[Opp]);
        }
        if (F_H == _file (opp_k_sq))
        {
            king_ring[Opp] |= shift<DEL_W> (king_ring[Opp]);
        }
        else
        if (F_A == _file (opp_k_sq))
        {
            king_ring[Opp] |= shift<DEL_E> (king_ring[Opp]);
        }

        king_attackers_count[Own] = u08(pop_count (king_ring[Opp] & sgl_attacks[Own][PAWN]));
        king_attackers_weight[Own] = 0;
        king_attacks_count[Own] = 0;
        // Remove from king_ring[] the squares defended by two pawns
        king_ring[Opp] &= ~pawn_dbl_attacks_bb (Opp, pos.pieces (Opp, PAWN));
    }

    /// pieces() evaluates the pieces of the color and type.
    template<bool Trace> template<Color Own, PieceType PT>
    Score Evaluator<Trace>::pieces ()
    {
        static_assert (NIHT <= PT && PT <= QUEN, "PT incorrect");

        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;

        Score score = SCORE_ZERO;

        sgl_attacks[Own][PT] = 0;
        if (QUEN == PT)
        {
            std::fill_n (queen_attacks[Own], 3, 0);
        }
        for (const auto &s : pos.squares[Own|PT])
        {
            assert((Own|PT) == pos[s]);
            // Find attacked squares, including x-ray attacks for Bishops, Rooks and Queens
            Bitboard attacks = pos.xattacks_from<PT> (s, Own);

            ful_attacks[Own] |= attacks;

            if (QUEN == PT)
            {
                queen_attacks[Own][0] |= PieceAttacks[NIHT][s];
                queen_attacks[Own][1] |= PieceAttacks[BSHP][s] & attacks;
                queen_attacks[Own][2] |= PieceAttacks[ROOK][s] & attacks;
            }

            if (contains (pos.si->king_blockers[Own], s))
            {
                attacks &= line_bb (pos.square (Own|KING), s);
            }

            switch (PT)
            {
            case BSHP:
            {
                Bitboard att = attacks & pos.pieces (Own) & ~pos.si->king_blockers[Own];
                Bitboard bp = att & front_rank_bb (Own, s) & pos.pieces (PAWN);
                dbl_attacks[Own] |= sgl_attacks[Own][NONE]
                                  & (  attacks
                                     | (0 != bp ? pawn_sgl_attacks_bb (Own, bp) & PieceAttacks[BSHP][s] : 0));
            }
                break;
            case QUEN:
            {
                Bitboard att = attacks & pos.pieces (Own) & ~pos.si->king_blockers[Own];
                Bitboard qp = att & front_rank_bb (Own, s) & pos.pieces (PAWN);
                Bitboard qb = att & PieceAttacks[BSHP][s] & pos.pieces (BSHP);
                Bitboard qr = att & PieceAttacks[ROOK][s] & pos.pieces (ROOK);
                dbl_attacks[Own] |= sgl_attacks[Own][NONE]
                                  & (  attacks
                                     | (0 != qp ? pawn_sgl_attacks_bb (Own, qp) & PieceAttacks[BSHP][s] : 0)
                                     | (0 != qb ? attacks_bb<BSHP> (s, pos.pieces () ^ qb) : 0)
                                     | (0 != qr ? attacks_bb<ROOK> (s, pos.pieces () ^ qr) : 0));
            }
                break;
            default:
                dbl_attacks[Own] |= sgl_attacks[Own][NONE]
                                  & attacks;
                break;
            }

            sgl_attacks[Own][PT]   |= attacks;
            sgl_attacks[Own][NONE] |= attacks;

            if (0 != (king_ring[Opp] & attacks))
            {
                ++king_attackers_count[Own];
                king_attackers_weight[Own] += KingAttackerWeight[PT];
                king_attacks_count[Own] += u08(pop_count (sgl_attacks[Opp][KING] & attacks));
            }

            auto mob = pop_count (mob_area[Own] & attacks);
            assert(0 <= mob && mob <= 27);

            // Bonus for piece mobility
            mobility[Own] += Mobility[PT - 1][mob];

            Bitboard b;
            // Special extra evaluation for pieces
            switch (PT)
            {
            case NIHT:
            case BSHP:
            {
                // Bonus for minor behind a pawn
                if (contains (pawn_sgl_pushes_bb (Opp, pos.pieces (PAWN)), s))
                {
                    score += MinorBehindPawn;
                }

                // Penalty for distance from the friend king
                score -= MinorKingProtect * dist (s, pos.square (Own|KING));

                b = Outposts_bb[Own]
                  & ~pe->attack_span[Opp];
                // Bonus for knight outpost squares
                if (contains (b, s))
                {
                    score += Outpost * (4 * (contains (sgl_attacks[Own][PAWN], s) ? 2 : 1) / PT);
                }
                else
                if (0 != (b &= attacks & ~pos.pieces (Own)))
                {
                    score += Outpost * (2 * (0 != (sgl_attacks[Own][PAWN] & b) ? 2 : 1) / PT);
                }

                if (BSHP == PT)
                {
                    // Penalty for pawns on the same color square as the bishop,
                    // more when the center files are blocked with pawns.
                    b = pos.pieces (Own, PAWN)
                      & Side_bb[CS_NO]
                      & pawn_sgl_pushes_bb (Opp, pos.pieces ());
                    score -= BishopPawns
                           * (1 + pop_count (b))
                           * pos.color_pawn_count (Own, color (s));

                    // Bonus for bishop on a long diagonal which can "see" both center squares
                    if (more_than_one (attacks_bb<BSHP> (s, pos.pieces (PAWN)) & Center_bb))
                    {
                        score += BishopOnDiagonal;
                    }

                    if (bool(Options["UCI_Chess960"]))
                    {
                        // An important Chess960 pattern: A cornered bishop blocked by a friend pawn diagonally in front of it.
                        // It is a very serious problem, especially when that pawn is also blocked.
                        // Bishop (white or black) on a1/h1 or a8/h8 which is trapped by own pawn on b2/g2 or b7/g7.
                        if (   1 >= mob
                            && contains (FA_bb|FH_bb, s)
                            && R_1 == rel_rank (Own, s))
                        {
                            auto del = Delta((F_E - _file (s))/3) + pawn_push (Own);
                            if (contains (pos.pieces (Own, PAWN), s+del))
                            {
                                score -= BishopTrapped
                                       * (!contains (pos.pieces (), s + del + pawn_push (Own)) ?
                                              !contains (pos.pieces (Own, PAWN), s + del + del) ?
                                                  1 : 2 : 4);
                            }
                        }
                    }
                }
            }
                break;
            case ROOK:
            {
                // Bonus for rook aligning with enemy pawns on the same rank/file
                if (R_4 < rel_rank (Own, s))
                {
                    score += RookOnPawns * pop_count (pos.pieces (Opp, PAWN) & PieceAttacks[ROOK][s]);
                }

                // Bonus for rook when on an open or semi-open file
                if (pos.semiopenfile_on (Own, s))
                {
                    score += RookOnFile[pos.semiopenfile_on (Opp, s) ? 1 : 0];
                }
                else
                // Penalty for rook when trapped by the king, even more if the king can't castle
                if (   3 >= mob
                    && R_5 > rel_rank (Own, s))
                {
                    auto kf = _file (pos.square (Own|KING));
                    if ((kf < F_E) == (_file (s) < kf))
                    {
                        score -= RookTrapped * (CR_NONE != pos.si->castle_right (Own) ? 1 : 2);
                    }
                }
            }
                break;
            case QUEN:
            {
                // Penalty for pin or discover attack on the queen
                b = 0;
                if ((  pos.slider_blockers (s, Opp, pos.pieces (Opp, QUEN), b, b)
                     & ~pos.si->king_blockers[Opp]
                     & ~(  pos.pieces (Opp, PAWN)
                         & file_bb (s)
                         & ~pawn_sgl_attacks_bb (Own, pos.pieces (Own)))) != 0)
                {
                    score -= QueenWeaken;
                }
            }
                break;
            default: assert(false);
                break;
            }
        }

        if (Trace)
        {
            write (Term (PT), Own, score);
        }

        return score;
    }

    /// king() evaluates the king of the color.
    template<bool Trace> template<Color Own>
    Score Evaluator<Trace>::king () const
    {
        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;

        auto own_k_sq = pos.square (Own|KING);

        // King Safety: friend pawns shelter and enemy pawns storm
        u08 index = pe->king_safety_on<Own> (pos, own_k_sq);
        Score score = pe->king_safety[Own][index];
        if (   0 != index
            && pos.si->can_castle (Own|CS_KING)
            && pos.expeded_castle (Own, CS_KING)
            && 0 == (pos.castle_king_path_bb[Own][CS_KING] & ful_attacks[Opp])
            && mg_value (score) < mg_value (pe->king_safety[Own][0]))
        {
            score = pe->king_safety[Own][0];
        }
        if (   1 != index
            && pos.si->can_castle (Own|CS_QUEN)
            && pos.expeded_castle (Own, CS_QUEN)
            && 0 == (pos.castle_king_path_bb[Own][CS_QUEN] & ful_attacks[Opp])
            && mg_value (score) < mg_value (pe->king_safety[Own][1]))
        {
            score = pe->king_safety[Own][1];
        }

        score += mk_score (VALUE_ZERO, -16 * pe->king_pawn_dist[Own][index]);

        // Main king safety evaluation
        i32 king_danger = 0;

        // Attacked squares defended at most once by friend queen or king
        Bitboard weak_area =  sgl_attacks[Opp][NONE]
                           & ~dbl_attacks[Own]
                           & (  ~sgl_attacks[Own][NONE]
                              |  sgl_attacks[Own][QUEN]
                              |  sgl_attacks[Own][KING]);

        // Safe squares where enemy's safe checks are possible on next move
        Bitboard safe_area = ~pos.pieces (Opp)
                           & (  ~sgl_attacks[Own][NONE]
                              | (  weak_area
                                 & dbl_attacks[Opp]));

        Bitboard unsafe_check = 0;

        //// Enemy pawn checks
        //Bitboard pawn_check = PawnAttacks[Own][own_k_sq]
        //                    & sgl_attacks[Opp][PAWN];
        //// Enemy pawn safe checks
        //Bitboard pawn_safe_check = pawn_check
        //                         & safe_area;
        //if (0 != pawn_safe_check)
        //{
        //    king_danger += pop_count (pawn_safe_check) * SafeCheckWeight[PAWN];
        //}
        //else
        //{
        //    unsafe_check |= pawn_check;
        //}

        // Enemy knight safe checks
        Bitboard niht_safe_check = PieceAttacks[NIHT][own_k_sq]
                                 & sgl_attacks[Opp][NIHT]
                                 & safe_area;
        if (0 != niht_safe_check)
        {
            king_danger += pop_count (niht_safe_check) * SafeCheckWeight[NIHT];
        }
        else
        {
            unsafe_check |= PieceAttacks[NIHT][own_k_sq]
                          & sgl_attacks[Opp][NIHT];
        }

        Bitboard bshp_attack = attacks_bb<BSHP> (own_k_sq, pos.pieces () ^ pos.pieces (Own, QUEN));
        Bitboard rook_attack = attacks_bb<ROOK> (own_k_sq, pos.pieces () ^ pos.pieces (Own, QUEN));

        // Enemy queen safe checks
        Bitboard quen_safe_check = (bshp_attack | rook_attack)
                                 &  sgl_attacks[Opp][QUEN]
                                 & ~sgl_attacks[Own][QUEN]
                                 & safe_area;

        Bitboard b;

        // Enemy bishop safe checks
        Bitboard bshp_safe_check = bshp_attack
                                 & sgl_attacks[Opp][BSHP]
                                 & safe_area;
        b =  bshp_safe_check
          & ~quen_safe_check;
        if (0 != b)
        {
            king_danger += pop_count (b) * SafeCheckWeight[BSHP];
        }
        else
        {
            unsafe_check |= bshp_attack
                          & sgl_attacks[Opp][BSHP];
        }

        // Enemy rook safe checks
        Bitboard rook_safe_check = rook_attack
                                 & sgl_attacks[Opp][ROOK]
                                 & safe_area;
        b =  rook_safe_check
          & ~quen_safe_check;
        if (0 != b)
        {
            king_danger += pop_count (b) * SafeCheckWeight[ROOK];
        }
        else
        {
            unsafe_check |= rook_attack
                          & sgl_attacks[Opp][ROOK];
        }

        if (0 != quen_safe_check)
        {
            king_danger += pop_count (quen_safe_check) * SafeCheckWeight[QUEN];
        }
        //else
        //{
        //    unsafe_check |= (bshp_attack | rook_attack)
        //                  &  sgl_attacks[Opp][QUEN]
        //                  & ~sgl_attacks[Own][QUEN];
        //}

        b =  quen_safe_check
          & (bshp_safe_check | rook_safe_check);
        if (0 != b)
        {
            king_danger += pop_count (b) * 200;
        }

        // Unsafe or occupied checking squares will also be considered, as long as
        // the square is in the attacker's mobility area.
        unsafe_check &= mob_area[Opp];

        b = KingFlank_bb[_file (own_k_sq)]
          & Camp_bb[Own]
          & sgl_attacks[Opp][NONE];
        // Friend king flank attacks count
        i32 tropism = pop_count (b)                    // Squares attacked by enemy in friend king flank
                    + pop_count (b & dbl_attacks[Opp]);// Squares attacked by enemy twice in friend king flank.

        // Initialize the king danger, which will be transformed later into a score.
        // - number and types of the enemy's attacking pieces,
        // - number of attacked and undefended squares around friend king,
        // - quality of the pawn shelter ('mg score' safety).
        king_danger +=   1 * king_attackers_count[Opp]*king_attackers_weight[Opp]
                     +  69 * king_attacks_count[Opp]
                     + 185 * pop_count (king_ring[Own] & weak_area)
                     + 150 * pop_count (pos.si->king_blockers[Own] | unsafe_check)
                     +   1 * mg_value (mobility[Opp] - mobility[Own])
                     +   5 * tropism * tropism / 16
                     -   3 * mg_value (score) / 4
                     -   7;

        Bitboard king_spot = sgl_attacks[Own][KING] | own_k_sq;
        
        // If friend knight is near by to defend king
        if (0 != (king_spot & sgl_attacks[Own][NIHT]))
        {
            king_danger -= 100;
        }
        // If friend bishop is near by to defend king
        if (0 != (king_spot & sgl_attacks[Own][BSHP]))
        {
            king_danger -= 35;
        }
        // If no enemy queen
        if (0 == pos.pieces (Opp, QUEN))
        {
            king_danger -= 873;
        }

        // Transform the king danger into a score
        if (100 < king_danger)
        {
            score -= mk_score (king_danger*king_danger / 0x1000, king_danger / 0x10);
        }

        // Penalty for king on a pawnless flank
        if (0 == (pos.pieces (PAWN) & KingFlank_bb[_file (own_k_sq)]))
        {
            score -= PawnLessFlank;
        }

        // King tropism: Penalty for slow motion attacks moving towards friend king zone
        score -= KingTropism * tropism;

        if (Trace)
        {
            write (Term (KING), Own, score);
        }

        return score;
    }

    /// threats() evaluates the threats of the color.
    template<bool Trace> template<Color Own>
    Score Evaluator<Trace>::threats () const
    {
        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;

        Score score = SCORE_ZERO;

        // Enemy non-pawns
        Bitboard nonpawns_enemies =  pos.pieces (Opp)
                                  & ~pos.pieces (PAWN);
        // Squares defended by the opponent,
        // - attack the square with a pawn
        // - attack the square twice and not defended twice.
        Bitboard defended_area = sgl_attacks[Opp][PAWN]
                               | (   dbl_attacks[Opp]
                                  & ~dbl_attacks[Own]);
        // Enemy not defended and under attacked by any friend piece
        Bitboard attacked_weak_enemies =  pos.pieces (Opp)
                                       & ~defended_area
                                       &  sgl_attacks[Own][NONE];
        // Non-pawn enemies, defended by enemies
        Bitboard defended_nonpawns_enemies = nonpawns_enemies
                                           & defended_area;

        Bitboard b;

        if (   0 != attacked_weak_enemies
            || 0 != defended_nonpawns_enemies)
        {
            // Bonus according to the type of attacking pieces

            // Enemies attacked by minors
            b = (  attacked_weak_enemies
                   // Enemy defended non-pawns
                 | defended_nonpawns_enemies)
              & (  sgl_attacks[Own][NIHT]
                 | sgl_attacks[Own][BSHP]);
            while (0 != b)
            {
                auto s = pop_lsq (b);
                auto pt = ptype (pos[s]);
                score += MinorThreat[pt];
                if (PAWN != pt)
                {
                    score += RankThreat * rel_rank (Opp, s);
                }
            }

            if (0 != attacked_weak_enemies)
            {
                // Enemies attacked by majors
                b =  attacked_weak_enemies
                  &  sgl_attacks[Own][ROOK];
                while (0 != b)
                {
                    auto s = pop_lsq (b);
                    auto pt = ptype (pos[s]);
                    score += MajorThreat[pt];
                    if (PAWN != pt)
                    {
                        score += RankThreat * rel_rank (Opp, s);
                    }
                }
                // Enemies attacked by king
                b =  attacked_weak_enemies
                  &  sgl_attacks[Own][KING];
                if (0 != b)
                {
                    score += KingThreat;
                }

                // Enemies attacked are hanging
                b = attacked_weak_enemies
                  & (  ~sgl_attacks[Opp][NONE]
                     | (  nonpawns_enemies
                        & dbl_attacks[Own]));
                score += PieceHanged * pop_count (b);
            }
        }

        // Bonus for restricting their piece moves
        Bitboard restricted =  sgl_attacks[Opp][NONE]
                            & ~defended_area
                            &  sgl_attacks[Own][NONE];
        score += PieceRestricted * pop_count (restricted);

        // Bonus for opponent unopposed weak pawns
        if (0 != pos.pieces (Own, ROOK, QUEN))
        {
            score += PawnWeakUnopposed * pe->weak_unopposed_count[Opp];
        }

        Bitboard safe_area;

        safe_area =  sgl_attacks[Own][NONE]
                  | ~sgl_attacks[Opp][NONE];
        // Safe friend pawns
        b =  safe_area
          &  pos.pieces (Own, PAWN);
        // Safe friend pawns attacks on nonpawn enemies
        b =  nonpawns_enemies
          &  pawn_sgl_attacks_bb (Own, b)
          &  sgl_attacks[Own][PAWN];
        score += PawnThreat * pop_count (b);

        // Friend pawns who can push on the next move
        b =  pos.pieces (Own, PAWN)
          & ~pos.si->king_blockers[Own];
        // Friend pawns push
        b =  pawn_sgl_pushes_bb (Own, b)
          & ~pos.pieces ();
        b |= pawn_sgl_pushes_bb (Own, b & rank_bb (rel_rank (Own, R_3)))
          & ~pos.pieces ();
        // Friend pawns push safe
        b &= safe_area
          & ~sgl_attacks[Opp][PAWN];
        // Friend pawns push safe attacks an enemies
        b =  pawn_sgl_attacks_bb (Own, b)
          &  pos.pieces (Opp);
        score += PawnPushThreat * pop_count (b);

        // Bonus for threats on the next moves against enemy queens
        if (0 != pos.pieces (Opp, QUEN))
        {
            safe_area =  mob_area[Own]
                      & ~defended_area;
            b = safe_area
              & (sgl_attacks[Own][NIHT] & queen_attacks[Opp][0]);
            score += KnightOnQueen * pop_count (b);

            b = safe_area
              & (  (sgl_attacks[Own][BSHP] & queen_attacks[Opp][1])
                 | (sgl_attacks[Own][ROOK] & queen_attacks[Opp][2]))
              & dbl_attacks[Own];
            // Bonus for safe slider attack threats on enemy queen
            score += SliderOnQueen * pop_count (b);
        }

        if (Trace)
        {
            write (Term::THREAT, Own, score);
        }

        return score;
    }

    /// passers() evaluates the passed pawns of the color.
    template<bool Trace> template<Color Own>
    Score Evaluator<Trace>::passers () const
    {
        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;

        auto king_proximity = [&](Color c, Square s) { return std::min (dist (pos.square (c|KING), s), 5); };

        Score score = SCORE_ZERO;

        Bitboard psr = pe->passers[Own];
        while (0 != psr)
        {
            auto s = pop_lsq (psr);
            assert(0 == (pos.pieces (Opp, PAWN) & pawn_sgl_pushes_bb (Own, front_line_bb (Own, s))));

            i32 r = rel_rank (Own, s);
            // Base bonus depending on rank.
            Score bonus = PasserRank[r];

            auto push_sq = s + pawn_push (Own);

            if (R_3 < r)
            {
                i32 w = (r-2) * (r-2) + 2;

                // Adjust bonus based on the king's proximity
                bonus += mk_score (0, +5*w*king_proximity (Opp, push_sq)
                                      -2*w*king_proximity (Own, push_sq));
                // If block square is not the queening square then consider also a second push.
                if (R_7 != r)
                {
                    bonus += mk_score (0, -1*w*king_proximity (Own, push_sq + pawn_push (Own)));
                }

                // If the pawn is free to advance.
                if (pos.empty (push_sq))
                {
                    // Squares to queen
                    Bitboard front_line = front_line_bb (Own, s);
                    Bitboard safe_front_line = front_line
                        ,  unsafe_front_line = front_line;
                    // If there is a rook or queen attacking/defending the pawn from behind, consider front squares.
                    // Otherwise consider only the squares in the pawn's path attacked or occupied by the enemy.
                    Bitboard behind_major = front_line_bb (Opp, s)
                                          & pos.pieces (ROOK, QUEN);
                    // If there is no friend rook or queen attacking the pawn from behind,
                    // consider only the squares in the pawn's path attacked by the friend.
                    // Otherwise add all X-ray attacks by the friend rook or queen.
                    if ((  behind_major
                         & pos.pieces (Own)
                         & ~pos.si->king_blockers[Own]
                         /*& attacks_bb<ROOK> (s, pos.pieces (Own))*/) == 0)
                    {
                        safe_front_line &= sgl_attacks[Own][NONE];
                    }
                    // If there is no enemy rook or queen attacking the pawn from behind,
                    // consider only the squares in the pawn's path attacked or occupied by the enemy,
                    // Otherwise add all X-ray attacks by the enemy rook or queen.
                    if ((  behind_major
                         & pos.pieces (Opp)
                         & ~pos.si->king_blockers[Opp]
                         /*& attacks_bb<ROOK> (s, pos.pieces (Opp))*/) == 0)
                    {
                        unsafe_front_line &= sgl_attacks[Opp][NONE] | pos.pieces (Opp);
                    }

                    i32 k;
                    // Give a big bonus if the path to the queen is not attacked,
                    // a smaller bonus if the block square is not attacked.
                    k = 0 != unsafe_front_line ?
                             contains (unsafe_front_line, push_sq) ?
                                0 : 9 : 20;
                    // Give a big bonus if the path to the queen is fully defended,
                    // a smaller bonus if the block square is defended.
                    k += safe_front_line != front_line ?
                            !contains (safe_front_line, push_sq) ?
                                0 : 4 : 6;

                    bonus += mk_score (k*w, k*w);
                }
            }

            // Scale down bonus for candidate passers which need more than one 
            // pawn push to become passed or have a pawn in front of them.
            if (   !pos.pawn_passed_at (Own, push_sq)
                || 0 != (pos.pieces (PAWN) & front_line_bb (Own, s)))
            {
                bonus /= 2;
            }

            score += bonus + PasserFile[std::min (_file (s), ~_file (s))];
        }

        if (Trace)
        {
            write (Term::PASSER, Own, score);
        }

        return score;
    }

    /// space() evaluates the space of the color.
    /// The space evaluation is a simple bonus based on the number of safe squares
    /// available for minor pieces on the central four files on ranks 2-4
    /// Safe squares one, two or three squares behind a friend pawn are counted twice
    /// The aim is to improve play on opening
    template<bool Trace> template<Color Own>
    Score Evaluator<Trace>::space () const
    {
        constexpr auto Opp = WHITE == Own ? BLACK : WHITE;

        if (pos.non_pawn_material () < Value(12222)) // Space Threshold
        {
            return SCORE_ZERO;
        }

        // Find all squares which are at most three squares behind some friend pawn
        Bitboard behind = pos.pieces (Own, PAWN);
        behind |= pawn_sgl_pushes_bb (Opp, behind);
        behind |= pawn_dbl_pushes_bb (Opp, behind);

        // Safe squares for friend pieces inside the area defined by SpaceMask.
        Bitboard safe_space = Region_bb[Own]
                            & Side_bb[CS_NO]
                            & ~pos.pieces (Own, PAWN)
                            & ~sgl_attacks[Opp][PAWN];

        i32 bonus = pop_count (safe_space) + pop_count (behind & safe_space);
        i32 weight = pos.count (Own) - 1;
        Score score = mk_score (bonus * weight * weight / 16, 0);

        if (Trace)
        {
            write (Term::SPACE, Own, score);
        }

        return score;
    }

    /// initiative() evaluates the initiative correction value for the position
    /// i.e. second order bonus/malus based on the known attacking/defending status of the players
    template<bool Trace>
    Score Evaluator<Trace>::initiative (Value eg) const
    {
        // Compute the initiative bonus for the attacking side
        i32 complexity =   9 * pe->passed_count ()
                       +  11 * pos.count (PAWN)
                        // Outflanking
                       +   9 * (  dist<File> (pos.square (WHITE|KING), pos.square (BLACK|KING))
                                - dist<Rank> (pos.square (WHITE|KING), pos.square (BLACK|KING)))
                       - 103;
        // Pawn on both flanks
        if (   0 != (pos.pieces (PAWN) & Side_bb[CS_KING])
            && 0 != (pos.pieces (PAWN) & Side_bb[CS_QUEN]))
        {
            complexity += 18;
        }
        if (VALUE_ZERO == pos.non_pawn_material ())
        {
            complexity += 49;
        }

        Score score = mk_score (0, sign (eg) * std::max (complexity, -abs (eg)));

        if (Trace)
        {
            write (Term::INITIATIVE, score);
        }

        return score;
    }

    /// scale() evaluates the scale for the position
    template<bool Trace>
    Scale Evaluator<Trace>::scale (Value eg) const
    {
        auto color = eg >= VALUE_ZERO ? WHITE : BLACK;

        auto scl = nullptr != me->scale_func[color] ?
                    (*me->scale_func[color])(pos) :
                    SCALE_NONE;
        if (SCALE_NONE == scl)
        {
            scl = me->scale[color];
        }
        assert(SCALE_NONE != scl);

        // If don't already have an unusual scale, check for certain types of endgames.
        if (SCALE_NORMAL == scl)
        {
            bool bishop_oppose = 1 == pos.count (WHITE|BSHP)
                              && 1 == pos.count (BLACK|BSHP)
                              && opposite_colors (pos.square (WHITE|BSHP), pos.square (BLACK|BSHP));
            return bishop_oppose
                && pos.non_pawn_material () == 2 * VALUE_MG_BSHP ?
                    // Endings with opposite-colored bishops and no other pieces is almost a draw
                    Scale(16 + 4 * pe->passed_count ()) :
                    std::min (Scale(40 + (bishop_oppose ? 2 : 7) * pos.count (color|PAWN)), SCALE_NORMAL);
        }
        return scl;
    }

    /// value() computes the various parts of the evaluation and
    /// returns the value of the position from the point of view of the side to move.
    template<bool Trace>
    Value Evaluator<Trace>::value ()
    {
        assert(0 == pos.si->checkers);

        // Probe the material hash table
        me = Material::probe (pos);
        // If have a specialized evaluation function for the material configuration
        if (nullptr != me->value_func)
        {
            return (*me->value_func) (pos);
        }

        // Probe the pawn hash table
        pe = Pawns::probe (pos);

        Score score;
        // Score is computed internally from the white point of view, initialize by
        // - incrementally updated scores (material + piece square tables).
        // - material imbalance.
        // - pawn score
        score = pos.psq
              + me->imbalance
              + (pe->scores[WHITE] - pe->scores[BLACK])
              + pos.thread->contempt;

        // Early exit if score is high
        Value v = (mg_value (score) + eg_value (score)) / 2;

        if (abs (v) > (Value(1400) + pos.non_pawn_material () / 64)) // Lazy Threshold
        {
            switch (pos.active)
            {
            case WHITE: return +v;
            case BLACK: return -v;
            default: assert(false); return VALUE_ZERO;
            }
        }

        if (Trace)
        {
            clear ();
        }

        initialize<WHITE> ();
        initialize<BLACK> ();

        // Pieces should be evaluated first (populate attack information)
        score += pieces<WHITE, NIHT> () - pieces<BLACK, NIHT> ();
        score += pieces<WHITE, BSHP> () - pieces<BLACK, BSHP> ();
        score += pieces<WHITE, ROOK> () - pieces<BLACK, ROOK> ();
        score += pieces<WHITE, QUEN> () - pieces<BLACK, QUEN> ();

        assert((sgl_attacks[WHITE][NONE] & dbl_attacks[WHITE]) == dbl_attacks[WHITE]);
        assert((sgl_attacks[BLACK][NONE] & dbl_attacks[BLACK]) == dbl_attacks[BLACK]);

        score += mobility[WHITE] - mobility[BLACK];

        // Rest should be evaluated after (full attack information needed including king)
        score += king   <WHITE> () - king   <BLACK> ()
               + threats<WHITE> () - threats<BLACK> ()
               + passers<WHITE> () - passers<BLACK> ()
               + space  <WHITE> () - space  <BLACK> ();

        score += initiative (eg_value (score));

        assert(-VALUE_INFINITE < mg_value (score) && mg_value (score) < +VALUE_INFINITE);
        assert(-VALUE_INFINITE < eg_value (score) && eg_value (score) < +VALUE_INFINITE);
        assert(0 <= me->phase && me->phase <= Material::PhaseResolution);

        // Interpolates between midgame and scaled endgame values.
        v = Value(  (  mg_value (score) * (me->phase)
                     + eg_value (score) * (Material::PhaseResolution - me->phase) * scale (eg_value (score)) / SCALE_NORMAL)
                  / Material::PhaseResolution);

        if (Trace)
        {
            // Write remaining evaluation terms
            write (Term (PAWN), pe->scores[WHITE], pe->scores[BLACK]);
            write (Term::MATERIAL, pos.psq);
            write (Term::IMBALANCE, me->imbalance);
            write (Term::MOBILITY, mobility[WHITE], mobility[BLACK]);
            write (Term::TOTAL, score);
        }

        // Active side's point of view
        switch (pos.active)
        {
        case WHITE: return +v + Tempo;
        case BLACK: return -v + Tempo;
        default: assert(false); return VALUE_ZERO;
        }
    }
}

/// evaluate() returns a static evaluation of the position from the point of view of the side to move.
Value evaluate (const Position &pos)
{
    return Evaluator<false> (pos).value ();
}

/// trace() returns a string (suitable for outputting to stdout) that contains
/// the detailed descriptions and values of each evaluation term.
string trace (const Position &pos)
{
    pos.thread->contempt = SCORE_ZERO; // Reset any dynamic contempt
    auto value = Evaluator<true> (pos).value ();
    // Trace scores are from White's point of view
    switch (pos.active)
    {
    case WHITE: value = +value; break;
    case BLACK: value = -value; break;
    default: assert(false); value = VALUE_ZERO; break;
    }

    ostringstream oss;
    oss << std::setprecision (2) << std::fixed
        << "      Eval Term |    White    |    Black    |    Total     \n"
        << "                |   MG    EG  |   MG    EG  |   MG    EG   \n"
        << "----------------+-------------+-------------+--------------\n"
        << "       Material" << Term::MATERIAL
        << "      Imbalance" << Term::IMBALANCE
        << "           Pawn" << Term (PAWN)
        << "         Knight" << Term (NIHT)
        << "         Bishop" << Term (BSHP)
        << "           Rook" << Term (ROOK)
        << "          Queen" << Term (QUEN)
        << "       Mobility" << Term::MOBILITY
        << "           King" << Term (KING)
        << "         Threat" << Term::THREAT
        << "         Passer" << Term::PASSER
        << "          Space" << Term::SPACE
        << "     Initiative" << Term::INITIATIVE
        << "----------------+-------------+-------------+--------------\n"
        << "          Total" << Term::TOTAL
        << std::endl
        << std::showpos << std::showpoint
        << "Evaluation: " << value_to_cp (value) / 100.0 << " (white side)\n"
        << std::noshowpoint << std::noshowpos;
    return oss.str ();
}
