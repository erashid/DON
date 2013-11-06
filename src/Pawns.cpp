#include "Pawns.h"
#include "BitCount.h"
#include "Position.h"

using namespace BitBoard;

namespace {

#define V Value
#define S(mg, eg) mk_score(mg, eg)

    // Doubled pawn penalty by opposed flag and file
    const Score Doubled[CLR_NO][F_NO] = {
        {S(13, 43), S(20, 48), S(23, 48), S(23, 48),
        S(23, 48), S(23, 48), S(20, 48), S(13, 43), },
        {S(13, 43), S(20, 48), S(23, 48), S(23, 48),
        S(23, 48), S(23, 48), S(20, 48), S(13, 43), } };

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

#undef S
#undef V

    template<Color C>
    Score evaluate(const Position &pos, Pawns::Entry* e)
    {
        const Color  C_  = ((WHITE == C) ? BLACK  : WHITE);
        const Delta RCAP = ((WHITE == C) ? DEL_NE : DEL_SW);
        const Delta LCAP = ((WHITE == C) ? DEL_NW : DEL_SE);

        Score pawn_value = SCORE_ZERO;

        Bitboard w_pawns = pos.pieces(C, PAWN);
        Bitboard b_pawns = pos.pieces(C_, PAWN);

        e->_passed_pawns[C] = e->_candidate_pawns[C] = 0;
        e->_king_sq[C] = SQ_NO;
        e->_semiopen_files[C] = 0xFF;
        e->_pawn_attacks[C] = shift_del<RCAP>(w_pawns) | shift_del<LCAP>(w_pawns);
        e->_num_pawns_on_sq[C][BLACK] = pop_count<MAX15>(w_pawns & DR_SQ_bb);
        e->_num_pawns_on_sq[C][WHITE] = pos.piece_count<PAWN>(C) - e->_num_pawns_on_sq[C][BLACK];

        const SquareList pl = pos.list<PAWN>(C);

        // Loop through all pawns of the current color and score each pawn
        std::for_each (pl.cbegin (), pl.cend (), [&] (Square s)
        {
            const Delta PUSH = ((WHITE == C) ? DEL_N  : DEL_S);

            assert(pos[s] == (C | PAWN));

            File f = _file (s);
            Rank r = rel_rank (C, s);

            // This file cannot be semi-open
            e->_semiopen_files[C] &= ~(1 << f);

            // Our rank plus previous one. Used for chain detection
            Bitboard b = rank_bb(s) | rank_bb(s - pawn_push(C));

            // Flag the pawn as passed, isolated, doubled or member of a pawn
            // chain (but not the backward one).
            bool chain    =   w_pawns & adj_files_bb(f) & b;
            bool isolated = !(w_pawns & adj_files_bb(f));
            bool doubled  =   w_pawns & front_squares_bb(C, s);
            bool opposed  =   b_pawns & front_squares_bb(C, s);
            bool passed   = !(b_pawns & passer_span_pawn_bb(C, s));

            bool backward;
            // Test for backward pawn.
            // If the pawn is passed, isolated, or member of a pawn chain it cannot
            // be backward. If there are friendly pawns behind on adjacent files
            // or if can capture an enemy pawn it cannot be backward either.
            if (   (passed | isolated | chain)
                || (w_pawns & attack_span_pawn_bb(C_, s))
                || (pos.attacks_from<PAWN>(C, s) & b_pawns))
            {
                backward = false;
            }
            else
            {
                // We now know that there are no friendly pawns beside or behind this
                // pawn on adjacent files. We now check whether the pawn is
                // backward by looking in the forward direction on the adjacent
                // files, and picking the closest pawn there.
                b = attack_span_pawn_bb(C, s) & (w_pawns | b_pawns);
                b = attack_span_pawn_bb(C, s) & rank_bb (scan_rel_backmost_sq(C, b));

                // If we have an enemy pawn in the same or next rank, the pawn is
                // backward because it cannot advance without being captured.
                backward = (b | shift_del<PUSH>(b)) & b_pawns;
            }

            assert(opposed | passed | (attack_span_pawn_bb (C, s) & b_pawns));

            // A not passed pawn is a candidate to become passed if it is free to
            // advance and if the number of friendly pawns beside or behind this
            // pawn on adjacent files is higher or equal than the number of
            // enemy pawns in the forward direction on the adjacent files.
            bool candidate =   !(opposed | passed | backward | isolated)
                && (b = attack_span_pawn_bb(C_, s + pawn_push(C)) & w_pawns) != 0
                &&  pop_count<MAX15>(b) >= pop_count<MAX15>(attack_span_pawn_bb(C, s) & b_pawns);

            // Passed pawns will be properly scored in evaluation because we need
            // full attack info to evaluate passed pawns. Only the frontmost passed
            // pawn on each file is considered a true passed pawn.
            if (passed && !doubled) e->_passed_pawns[C] |= s;

            // Score this pawn
            if (isolated)   pawn_value -= Isolated[opposed][f];

            if (doubled)    pawn_value -= Doubled[opposed][f];

            if (backward)   pawn_value -= Backward[opposed][f];

            if (chain)      pawn_value += ChainMember[f][r];

            if (candidate)
            {
                pawn_value += CandidatePassed[r];

                if (!doubled) e->_candidate_pawns[C] |= s;
            }
        });

        return pawn_value;
    }

} // namespace

namespace Pawns {

    // Entry::shelter_storm() calculates shelter and storm penalties for the file
    // the king is on, as well as the two adjacent files.
    template<Color C>
    Value Entry::shelter_storm(const Position &pos, Square ksq)
    {
        const Color C_ = ((WHITE == C) ? BLACK : WHITE);

        Value safety = MaxSafetyBonus;
        Bitboard b = pos.pieces(PAWN) & (front_ranks_bb(C, _rank(ksq)) | rank_bb(ksq));
        Bitboard w_pawns = b & pos.pieces(C);
        Bitboard b_pawns = b & pos.pieces(C_);

        File kf = std::max (F_B, std::min (F_G, _file (ksq)));

        for (File f = kf - 1; f <= kf + 1; ++f)
        {
            b = w_pawns & file_bb (f);
            Rank w_rk = b ? rel_rank (C, scan_rel_backmost_sq (C, b)) : R_1;
            safety -= ShelterWeakness[w_rk];

            b  = b_pawns & file_bb (f);
            Rank b_rk = b ? rel_rank (C, scan_rel_frntmost_sq (C_, b)) : R_1;
            safety -= StormDanger[w_rk == R_1 ? 0 : b_rk == w_rk + 1 ? 2 : 1][b_rk];
        }

        return safety;
    }

    // Entry::update_safety() calculates and caches a bonus for king safety. It is
    // called only when king square changes, about 20% of total king_safety() calls.
    template<Color C>
    Score Entry::update_safety(const Position &pos, Square ksq)
    {
        _king_sq[C] = ksq;
        _castle_rights[C] = pos.can_castle(C);
        _min_dist_KP[C] = 0;

        Bitboard pawns = pos.pieces(C, PAWN);
        if (pawns)
        {
            while (!(dia_rings_bb(ksq, _min_dist_KP[C]++) & pawns))
            {}
        }

        if (rel_rank(C, ksq) > R_4)
        {
            return _king_safety[C] = mk_score(0, -16 * _min_dist_KP[C]);
        }

        Value bonus = shelter_storm<C>(pos, ksq);

        // If we can castle use the bonus after the castle if is bigger
        if (pos.can_castle(mk_castle_right(C, CS_K)))
        {
            bonus = std::max (bonus, shelter_storm<C>(pos, rel_sq(C, SQ_G1)));
        }
        if (pos.can_castle(mk_castle_right(C, CS_Q)))
        {
            bonus = std::max (bonus, shelter_storm<C>(pos, rel_sq(C, SQ_C1)));
        }

        return _king_safety[C] = mk_score(bonus, -16 * _min_dist_KP[C]);
    }

    // Explicit template instantiation
    template Score Entry::update_safety<WHITE>(const Position &pos, Square ksq);
    template Score Entry::update_safety<BLACK>(const Position &pos, Square ksq);


    void initialize ()
    {
        const int32_t chainByFile[8] = { 1, 3, 3, 4, 4, 3, 3, 1 };
        for (Rank r = R_1; r < R_8; ++r)
        {
            for (File f = F_A; f <= F_H; ++f)
            {
                int32_t bonus = r * (r-1) * (r-2) + chainByFile[f] * (r/2 + 1);
                ChainMember[f][r] = mk_score (bonus, bonus);
            }
        }
    }

    // probe() takes a position object as input, computes a Entry object, and returns
    // a pointer to it. The result is also stored in a hash table, so we don't have
    // to recompute everything when the same pawn structure occurs again.
    Entry* probe(const Position &pos, Table &table)
    {
        Key key = pos.pawn_key();
        Entry *e = table[key];

        if (e->key == key) return e;

        e->key = key;
        e->_pawn_value = evaluate<WHITE>(pos, e) - evaluate<BLACK>(pos, e);
        return e;
    }


} // namespace Pawns

