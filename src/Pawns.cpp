#include "Pawns.h"
#include "BitCount.h"
#include "Position.h"

using namespace std;
using namespace BitBoard;

namespace {

#define V Value
#define S(mg, eg) mk_score(mg, eg)

    // Doubled pawn penalty by file
    const Score Doubled[F_NO] = {
        S(13, 43), S(20, 48), S(23, 48), S(23, 48),
        S(23, 48), S(23, 48), S(20, 48), S(13, 43), };

    // Isolated pawn penalty by opposed flag and file
    const Score Isolated[CLR_NO][F_NO] = {
        {S(37, 45), S(54, 52), S(60, 52), S(60, 52),
        S(60, 52), S(60, 52), S(54, 52), S(37, 45), },
        {S(25, 30), S(36, 35), S(40, 35), S(40, 35),
        S(40, 35), S(40, 35), S(36, 35), S(25, 30), } };

    // Backward pawn penalty by opposed flag and file
    const Score Backward[CLR_NO][F_NO] = {
        {S(30, 42), S(43, 46), S(49, 46), S(49, 46),
        S(49, 46), S(49, 46), S(43, 46), S(30, 42), },
        {S(20, 28), S(29, 31), S(33, 31), S(33, 31),
        S(33, 31), S(33, 31), S(29, 31), S(20, 28), } };

    // Candidate passed pawn bonus by [rank]
    const Score CandidatePassed[R_NO] = {
        S( 0, 0), S( 6, 13), S(6,13), S(14,29), S(34,68), S(83,166), S(0, 0), S( 0, 0), };

    // Weakness of our pawn shelter in front of the king indexed by [rank]
    const Value ShelterWeakness[R_NO] = {
        V(100), V(0), V(27), V(73), V(92), V(101), V(101), };

    // Danger of enemy pawns moving toward our king indexed by
    // [no friendly pawn | pawn unblocked | pawn blocked][rank of enemy pawn]
    const Value StormDanger[3][R_NO] = {
        { V( 0),  V(64), V(128), V(51), V(26) },
        { V(26),  V(32), V( 96), V(38), V(20) },
        { V( 0),  V( 0), V( 64), V(25), V(13) }, };

    // Max bonus for king safety. Corresponds to start position with all the pawns
    // in front of the king and no enemy pawn on the horizont.
    const Value MaxSafetyBonus = V(263);

    // Pawn chain membership bonus by [file] and [rank]
    //const Score ChainMember[F_NO][R_NO] = {
    //    { S(0, 0), S(14, 0), S(16, 4), S(18,  9), S(28, 28), S(52, 104), S(118, 236) },
    //    { S(0, 0), S(16, 0), S(18, 5), S(20, 10), S(30, 30), S(54, 108), S(120, 240) },
    //    { S(0, 0), S(16, 0), S(18, 5), S(20, 10), S(30, 30), S(54, 108), S(120, 240) },
    //    { S(0, 0), S(17, 0), S(19, 6), S(22, 11), S(33, 33), S(59, 118), S(127, 254) },
    //    { S(0, 0), S(17, 0), S(19, 6), S(22, 11), S(33, 33), S(59, 118), S(127, 254) },
    //    { S(0, 0), S(16, 0), S(18, 5), S(20, 10), S(30, 30), S(54, 108), S(120, 240) },
    //    { S(0, 0), S(16, 0), S(18, 5), S(20, 10), S(30, 30), S(54, 108), S(120, 240) },
    //    { S(0, 0), S(14, 0), S(16, 4), S(18,  9), S(28, 28), S(52, 104), S(118, 236) }, };

    // Pawn chain membership bonus by [file] and [rank] (initialized by formula)
    Score ChainMember[F_NO][R_NO];

#undef S
#undef V

    template<Color C>
    Score evaluate (const Position &pos, Pawns::Entry *e)
    {
        const Color  C_  = ((WHITE == C) ? BLACK  : WHITE);
        const Delta RCAP = ((WHITE == C) ? DEL_NE : DEL_SW);
        const Delta LCAP = ((WHITE == C) ? DEL_NW : DEL_SE);

        Bitboard pawns[CLR_NO] =
        {
            pos.pieces (C , PAWN),
            pos.pieces (C_, PAWN),
        };

        e->_passed_pawns  [C] = e->_candidate_pawns[C] = 0;
        e->_king_sq       [C] = SQ_NO;
        e->_semiopen_files[C] = 0xFF;
        e->_pawn_attacks  [C] = shift_del<RCAP>(pawns[0]) | shift_del<LCAP>(pawns[0]);
        e->_num_pawns_on_sq[C][BLACK] = pop_count<MAX15>(pawns[0] & DR_SQ_bb);
        e->_num_pawns_on_sq[C][WHITE] = pos.piece_count<PAWN>(C) - e->_num_pawns_on_sq[C][BLACK];

        Score pawn_score = SCORE_ZERO;

        const SquareList pl = pos.list<PAWN> (C);
        // Loop through all pawns of the current color and score each pawn
        for_each (pl.cbegin (), pl.cend (), [&] (Square s)
        {
            const Delta PUSH = ((WHITE == C) ? DEL_N  : DEL_S);

            ASSERT (pos[s] == (C | PAWN));

            File f = _file (s);
            Rank r = rel_rank (C, s);

            // This file cannot be semi-open
            e->_semiopen_files[C] &= ~(1 << f);

            // Our rank plus previous one. Used for chain detection
            Bitboard rr_bb = rank_bb (s) | rank_bb (s - PUSH);

            // Flag the pawn as passed, isolated, doubled or member of a pawn
            // chain (but not the backward one).
            bool chain    =   pawns[0] & adj_files_bb (f) & rr_bb;
            bool isolated = !(pawns[0] & adj_files_bb (f));
            bool doubled  =   pawns[0] & front_squares_bb (C, s);
            bool opposed  =   pawns[1] & front_squares_bb (C, s);
            bool passed   = !(pawns[1] & passer_span_pawn_bb (C, s));

            bool backward;
            // Test for backward pawn.
            // If the pawn is passed, isolated, or member of a pawn chain it cannot
            // be backward. If there are friendly pawns behind on adjacent files
            // or if can capture an enemy pawn it cannot be backward either.
            if (   (passed | isolated | chain)
                || (pawns[0] & attack_span_pawn_bb (C_, s))
                || (pos.attacks_from<PAWN>(C, s) & pawns[1]))
            {
                backward = false;
            }
            else
            {
                Bitboard b;
                // We now know that there are no friendly pawns beside or behind this
                // pawn on adjacent files. We now check whether the pawn is
                // backward by looking in the forward direction on the adjacent
                // files, and picking the closest pawn there.
                b = attack_span_pawn_bb (C, s) & (pawns[0] | pawns[1]);
                b = attack_span_pawn_bb (C, s) & rank_bb (scan_rel_backmost_sq (C, b));

                // If we have an enemy pawn in the same or next rank, the pawn is
                // backward because it cannot advance without being captured.
                backward = (b | shift_del<PUSH> (b)) & pawns[1];
            }

            ASSERT (opposed | passed | (attack_span_pawn_bb (C, s) & pawns[1]));

            // A not passed pawn is a candidate to become passed, if it is free to
            // advance and if the number of friendly pawns beside or behind this
            // pawn on adjacent files is higher or equal than the number of
            // enemy pawns in the forward direction on the adjacent files.
            Bitboard adj_pawns;
            bool candidate =   !(opposed | passed | backward | isolated)
                && (adj_pawns = attack_span_pawn_bb (C_, s + PUSH) & pawns[0]) != 0
                &&  pop_count<MAX15>(adj_pawns) >= pop_count<MAX15>(attack_span_pawn_bb (C, s) & pawns[1]);

            // Passed pawns will be properly scored in evaluation because we need
            // full attack info to evaluate passed pawns. Only the frontmost passed
            // pawn on each file is considered a true passed pawn.
            if (passed && !doubled) e->_passed_pawns[C] |= s;

            // Score this pawn
            if (doubled)    pawn_score -= Doubled[f];

            if (isolated)   pawn_score -= Isolated[opposed][f];

            if (backward)   pawn_score -= Backward[opposed][f];

            if (chain)      pawn_score += ChainMember[f][r];

            if (candidate)
            {
                pawn_score += CandidatePassed[r];

                if (!doubled) e->_candidate_pawns[C] |= s;
            }
        });

        return pawn_score;
    }

} // namespace

namespace Pawns {

    template<Color C>
    // Entry::shelter_storm() calculates shelter and storm penalties for the file
    // the king is on, as well as the two adjacent files.
    Value Entry::shelter_storm (const Position &pos, Square k_sq)
    {
        const Color C_ = ((WHITE == C) ? BLACK : WHITE);
        
        Bitboard front_pawns = pos.pieces (PAWN) & (front_ranks_bb (C, _rank (k_sq)) | rank_bb (k_sq));
        
        Bitboard pawns[CLR_NO] =
        {
            front_pawns & pos.pieces (C ),
            front_pawns & pos.pieces (C_),
        };

        Value safety = MaxSafetyBonus;

        File kf = max (F_B, min (F_G, _file (k_sq)));
        for (File f = kf - 1; f <= kf + 1; ++f)
        {
            Bitboard fb_pawns;
            fb_pawns = pawns[0] & file_bb (f);
            Rank w_rk = fb_pawns ? rel_rank (C, scan_rel_backmost_sq (C, fb_pawns)) : R_1;
            safety -= ShelterWeakness[w_rk];

            fb_pawns  = pawns[1] & file_bb (f);
            Rank b_rk = fb_pawns ? rel_rank (C, scan_rel_frntmost_sq (C_, fb_pawns)) : R_1;
            safety -= StormDanger[(w_rk == R_1) ? 0 : (b_rk == w_rk + 1) ? 2 : 1][b_rk];
        }

        return safety;
    }

    template<Color C>
    // Entry::update_safety() calculates and caches a bonus for king safety. It is
    // called only when king square changes, about 20% of total king_safety() calls.
    Score Entry::update_safety (const Position &pos, Square k_sq)
    {
        _king_sq[C] = k_sq;
        _castle_rights[C] = pos.can_castle(C);
        _min_dist_KP[C] = 0;

        Bitboard pawns = pos.pieces (C, PAWN);
        if (pawns)
        {
            while (!(dia_rings_bb(k_sq, _min_dist_KP[C]) & pawns))
            {
                _min_dist_KP[C]++;
            }
        }

        if (rel_rank(C, k_sq) > R_4)
        {
            return _king_safety[C] = mk_score(0, -16 * _min_dist_KP[C]);
        }

        Value bonus = shelter_storm<C>(pos, k_sq);

        // If we can castle use the bonus after the castle if is bigger
        if (pos.can_castle(mk_castle_right(C, CS_K)))
        {
            bonus = max (bonus, shelter_storm<C>(pos, rel_sq(C, SQ_G1)));
        }
        if (pos.can_castle(mk_castle_right(C, CS_Q)))
        {
            bonus = max (bonus, shelter_storm<C>(pos, rel_sq(C, SQ_C1)));
        }

        return _king_safety[C] = mk_score (bonus, -16 * _min_dist_KP[C]);
    }

    // Explicit template instantiation
    // -------------------------------
    template Score Entry::update_safety<WHITE>(const Position &pos, Square k_sq);
    template Score Entry::update_safety<BLACK>(const Position &pos, Square k_sq);


    void initialize ()
    {
        const int16_t ChainByFile[8] = { 1, 3, 3, 4, 4, 3, 3, 1 };

        for (Rank r = R_1; r < R_8; ++r)
        {
            for (File f = F_A; f <= F_H; ++f)
            {
                int16_t bonus = r * (r-1) * (r-2) + ChainByFile[f] * (r/2 + 1);
                ChainMember[f][r] = mk_score (bonus, bonus);
            }
        }
    }

    // probe() takes a position object as input, computes a Entry object, and returns
    // a pointer to it. The result is also stored in a hash table, so we don't have
    // to recompute everything when the same pawn structure occurs again.
    Entry* probe (const Position &pos, Table &table)
    {
        Key pawn_key = pos.pawn_key ();
        Entry *e = table[pawn_key];

        if (e->_pawn_key == pawn_key) return e;

        e->_pawn_key = pawn_key;
        e->_pawn_score = evaluate<WHITE>(pos, e) - evaluate<BLACK>(pos, e);
        return e;
    }

} // namespace Pawns
