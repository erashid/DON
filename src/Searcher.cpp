﻿#include "Searcher.h"

#include <iterator>

#include "UCI.h"
#include "PRNG.h"
#include "MovePicker.h"
#include "Transposition.h"
#include "Evaluator.h"
#include "Thread.h"
#include "Zobrist.h"
#include "TBsyzygy.h"
#include "Polyglot.h"
#include "Notation.h"
#include "Debugger.h"

using namespace std;
using namespace UCI;
using namespace BitBoard;
using namespace MoveGen;
using namespace MovePick;
using namespace Transposition;
using namespace Evaluator;
using namespace Threading;
using namespace Zobrists;
using namespace TBSyzygy;
using namespace Polyglot;
using namespace Notation;
using namespace Debugger;

/*
// insert_pv_into_tt() inserts the PV back into the TT.
// This makes sure the old PV moves are searched first,
// even if the old TT entries have been overwritten.
void RootMove::insert_pv_into_tt (Position &pos) const
{
    StateInfo states[MaxPlies], *si = states;

    u08 ply = 0;
    for (auto m : *this)
    {
        assert(m != MOVE_NONE);
        assert(MoveList<LEGAL> (pos).contains (m));

        bool tt_hit;
        auto *tte = TT.probe (pos.posi_key (), tt_hit);
        // Don't overwrite correct entries
        if (   !tt_hit
            || m != tte->move ())
        {
            tte->save (pos.posi_key (),
                        m,
                        VALUE_NONE,
                        //pos.checkers () == 0 ?
                        //    evaluate (pos) :
                            VALUE_NONE,
                        DEPTH_NONE,
                        BOUND_NONE,
                        TT.generation ());
        }
        pos.do_move (m, *si++, pos.gives_check (m, CheckInfo (pos)));
        if (++ply >= MaxPlies)
        {
            break;
        }
    }
    while (ply != 0)
    {
        pos.undo_move ();
        --ply;
    }
}
// extract_pv_from_tt() extract the PV back from the TT.
void RootMove::extract_pv_from_tt (Position &pos)
{
    StateInfo states[MaxPlies], *si = states;

    resize (1);
    u08 ply = 0;
    auto m = at (ply);
    pos.do_move (m, *si++, pos.gives_check (m, CheckInfo (pos)));
    ++ply;
        
    auto expected_value = -new_value;
    bool tt_hit;
    const auto *tte = TT.probe (pos.posi_key (), tt_hit);
    while (   tt_hit
            && ply < MaxPlies 
            && expected_value == value_of_tt (tte->value (), ply+1)
            && (m = tte->move ()) != MOVE_NONE // Local copy to be SMP safe
            && pos.pseudo_legal (m)
            && pos.legal (m))
    {
        //assert(m != MOVE_NONE);
        assert(MoveList<LEGAL> (pos).contains (m));
        assert(!pos.draw ());

        *this += m;
        pos.do_move (m, *si++, pos.gives_check (m, CheckInfo (pos)));
        ++ply;

        expected_value = -expected_value;
        tte = TT.probe (pos.posi_key (), tt_hit);
    }
    while (ply != 0)
    {
        pos.undo_move ();
        --ply;
    }
}
*/

// extract_ponder_move_from_tt() is called in case have no ponder move before exiting the search,
// for instance, in case stop the search during a fail high at root.
// Try hard to have a ponder move which has to return to the GUI, otherwise in case of 'ponder on' have nothing to think on.
bool RootMove::extract_ponder_move_from_tt (Position &pos)
{
    assert(size () == 1);
    assert(_ok (at (0)));

    StateInfo si;
    auto m = at (0);
    pos.do_move (m, si, pos.gives_check (m, CheckInfo (pos)));
    bool tt_hit;
    const auto *tte = TT.probe (pos.posi_key (), tt_hit);
    if (   tt_hit
        && (m = tte->move ()) != MOVE_NONE // Local copy to be SMP safe
        && pos.pseudo_legal (m)
        && pos.legal (m))
    {
        //assert(m != MOVE_NONE);
        assert(MoveList<LEGAL> (pos).contains (m));
        assert(!pos.draw ());
        *this += m;
    }
    pos.undo_move ();
    return size () > 1;
}

RootMove::operator string () const
{
    ostringstream oss;
    for (auto m : *this)
    {
        assert(_ok (m));
        oss << ' ' << move_to_can (m);
    }
    return oss.str ();
}

namespace Searcher {

    Limit  Limits;

    atomic_bool
           ForceStop     { false }  // Stop search on request
        ,  PonderhitStop { false }; // Stop search on ponder-hit

    u16    MultiPV       = 1;
    //i32    MultiPV_cp    = 0;

    i16    FixedContempt = 0
        ,  ContemptTime  = 30
        ,  ContemptValue = 50;

    string HashFile     = "Hash.dat";

    bool   OwnBook      = false;
    string BookFile     = "Book.bin";
    bool   BookMoveBest = true;
    i16    BookUptoMove = 20;

    Depth  TBDepthLimit = DEPTH_1;
    i32    TBPieceLimit = 6;
    bool   TBUseRule50  = true;
    u16    TBHits       = 0;
    bool   TBHasRoot    = false;

    string OutputFile   = Empty;

    namespace {

// prefetch() preloads the given address in L1/L2 cache.
// This is a non-blocking function that doesn't stall
// the CPU waiting for data to be loaded from memory,
// which can be quite slow.
#if defined(PREFETCH)

#   if defined(_MSC_VER) || defined(__INTEL_COMPILER)

#       include <xmmintrin.h> // Intel and Microsoft header for _mm_prefetch()

        void prefetch (const void *addr)
        {
#       if defined(__INTEL_COMPILER)
            // This hack prevents prefetches from being optimized away by
            // Intel compiler. Both MSVC and gcc seem not be affected by this.
            __asm__ ("");
#       endif
            _mm_prefetch (reinterpret_cast<const char*> (addr), _MM_HINT_T0);
        }

#   else

        void prefetch (const void *addr)
        {
            __builtin_prefetch (addr);
        }

#   endif

#else

        void prefetch (const void *)
        {}

#endif

        // Razor margin lookup table (initialize at startup)
        // [depth]
        Value RazorMargins[DEPTH_3];
        // Futility move count lookup table (initialize at startup)
        // [improving][depth]
        u08   FutilityMoveCounts[2][DEPTH_16];

        const u08 ReductionMoveCount = 64;
        // ReductionDepths lookup table (initialize at startup)
        // [pv][improving][depth][move_count]
        Depth ReductionDepths[2][2][DEPTH_64][ReductionMoveCount];
        Depth reduction_depths (bool PVNode, bool imp, Depth d, u08 mc)
        {
            return ReductionDepths[PVNode ? 1 : 0]
                                  [imp ? 1 : 0]
                                  [min (d, DEPTH_64 - DEPTH_1)]
                                  [min (mc, u08(ReductionMoveCount-1))];
        }

        Value DrawValue     [CLR_NO]
            , BaseContempt  [CLR_NO];

        CM2DValueStats CounterMoveHistoryValues;

        ofstream OutputStream;

        const u08 TimerResolution = 5;
        // check_limits() is used to print debug info and, more importantly,
        // to detect when out of available limits and thus stop the search.
        void check_limits ()
        {
            static auto last_info_time = now ();

            auto elapsed_time = Threadpool.time_mgr.elapsed_time ();

            auto now_time = Limits.start_time + elapsed_time;
            if (now_time - last_info_time >= MilliSec)
            {
                last_info_time = now_time;
                dbg_print ();
            }

            // An engine may not stop pondering until told so by the GUI
            if (Limits.ponder)
            {
                return;
            }

            if (   (Limits.use_time_management () && elapsed_time > Threadpool.time_mgr.maximum_time - 2 * TimerResolution)
                || (Limits.movetime != 0          && elapsed_time >= Limits.movetime)
                || (Limits.nodes != 0             && Threadpool.nodes () >= Limits.nodes))
            {
                ForceStop = true;
            }
        }

        // update_stats() updates killers, history, countermoves, countermoves and followupmoves history stats
        // when a new quiet best move is found.
        void update_stats (const Position &pos, Stack *ss, Move move, Depth depth, const MoveVector &quiet_moves)
        {
            assert(!pos.empty (org_sq (move)));

            if (ss->killer_moves[0] != move)
            {
                ss->killer_moves[1] = ss->killer_moves[0];
                ss->killer_moves[0] = move;
            }

            auto *thread = pos.thread ();

            auto opp_move_dst = dst_sq ((ss-1)->current_move);
            auto bonus = Value(i32(depth)*(i32(depth) + 2) - 2);

            thread->history_values.update (pos[org_sq (move)], dst_sq (move), bonus);
            thread->org_dst_values.update (pos.active (), move, bonus);

            if ((ss-1)->counter_move_values != nullptr)
            {
                thread->counter_moves.update (pos[opp_move_dst], opp_move_dst, move);
                (ss-1)->counter_move_values->update (pos[org_sq (move)], dst_sq (move), bonus);
            }
            if ((ss-2)->counter_move_values != nullptr)
            {
                (ss-2)->counter_move_values->update (pos[org_sq (move)], dst_sq (move), bonus);
            }
            if ((ss-4)->counter_move_values != nullptr)
            {
                (ss-4)->counter_move_values->update (pos[org_sq (move)], dst_sq (move), bonus);
            }

            // Decrease all the other played quiet moves
            assert(std::find (quiet_moves.begin (), quiet_moves.end (), move) == quiet_moves.end ());
            for (auto m : quiet_moves)
            {
                thread->history_values.update (pos[org_sq (m)], dst_sq (m), -bonus);
                thread->org_dst_values.update (pos.active (), m, -bonus);

                if ((ss-1)->counter_move_values != nullptr)
                {
                    (ss-1)->counter_move_values->update (pos[org_sq (m)], dst_sq (m), -bonus);
                }
                if ((ss-2)->counter_move_values != nullptr)
                {
                    (ss-2)->counter_move_values->update (pos[org_sq (m)], dst_sq (m), -bonus);
                }
                if ((ss-4)->counter_move_values != nullptr)
                {
                    (ss-4)->counter_move_values->update (pos[org_sq (m)], dst_sq (m), -bonus);
                }
            }

            // Extra penalty for PV move in previous ply when it gets refuted
            if (   (ss-1)->move_count == 1
                && pos.capture_type () == NONE)
            {
                bonus += Value(2*i32(depth) + 3);

                if ((ss-2)->counter_move_values != nullptr)
                {
                    (ss-2)->counter_move_values->update (pos[opp_move_dst], opp_move_dst, -bonus);
                }
                if ((ss-3)->counter_move_values != nullptr)
                {
                    (ss-3)->counter_move_values->update (pos[opp_move_dst], opp_move_dst, -bonus);
                }
                if ((ss-5)->counter_move_values != nullptr)
                {
                    (ss-5)->counter_move_values->update (pos[opp_move_dst], opp_move_dst, -bonus);
                }
            }
        }
        void update_stats (const Position &pos, Stack *ss, Move move, Depth depth)
        {
            static const MoveVector quiet_moves (0);
            update_stats (pos, ss, move, depth, quiet_moves);
        }

        // update_pv() add current move and appends child pv[]
        void update_pv (MoveVector &pv, Move move, const MoveVector &child_pv)
        {
            assert(_ok (move));
            pv.clear ();
            pv.push_back (move);
            if (!child_pv.empty ())
            {
                pv.reserve (child_pv.size () + 1);
                std::copy (child_pv.begin (), child_pv.end (), std::back_inserter (pv));
            }
        }

        // It adjusts a mate score from "plies to mate from the root" to "plies to mate from the current position".
        // Non-mate scores are unchanged.
        // The function is called before storing a value to the transposition table.
        Value value_to_tt (Value v, i32 ply)
        {
            assert(v != VALUE_NONE);
            return v >= +VALUE_MATE_IN_MAX_PLY ? v + ply :
                   v <= -VALUE_MATE_IN_MAX_PLY ? v - ply :
                   v;
        }
        // It adjusts a mate score from "plies to mate from the current position" to "plies to mate from the root".
        // Non-mate scores are unchanged.
        // The function is called after retrieving a value of the transposition table.
        Value value_of_tt (Value v, i32 ply)
        {
            return v == VALUE_NONE             ? VALUE_NONE :
                   v >= +VALUE_MATE_IN_MAX_PLY ? v - ply :
                   v <= -VALUE_MATE_IN_MAX_PLY ? v + ply :
                   v;
        }

        // Formats PV information according to UCI protocol.
        // UCI requires to send all the PV lines also if are still to be searched
        // and so refer to the previous search score.
        string multipv_info (Thread *const &thread, Value alfa, Value beta)
        {
            auto pv_index     = thread->pv_index;
            auto total_nodes  = Threadpool.nodes ();
            auto elapsed_time = std::max (Threadpool.time_mgr.elapsed_time (), TimePoint(1));
            assert(elapsed_time > 0);

            ostringstream oss;
            for (u16 i = 0; i < Threadpool.pv_limit; ++i)
            {
                auto d = i <= pv_index ?
                    thread->running_depth :
                    thread->running_depth - DEPTH_1;
                if (d <= DEPTH_0)
                {
                    continue;
                }
                auto v = i <= pv_index ?
                    thread->root_moves[i].new_value :
                    thread->root_moves[i].old_value;
                bool tb =
                       TBHasRoot
                    && abs (v) < +VALUE_MATE - i32(MaxPlies);

                oss << "info"
                    << " multipv "  << std::setw (2) << i + 1
                    << " depth "    << d
                    << " seldepth " << thread->max_ply
                    << " score "    << to_string (tb ? ProbeValue : v)
                    << (!tb && i == pv_index ?
                            beta <= v ? " lowerbound" :
                                v <= alfa ? " upperbound" : "" : "")
                    << " nodes "    << total_nodes
                    << " time "     << elapsed_time
                    << " nps "      << total_nodes * MilliSec / elapsed_time
                    << " hashfull " << (elapsed_time > MilliSec ? TT.hash_full () : 0)
                    << " tbhits "   << TBHits
                    << " pv"        << thread->root_moves[i]
                    << (i+1 < Threadpool.pv_limit ? '\n' : '\0');
            }
            return oss.str ();
        }

        // quien_search<>() is the quiescence search function,
        // which is called by the main depth limited search function
        // when the remaining depth is less than equal to DEPTH_0.
        template<bool PVNode, bool InCheck>
        Value quien_search (Position &pos, Stack *const &ss, Value alfa, Value beta, Depth depth)
        {
            assert(InCheck == (pos.checkers () != 0));
            assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
            assert(PVNode || (alfa == beta-1));
            assert(depth <= DEPTH_0);
            assert(ss->ply >= 1
                && ss->ply == (ss-1)->ply + 1
                && ss->ply < MaxPlies);

            auto pv_alfa = -VALUE_INFINITE;

            if (PVNode)
            {
                pv_alfa = alfa; // To flag BOUND_EXACT when eval above alfa and no available moves

                ss->pv.clear ();
            }

            ss->current_move = MOVE_NONE;
            ss->counter_move_values = nullptr;

            // Check for an immediate draw or maximum ply reached
            if (   pos.draw ()
                || ss->ply >= MaxPlies)
            {
                return ss->ply >= MaxPlies
                    && !InCheck ?
                        evaluate (pos) :
                        DrawValue[pos.active ()];
            }

            CheckInfo ci (pos);

            Move move;
            // Transposition table lookup
            auto posi_key = pos.posi_key ();
            bool tt_hit;
            auto *tte = TT.probe (posi_key, tt_hit);
            auto tt_move =
                   tt_hit
                && (move = tte->move ()) != MOVE_NONE
                && pos.pseudo_legal (move)
                && pos.legal (move, ci.abs_pinneds) ?
                    move :
                    MOVE_NONE;
            assert(   tt_move == MOVE_NONE
                   || (   pos.pseudo_legal (tt_move)
                       && pos.legal (tt_move, ci.abs_pinneds)));
            auto tt_ext   = tt_hit
                         && tte->move () == tt_move;
            auto tt_value = tt_ext ?
                            value_of_tt (tte->value (), ss->ply) :
                            VALUE_NONE;

            // Decide whether or not to include checks,
            // this fixes also the type of TT entry depth that are going to use.
            // Note that in quien_search use only 2 depth in TT: QS_CHECK (0) or QS_NO_CHECK (-1).
            auto qs_depth =
                   InCheck
                || depth == DEPTH_0 ?
                    DEPTH_0 : DEPTH_1_;

            if (   !PVNode
                && tt_ext
                && tte->depth () >= qs_depth
                && tt_value != VALUE_NONE // Only in case of TT access race
                && (  tte->bound ()
                    & (tt_value >= beta ? BOUND_LOWER : BOUND_UPPER)) != BOUND_NONE)
            {
                if (tt_move != MOVE_NONE)
                {
                    ss->current_move = tt_move;
                    ss->counter_move_values = &CounterMoveHistoryValues(pos[org_sq (tt_move)], dst_sq (tt_move));
                }
                return tt_value;
            }

            Value best_value
                , futility_base;

            // Evaluate the position statically
            if (InCheck)
            {
                ss->static_eval = VALUE_NONE;
                best_value =
                futility_base = -VALUE_INFINITE;
            }
            else
            {
                Value tt_eval;
                if (tt_ext)
                {
                    // Never assume anything on values stored in TT
                    ss->static_eval = tt_eval =
                        tte->eval () != VALUE_NONE ?
                            tte->eval () :
                            evaluate (pos);
                    // Can tt_value be used as a better position evaluation?
                    if (   tt_value != VALUE_NONE
                        && (  tte->bound ()
                            & (tt_value > tt_eval ? BOUND_LOWER : BOUND_UPPER)) != BOUND_NONE)
                    {
                        tt_eval = tt_value;
                    }
                }
                else
                {
                    ss->static_eval = tt_eval =
                        (ss-1)->current_move != MOVE_NULL ?
                            evaluate (pos) :
                            -(ss-1)->static_eval + 2*Tempo;

                    tte->save (posi_key,
                               MOVE_NONE,
                               VALUE_NONE,
                               ss->static_eval,
                               DEPTH_NONE,
                               BOUND_NONE,
                               TT.generation ());
                }

                if (alfa < tt_eval)
                {
                    // Stand pat. Return immediately if static value is at least beta
                    if (tt_eval >= beta)
                    {
                        if (!tt_ext)
                        {
                            tte->save (posi_key,
                                       MOVE_NONE,
                                       value_to_tt (tt_eval, ss->ply),
                                       ss->static_eval,
                                       qs_depth,
                                       BOUND_LOWER,
                                       TT.generation ());
                        }

                        assert(-VALUE_INFINITE < tt_eval && tt_eval < +VALUE_INFINITE);
                        return tt_eval;
                    }

                    assert(tt_eval < beta);
                    // Update alfa! Always alfa < beta
                    if (PVNode)
                    {
                        alfa = tt_eval;
                    }
                }

                best_value = tt_eval;
                futility_base = best_value + 128;
            }

            auto *thread = pos.thread ();
            auto best_move = MOVE_NONE;

            // Initialize move picker for the current position, and prepare to search the moves.
            MovePicker mp (pos, tt_move, depth, (ss-1)->current_move);
            StateInfo si;
            // Loop through the moves until no moves remain or a beta cutoff occurs.
            while ((move = mp.next_move ()) != MOVE_NONE)
            {
                // Check for legality before making the move
                assert(_ok (move)
                    && pos.pseudo_legal (move)
                    && pos.legal (move, ci.abs_pinneds));

                auto mpc = pos[org_sq (move)];
                auto mpt = ptype (mpc);
                assert(mpc != NO_PIECE
                    && mpt != NONE);
                auto dst = dst_sq (move);

                bool gives_check =
                       mtype (move) == NORMAL
                    && ci.dsc_checkers == 0 ?
                        (ci.checking_bb[mpt] & dst) != 0 :
                        pos.gives_check (move, ci);

                // Futility pruning
                if (   !InCheck
                    && Limits.mate == 0
                    && futility_base > -VALUE_KNOWN_WIN
                    && futility_base <= alfa
                    && !gives_check
                        // Advance pawn push
                    && !(   mpt == PAWN
                         && rel_rank (pos.active (), dst) > R_5))
                {
                    // Due to not advanced pawn push
                    assert(mtype (move) != ENPASSANT
                        && mtype (move) != PROMOTE);

                    // Futility pruning parent node
                    auto futility_value = futility_base + PieceValues[EG][ptype (pos[dst])];
                    if (futility_value <= alfa)
                    {
                        if (best_value < futility_value)
                        {
                            best_value = futility_value;
                        }
                        continue;
                    }
                    // Prune moves with negative or zero SEE
                    if (pos.see (move) <= VALUE_ZERO)
                    {
                        if (best_value < futility_base)
                        {
                            best_value = futility_base;
                        }
                        continue;
                    }
                }

                // Don't search moves with negative SEE values
                if (   (   !InCheck
                        // Evasion Prunable: Detect non-capture evasions that are candidate to be pruned
                        || (   best_value > -VALUE_MATE_IN_MAX_PLY
                            && !pos.capture (move)))
                    && Limits.mate == 0
                    && mtype (move) != PROMOTE
                    && pos.see_sign (move) < VALUE_ZERO)
                {
                    continue;
                }

                ss->current_move = move;
                ss->counter_move_values = &CounterMoveHistoryValues(mpc, dst);

                bool capture_or_promotion = pos.capture_or_promotion (move);

                // Speculative prefetch as early as possible
                prefetch (TT.cluster_entry (pos.move_posi_key (move)));

                // Make the move
                pos.do_move (move, si, gives_check);

                if (   mpt == PAWN
                    || pos.capture_type () == PAWN)
                {
                    prefetch (thread->pawn_table[pos.pawn_key ()]);
                }
                if (capture_or_promotion)
                {
                    prefetch (thread->matl_table[pos.matl_key ()]);
                }

                auto value =
                    gives_check ?
                        -quien_search<PVNode, true > (pos, ss+1, -beta, -alfa, depth - DEPTH_1) :
                        -quien_search<PVNode, false> (pos, ss+1, -beta, -alfa, depth - DEPTH_1);

                // Undo the move
                pos.undo_move ();

                assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);

                // Check for new best move
                if (best_value < value)
                {
                    best_value = value;

                    if (alfa < value)
                    {
                        best_move = move;

                        // Update pv even in fail-high case
                        if (PVNode)
                        {
                            update_pv (ss->pv, move, (ss+1)->pv);
                        }
                        // Fail high
                        if (value >= beta)
                        {
                            tte->save (posi_key,
                                       move,
                                       value_to_tt (value, ss->ply),
                                       ss->static_eval,
                                       qs_depth,
                                       BOUND_LOWER,
                                       TT.generation ());

                            assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);
                            return value;
                        }

                        assert(value < beta);
                        // Update alfa! Always alfa < beta
                        if (PVNode)
                        {
                            alfa = value;
                        }
                    }
                }
            }

            // All legal moves have been searched.
            // A special case: If in check and no legal moves were found, it is checkmate.
            if (   InCheck
                && best_value == -VALUE_INFINITE)
            {
                // Plies to mate from the root
                return mated_in (ss->ply);
            }

            tte->save (posi_key,
                       best_move,
                       value_to_tt (best_value, ss->ply),
                       ss->static_eval,
                       qs_depth,
                          PVNode
                       && pv_alfa < best_value ?
                           BOUND_EXACT :
                           BOUND_UPPER,
                       TT.generation ());

            assert(-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);
            return best_value;
        }

        // depth_search<>() is the main depth limited search function.
        template<bool PVNode, bool CutNode, bool InCheck>
        Value depth_search (Position &pos, Stack *const &ss, Value alfa, Value beta, Depth depth)
        {
            const bool root_node =
                   PVNode
                && ss->ply == 1;
            assert(!(PVNode && CutNode));
            assert(InCheck == (pos.checkers () != 0));
            assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
            assert(PVNode || (alfa == beta-1));
            assert(DEPTH_0 < depth && depth < DEPTH_MAX);
            assert(ss->ply >= 1
                && ss->ply == (ss-1)->ply + 1
                && ss->ply < MaxPlies);

            // Step 1. Initialize node
            auto *thread = pos.thread ();
            // Check for the available remaining limit
            if (thread->reset_count.load (memory_order_relaxed))
            {
                thread->reset_count = false;
                thread->count = 0;
            }
            if (++thread->count >= TimerResolution*MilliSec)
            {
                for (auto *th : Threadpool)
                {
                    th->reset_count = true;
                }
                check_limits ();
            }

            if (PVNode)
            {
                // Used to send 'seldepth' info to GUI
                if (thread->max_ply < ss->ply)
                {
                    thread->max_ply = ss->ply;
                }
            }

            ss->move_count = 0;
            ss->current_move = MOVE_NONE;
            ss->counter_move_values = nullptr;

            if (!root_node)
            {
                // Step 2. Check end condition
                // Check for aborted search, immediate draw or maximum ply reached
                if (   ForceStop.load (memory_order_relaxed)
                    || pos.draw ()
                    || ss->ply >= MaxPlies)
                {
                    return ss->ply >= MaxPlies
                        && !InCheck ?
                            evaluate (pos) :
                            DrawValue[pos.active ()];
                }

                // Step 3. Mate distance pruning.
                // Even if mate at the next move our score would be at best mates_in(ss->ply+1),
                // but if alfa is already bigger because a shorter mate was found upward in the tree
                // then there is no need to search further, will never beat current alfa.
                // Same logic but with reversed signs applies also in the opposite condition of
                // being mated instead of giving mate, in this case return a fail-high score.
                alfa = std::max (mated_in (ss->ply+0), alfa);
                beta = std::min (mates_in (ss->ply+1), beta);
                if (alfa >= beta)
                {
                    return alfa;
                }
            }

            assert((ss+1)->exclude_move == MOVE_NONE);
            assert(!(ss+1)->skip_pruning);

            std::fill_n ((ss+2)->killer_moves, MaxKillers, MOVE_NONE);

            CheckInfo ci (pos);
            
            Move move;
            // Step 4. Transposition table lookup
            // Don't want the score of a partial search to overwrite a previous full search
            // TT value, so use a different position key in case of an excluded move.
            auto exclude_move = ss->exclude_move;
            auto posi_key =
                exclude_move == MOVE_NONE ?
                    pos.posi_key () :
                    pos.posi_key () ^ Zobrist::ExclusionKey;
            bool tt_hit;
            auto *tte = TT.probe (posi_key, tt_hit);
            auto tt_move =
                root_node ?
                    thread->root_moves[thread->pv_index][0] :
                       tt_hit
                    && (move = tte->move ()) != MOVE_NONE
                    && pos.pseudo_legal (move)
                    && pos.legal (move, ci.abs_pinneds) ?
                        move :
                        MOVE_NONE;
            assert(   tt_move == MOVE_NONE
                   || (   pos.pseudo_legal (tt_move)
                       && pos.legal (tt_move, ci.abs_pinneds)));
            auto tt_ext   = tt_hit
                         && (   tte->move () == tt_move
                             || (   root_node
                                 && tte->move () == MOVE_NONE));
            auto tt_value = tt_ext ?
                            value_of_tt (tte->value (), ss->ply) :
                            VALUE_NONE;

            // At non-PV nodes we check for an early TT cutoff
            if (   !PVNode
                && tt_ext
                && tte->depth () >= depth
                && tt_value != VALUE_NONE // Only in case of TT access race
                && (  tte->bound ()
                    & (tt_value >= beta ? BOUND_LOWER : BOUND_UPPER)) != BOUND_NONE)
            {
                if (tt_move != MOVE_NONE)
                {
                    ss->current_move = tt_move;
                    ss->counter_move_values = &CounterMoveHistoryValues(pos[org_sq (tt_move)], dst_sq (tt_move));

                    // If tt_move is quiet, update killers, history, countermove and countermoves history on TT hit
                    if (   tt_value >= beta
                        && !pos.capture_or_promotion (tt_move))
                    {
                        update_stats (pos, ss, tt_move, depth);
                    }
                }
                return tt_value;
            }

            // Step 4A. Tablebase probe
            if (   !root_node
                && TBPieceLimit != 0)
            {
                auto piece_count = pos.count<NONE> ();

                if (   (   piece_count < TBPieceLimit
                        || (   piece_count == TBPieceLimit
                            && depth >= TBDepthLimit))
                    && pos.clock_ply () == 0
                    && pos.can_castle (CR_ANY) == CR_NONE)
                {
                    i32 found;
                    auto v = probe_wdl (pos, found);

                    if (found != 0)
                    {
                        ++TBHits;

                        auto draw_v = TBUseRule50 ? 1 : 0;

                        auto value =
                            v < -draw_v ? -VALUE_MATE + i32(MaxPlies + ss->ply) :
                            v > +draw_v ? +VALUE_MATE - i32(MaxPlies + ss->ply) :
                            VALUE_ZERO + 2 * draw_v * v;

                        tte->save (posi_key,
                                   MOVE_NONE,
                                   value_to_tt (value, ss->ply),
                                   //pos.checkers () == 0 ?
                                   //    evaluate (pos) :
                                       VALUE_NONE,
                                   std::min (depth + DEPTH_6, DEPTH_MAX - DEPTH_1),
                                   BOUND_EXACT,
                                   TT.generation ());

                        return value;
                    }
                }
            }

            StateInfo si;

            // Step 5. Evaluate the position statically
            if (InCheck)
            {
                ss->static_eval = VALUE_NONE;
            }
            else
            {
                Value tt_eval;
                if (tt_ext)
                {
                    // Never assume anything on values stored in TT
                    ss->static_eval = tt_eval =
                        tte->eval () != VALUE_NONE ?
                            tte->eval () :
                            evaluate (pos);
                    // Can tt_value be used as a better position evaluation?
                    if (   tt_value != VALUE_NONE
                        && (  tte->bound ()
                            & (tt_value > tt_eval ? BOUND_LOWER : BOUND_UPPER)) != BOUND_NONE)
                    {
                        tt_eval = tt_value;
                    }
                }
                else
                {
                    ss->static_eval = tt_eval =
                        (ss-1)->current_move != MOVE_NULL ?
                            evaluate (pos) :
                            -(ss-1)->static_eval + 2*Tempo;

                    tte->save (posi_key,
                               MOVE_NONE,
                               VALUE_NONE,
                               ss->static_eval,
                               DEPTH_NONE,
                               BOUND_NONE,
                               TT.generation ());
                }

                if (!ss->skip_pruning)
                {
                    // Step 6. Futility pruning: child node
                    // Betting that the opponent doesn't have a move that will reduce
                    // the score by more than futility margins [depth] if do a null move.
                    if (   !root_node
                        && Limits.mate == 0
                        && depth < DEPTH_7
                        && tt_eval < +VALUE_KNOWN_WIN
                        && pos.non_pawn_material (pos.active ()) > VALUE_ZERO)
                    {
                        auto stand_pat = tt_eval - 150*i32(depth);
                        if (stand_pat >= beta)
                        {
                            return stand_pat;
                        }
                    }

                    // Step 7. Razoring sort of forward pruning where rather than
                    // skipping an entire subtree, search it to a reduced depth.
                    if (   !PVNode
                        && Limits.mate == 0
                        && tt_move == MOVE_NONE
                        && depth < DEPTH_3
                        && tt_eval + RazorMargins[depth] <= alfa)
                    {
                        if (   depth == DEPTH_1
                            && tt_eval + RazorMargins[DEPTH_0] <= alfa)
                        {
                            return quien_search<false, false> (pos, ss, alfa, beta, DEPTH_0);
                        }
                        auto alfa_margin = std::max (alfa - RazorMargins[depth], -VALUE_INFINITE);
                        auto value = quien_search<false, false> (pos, ss, alfa_margin, alfa_margin+1, DEPTH_0);
                        if (value <= alfa_margin)
                        {
                            return value;
                        }
                    }

                    // Step 8. Null move search with verification search
                    if (   !PVNode
                        && Limits.mate == 0
                        && tt_eval >= beta
                        && (   depth > DEPTH_12
                            || ss->static_eval + 35*i32(depth - DEPTH_6) >= beta)
                        && pos.non_pawn_material (pos.active ()) > VALUE_ZERO)
                    {
                        assert(exclude_move == MOVE_NONE);
                        assert(_ok ((ss-1)->current_move)
                            && (ss-1)->counter_move_values != nullptr);

                        ss->current_move = MOVE_NULL;
                        ss->counter_move_values = nullptr;

                        // Null move dynamic reduction based on depth and static evaluation
                        auto reduced_depth = depth - Depth((67*i32(depth) + 823) / 256 + std::min (i32(tt_eval - beta)/200, 3));

                        // Speculative prefetch as early as possible
                        prefetch (TT.cluster_entry (  pos.posi_key ()
                                                    ^ Zob.active_color
                                                    ^ (pos.en_passant_sq () != SQ_NO ? Zob.en_passant[_file (pos.en_passant_sq ())] : 0)));

                        pos.do_null_move (si);
                        (ss+1)->skip_pruning = true;
                        auto null_value =
                            reduced_depth <= DEPTH_0 ?
                                -quien_search<false, false> (pos, ss+1, -beta, -(beta-1), DEPTH_0) :
                                -depth_search<false, !CutNode, false> (pos, ss+1, -beta, -(beta-1), reduced_depth);
                        (ss+1)->skip_pruning = false;
                        pos.undo_null_move ();

                        if (null_value >= beta)
                        {
                            // Don't do verification search at low depths
                            if (   depth <= DEPTH_12
                                && abs (beta) < +VALUE_KNOWN_WIN)
                            {
                                // Don't return unproven mates
                                return null_value < +VALUE_MATE_IN_MAX_PLY ?
                                        null_value : beta;
                            }

                            // Do verification search at high depths
                            ss->skip_pruning = true;
                            auto value =
                                reduced_depth <= DEPTH_0 ?
                                    quien_search<false, false> (pos, ss, beta-1, beta, DEPTH_0) :
                                    depth_search<false, false, false> (pos, ss, beta-1, beta, reduced_depth);
                            ss->skip_pruning = false;

                            if (value >= beta)
                            {
                                // Don't return unproven mates
                                return null_value < +VALUE_MATE_IN_MAX_PLY ?
                                        null_value : beta;
                            }
                        }
                    }

                    // Step 9. ProbCut
                    // If have a very good capture (i.e. SEE > see[captured_piece_type])
                    // and a reduced search returns a value much above beta,
                    // then can (almost) safely prune the previous move.
                    if (   !PVNode
                        && Limits.mate == 0
                        && depth > DEPTH_4
                        && abs (beta) < +VALUE_MATE_IN_MAX_PLY)
                    {
                        // ProbCut shallow depth
                        auto reduced_depth = depth - DEPTH_4;
                        auto beta_margin = std::min (beta + 200, +VALUE_INFINITE);

                        assert(reduced_depth > DEPTH_0);
                        assert(_ok ((ss-1)->current_move)
                            && (ss-1)->counter_move_values != nullptr);

                        // Initialize move picker for the current position, and prepare to search the moves.
                        MovePicker mp (pos, tt_move, PieceValues[MG][pos.capture_type ()]);
                        // Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs.
                        while ((move = mp.next_move ()) != MOVE_NONE)
                        {
                            // Check for legality before making the move
                            assert(_ok (move)
                                && pos.pseudo_legal (move)
                                && pos.legal (move, ci.abs_pinneds));

                            auto mpc = pos[org_sq (move)];
                            auto mpt = ptype (mpc);
                            assert(mpc != NO_PIECE
                                && mpt != NONE);
                            auto dst = dst_sq (move);

                            ss->current_move = move;
                            ss->counter_move_values = &CounterMoveHistoryValues(mpc, dst);

                            bool gives_check =
                                   mtype (move) == NORMAL
                                && ci.dsc_checkers == 0 ?
                                    (ci.checking_bb[mpt] & dst) != 0 :
                                    pos.gives_check (move, ci);
                            bool capture_or_promotion = pos.capture_or_promotion (move);

                            // Speculative prefetch as early as possible
                            prefetch (TT.cluster_entry (pos.move_posi_key (move)));

                            pos.do_move (move, si, gives_check);

                            if (   mpt == PAWN
                                || pos.capture_type () == PAWN)
                            {
                                prefetch (thread->pawn_table[pos.pawn_key ()]);
                            }
                            if (capture_or_promotion)
                            {
                                prefetch (thread->matl_table[pos.matl_key ()]);
                            }

                            auto value =
                                gives_check ?
                                    -depth_search<false, !CutNode, true > (pos, ss+1, -beta_margin, -beta_margin+1, reduced_depth) :
                                    -depth_search<false, !CutNode, false> (pos, ss+1, -beta_margin, -beta_margin+1, reduced_depth);

                            pos.undo_move ();

                            if (value >= beta_margin)
                            {
                                return value;
                            }
                        }
                    }

                    // Step 10. Internal iterative deepening (IID)
                    if (   tt_move == MOVE_NONE
                        && depth > (PVNode ? DEPTH_4 : DEPTH_7)
                        && (   PVNode
                            || ss->static_eval + 256 >= beta))
                    {
                        ss->skip_pruning = true;
                        depth_search<PVNode, CutNode, false> (pos, ss, alfa, beta, depth - DEPTH_2 - (PVNode ? DEPTH_0 : depth/4));
                        ss->skip_pruning = false;

                        tte = TT.probe (posi_key, tt_hit);
                        if (tt_hit)
                        {
                            tt_move =
                                   (move = tte->move ()) != MOVE_NONE
                                && pos.pseudo_legal (move)
                                && pos.legal (move, ci.abs_pinneds) ?
                                    move :
                                    MOVE_NONE;
                            assert(   tt_move == MOVE_NONE
                                   || (   pos.pseudo_legal (tt_move)
                                       && pos.legal (tt_move, ci.abs_pinneds)));
                            tt_ext = tte->move () == tt_move;
                            if (tt_ext)
                            {
                                tt_value = value_of_tt (tte->value (), ss->ply);
                            }
                        }
                    }
                }
            }

            // When in check search starts from here
            auto value      = -VALUE_INFINITE
               , best_value = -VALUE_INFINITE;

            auto best_move  = MOVE_NONE;

            bool singular_ext_node =
                   !root_node
                && tt_ext
                && exclude_move == MOVE_NONE // Recursive singular search is not allowed
                && tt_move != MOVE_NONE
                && depth > DEPTH_7
                && depth < tte->depth () + DEPTH_4
                && abs (tt_value) < +VALUE_KNOWN_WIN
                && (tte->bound () & BOUND_LOWER) != BOUND_NONE;

            bool improving =
                   (ss-2)->static_eval <= (ss-0)->static_eval
                || (ss-2)->static_eval == VALUE_NONE;

            u08 move_count = 0;

            MoveVector quiet_moves;
            quiet_moves.reserve (16);

            // Initialize move picker for the current position, and prepare to search the moves.
            MovePicker mp (pos, tt_move, ss);
            // Step 11. Loop through moves
            // Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs.
            while ((move = mp.next_move ()) != MOVE_NONE)
            {
                // Check for legality before making the move
                assert(_ok (move)
                    && pos.pseudo_legal (move)
                    && pos.legal (move, ci.abs_pinneds));

                if (   // At root obey the "searchmoves" option and skip moves not listed in
                       // RootMove list, as a consequence any illegal move is also skipped.
                       // In MultiPV mode also skip PV moves which have been already searched.
                       (   root_node
                        && std::find (thread->root_moves.begin () + thread->pv_index,
                                      thread->root_moves.end (), move) ==
                                      thread->root_moves.end ())
                       // Skip exclusion move
                    || move == exclude_move)
                {
                    continue;
                }

                ss->move_count = ++move_count;
                auto mpc = pos[org_sq (move)];
                auto mpt = ptype (mpc);
                assert(mpc != NO_PIECE
                    && mpt != NONE);
                auto dst = dst_sq (move);

                if (   root_node
                    && Threadpool.main_thread () == thread)
                {
                    auto elapsed_time = Threadpool.time_mgr.elapsed_time ();
                    if (elapsed_time > 3*MilliSec)
                    {
                        sync_cout
                            << "info"
                            << " depth "          << depth
                            << " currmovenumber " << std::setw (2) << thread->pv_index + move_count
                            << " currmove "       << move_to_can (move)
                            << " time "           << elapsed_time
                            << sync_endl;
                    }
                }

                if (PVNode)
                {
                    (ss+1)->pv.clear ();
                }

                bool gives_check =
                       mtype (move) == NORMAL
                    && ci.dsc_checkers == 0 ?
                        (ci.checking_bb[mpt] & dst) != 0 :
                        pos.gives_check (move, ci);
                bool capture_or_promotion = pos.capture_or_promotion (move);

                bool move_count_pruning =
                       depth < DEPTH_16
                    && move_count >= FutilityMoveCounts[improving][depth];

                // Step 12. Extend the move which seems dangerous like ...checks etc.
                bool extension =
                       gives_check
                    && !move_count_pruning
                    && pos.see_sign (move) >= VALUE_ZERO;

                // Singular extensions (SE).
                // We extend the TT move if its value is much better than its siblings.
                // If all moves but one fail low on a search of (alfa-s, beta-s),
                // and just one fails high on (alfa, beta), then that move is singular and should be extended.
                // To verify this do a reduced search on all the other moves but the tt_move,
                // if result is lower than and equal to tt_value minus a margin then extend tt_move.
                if (   !extension
                    && singular_ext_node
                    && move == tt_move)
                {
                    auto alfa_margin = std::max (tt_value - 2*i32(depth), -VALUE_INFINITE);
                    ss->exclude_move = move;
                    ss->skip_pruning = true;
                    value = depth_search<false, CutNode, InCheck> (pos, ss, alfa_margin, alfa_margin+1, depth/2);
                    ss->skip_pruning = false;
                    ss->exclude_move = MOVE_NONE;
                    if (value <= alfa_margin)
                    {
                        extension = true;
                    }
                }

                // Update the current move (this must be done after singular extension search)
                auto new_depth = depth - DEPTH_1 + (extension ? DEPTH_1 : DEPTH_0);

                // Step 13. Pruning at shallow depth
                if (   !InCheck
                    && !root_node
                    && Limits.mate == 0
                    && !capture_or_promotion
                    && best_value > -VALUE_MATE_IN_MAX_PLY
                        // ! Dangerous conditions
                    && !gives_check
                        // Advance pawn push
                    && !(   mpt == PAWN
                         && rel_rank (pos.active (), dst) > R_5))
                {
                    // Due to not advanced pawn push
                    assert(mtype (move) != ENPASSANT
                        && mtype (move) != PROMOTE);

                    if (    // Move count based pruning
                           move_count_pruning
                        ||
                            // Counter move values based pruning
                           (   depth < DEPTH_5
                            && move != ss->killer_moves[0]
                            && ((ss-1)->counter_move_values == nullptr || (*(ss-1)->counter_move_values)(mpc, dst) < VALUE_ZERO)
                            && ((ss-2)->counter_move_values == nullptr || (*(ss-2)->counter_move_values)(mpc, dst) < VALUE_ZERO)
                            && (   ((ss-1)->counter_move_values != nullptr && (ss-2)->counter_move_values != nullptr)
                                || (ss-4)->counter_move_values == nullptr || (*(ss-4)->counter_move_values)(mpc, dst) < VALUE_ZERO)))
                    {
                        continue;
                    }

                    // Value based pruning
                    auto predicted_depth = std::max (new_depth - reduction_depths (PVNode, improving, depth, move_count), DEPTH_0);
                    if (    // Futility pruning: parent node
                           (   predicted_depth < DEPTH_7
                            && ss->static_eval + 200*i32(predicted_depth) + 256 <= alfa)
                            // SEE pruning below a decreasing threshold with depth.
                        || (   predicted_depth < DEPTH_9
                            && pos.see_sign (move) < -400*i32(std::max (predicted_depth - DEPTH_3, DEPTH_0))))
                    {
                        continue;
                    }
                }

                ss->current_move = move;
                ss->counter_move_values = &CounterMoveHistoryValues(mpc, dst);

                // Speculative prefetch as early as possible
                prefetch (TT.cluster_entry (pos.move_posi_key (move)));

                // Step 14. Make the move
                pos.do_move (move, si, gives_check);

                if (   mpt == PAWN
                    || pos.capture_type () == PAWN)
                {
                    prefetch (thread->pawn_table[pos.pawn_key ()]);
                }
                if (capture_or_promotion)
                {
                    prefetch (thread->matl_table[pos.matl_key ()]);
                }

                bool full_depth_search;
                // Step 15. Reduced depth search (LMR).
                // If the move fails high will be re-searched at full depth.
                if (   depth > DEPTH_2
                    && move_count > 1
                    && !capture_or_promotion)
                {
                    assert(mtype (move) != PROMOTE);
                    auto reduction_depth = reduction_depths (PVNode, improving, new_depth, move_count);
                    
                    // Increase reduction for cut nodes
                    if (CutNode)
                    {
                        reduction_depth += DEPTH_2;
                    }
                    else
                    {
                        // Decrease reduction for moves that escape a capture in no-cut nodes.
                        if (   mtype (move) == NORMAL
                            && (NIHT <= mpt && mpt <= QUEN)
                            // For reverse move use see() instead of see_sign(), because the destination square is empty for normal move.
                            && pos.see (mk_move (dst, org_sq (move))) < VALUE_ZERO)
                        {
                            reduction_depth -= DEPTH_2;
                        }
                    }

                    // Decrease/Increase reduction for moves with a +ve/-ve history
                    reduction_depth -=
                        Depth((i32(thread->history_values(mpc, dst)
                             +     thread->org_dst_values(~pos.active (), move)
                             + ((ss-1)->counter_move_values != nullptr ? (*(ss-1)->counter_move_values)(mpc, dst) : VALUE_ZERO)
                             + ((ss-2)->counter_move_values != nullptr ? (*(ss-2)->counter_move_values)(mpc, dst) : VALUE_ZERO)
                             + ((ss-4)->counter_move_values != nullptr ? (*(ss-4)->counter_move_values)(mpc, dst) : VALUE_ZERO)) - 10000)/20000);

                    reduction_depth = std::min (std::max (reduction_depth, DEPTH_0), new_depth - DEPTH_1);

                    value =
                        gives_check ?
                            -depth_search<false, !CutNode, true > (pos, ss+1, -(alfa+1), -alfa, new_depth - reduction_depth) :
                            -depth_search<false, !CutNode, false> (pos, ss+1, -(alfa+1), -alfa, new_depth - reduction_depth);

                    full_depth_search = alfa < value
                                     && reduction_depth > DEPTH_0;

                    // Before going to full depth, check whether a fail high with half the reduction
                    i08 i = 0;
                    while (   i < 2
                           && full_depth_search
                           && new_depth >= 8*i32(pow (4, i))
                           && new_depth <= 2*i32(pow (2, i))*reduction_depth)
                    {
                        reduction_depth = reduction_depth / 2;
                        value =
                            gives_check ?
                                -depth_search<false, !CutNode, true > (pos, ss+1, -(alfa+1), -alfa, new_depth - reduction_depth) :
                                -depth_search<false, !CutNode, false> (pos, ss+1, -(alfa+1), -alfa, new_depth - reduction_depth);

                        full_depth_search = alfa < value;
                        ++i;
                    }
                }
                else
                {
                    full_depth_search = !PVNode
                                     || move_count > 1;
                }

                // Step 16. Full depth search when LMR is skipped or fails high
                if (full_depth_search)
                {
                    value =
                        new_depth <= DEPTH_0 ?
                            gives_check ?
                                -quien_search<false, true > (pos, ss+1, -(alfa+1), -alfa, DEPTH_0) :
                                -quien_search<false, false> (pos, ss+1, -(alfa+1), -alfa, DEPTH_0) :
                            gives_check ?
                                -depth_search<false, !CutNode, true > (pos, ss+1, -(alfa+1), -alfa, new_depth) :
                                -depth_search<false, !CutNode, false> (pos, ss+1, -(alfa+1), -alfa, new_depth);
                }

                // Do a full PV search on:
                // - 'full depth move count' move
                // - 'fail high' move (search only if value < beta)
                // otherwise let the parent node fail low with alfa >= value and try another move.
                if (   PVNode
                    && (   move_count == 1
                        || (   alfa < value
                            && (   root_node
                                || value < beta))))
                {
                    (ss+1)->pv.clear ();

                    value =
                        new_depth <= DEPTH_0 ?
                            gives_check ?
                                -quien_search<true, true > (pos, ss+1, -beta, -alfa, DEPTH_0) :
                                -quien_search<true, false> (pos, ss+1, -beta, -alfa, DEPTH_0) :
                            gives_check ?
                                -depth_search<true, false, true > (pos, ss+1, -beta, -alfa, new_depth) :
                                -depth_search<true, false, false> (pos, ss+1, -beta, -alfa, new_depth);
                }

                // Step 17. Undo move
                pos.undo_move ();

                assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);

                // Step 18. Check for the new best move
                // Finished searching the move. If a stop or a cutoff occurred,
                // the return value of the search cannot be trusted,
                // and return immediately without updating best move, PV and TT.
                if (ForceStop.load (memory_order_relaxed))
                {
                    return VALUE_ZERO;
                }

                if (root_node)
                {
                    auto &root_move = *std::find (thread->root_moves.begin (), thread->root_moves.end (), move);
                    // First PV legal move or new best move ?
                    if (   move_count == 1
                        || alfa < value)
                    {
                        root_move.resize (1);
                        auto &pv = (ss+1)->pv;
                        if (!pv.empty ())
                        {
                            root_move.reserve (pv.size () + 1);
                            std::copy (pv.begin (), pv.end (), std::back_inserter (root_move));
                        }
                        root_move.new_value = value;

                        // Record how often the best move has been changed in each iteration.
                        // This information is used for time management:
                        // When the best move changes frequently, allocate some more time.
                        if (   move_count > 1
                            && Limits.use_time_management ()
                            && Threadpool.main_thread () == thread)
                        {
                            Threadpool.best_move_change++;
                        }
                    }
                    else
                    {
                        // All other moves but the PV are set to the lowest value, this
                        // is not a problem when sorting becuase sort is stable and move
                        // position in the list is preserved, just the PV is pushed up.
                        root_move.new_value = -VALUE_INFINITE;
                    }
                }

                // Step 19. Check best value
                if (best_value < value)
                {
                    best_value = value;

                    if (alfa < value)
                    {
                        // If there is an unstable easy move for this position, clear it.
                        if (   PVNode
                            && Limits.use_time_management ()
                            && Threadpool.main_thread () == thread
                            && Threadpool.move_mgr.easy_move (pos.posi_key ()) != MOVE_NONE
                            && (   Threadpool.move_mgr.easy_move (pos.posi_key ()) != move
                                || move_count > 1))
                        {
                            Threadpool.move_mgr.clear ();
                        }

                        best_move = move;

                        // Update pv even in fail-high case
                        if (   PVNode
                            && !root_node)
                        {
                            update_pv (ss->pv, move, (ss+1)->pv);
                        }
                        // Fail high
                        if (value >= beta)
                        {
                            break;
                        }

                        assert(value < beta);
                        // Update alfa! Always alfa < beta
                        if (PVNode)
                        {
                            alfa = value;
                        }
                    }
                }

                if (   move != best_move
                    && !capture_or_promotion)
                {
                    quiet_moves.push_back (move);
                }
            }

            // Step 20. Check for checkmate and stalemate
            // If all possible moves have been searched and if there are no legal moves,
            // If in a singular extension search then return a fail low score (alfa).
            // Otherwise it must be a checkmate or a stalemate, so return value accordingly.
            if (move_count == 0)
            {
                assert(ss->current_move == MOVE_NONE);
                best_value =
                    exclude_move != MOVE_NONE ?
                        alfa :
                        InCheck ?
                            mated_in (ss->ply) :
                            DrawValue[pos.active ()];
            }
            else
            // Quiet best move: update killers, history, countermoves and countermoves history
            if (   best_move != MOVE_NONE
                && !pos.capture_or_promotion (best_move))
            {
                update_stats (pos, ss, best_move, depth, quiet_moves);
            }
            else
            // Bonus for prior countermove that caused the fail low
            if (   depth > DEPTH_2
                && best_move == MOVE_NONE
                && (ss-1)->counter_move_values != nullptr
                && pos.capture_type () == NONE)
            {
                auto opp_move_dst = dst_sq ((ss-1)->current_move);
                auto bonus = Value(i32(depth)*(i32(depth) + 2) - 2);
                if ((ss-2)->counter_move_values != nullptr)
                {
                    (ss-2)->counter_move_values->update (pos[opp_move_dst], opp_move_dst, bonus);
                }
                if ((ss-3)->counter_move_values != nullptr)
                {
                    (ss-3)->counter_move_values->update (pos[opp_move_dst], opp_move_dst, bonus);
                }
                if ((ss-5)->counter_move_values != nullptr)
                {
                    (ss-5)->counter_move_values->update (pos[opp_move_dst], opp_move_dst, bonus);
                }
            }

            tte->save (posi_key,
                       best_move,
                       value_to_tt (best_value, ss->ply),
                       ss->static_eval,
                       depth,
                       best_value >= beta ?
                           BOUND_LOWER :
                              PVNode
                           && best_move != MOVE_NONE ?
                               BOUND_EXACT :
                               BOUND_UPPER,
                       TT.generation ());

            assert(-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);
            return best_value;
        }
    }

    // perft<>() is utility to verify move generation.
    // All the leaf nodes up to the given depth are generated, and the sum is returned.
    template<bool RootNode>
    u64 perft (Position &pos, Depth depth)
    {
        u64 leaf_nodes = 0;
        for (const auto &vm : MoveList<LEGAL> (pos))
        {
            u64 inter_nodes;
            if (   RootNode
                && depth <= DEPTH_1)
            {
                inter_nodes = 1;
            }
            else
            {
                StateInfo si;
                pos.do_move (vm.move, si, pos.gives_check (vm.move, CheckInfo (pos)));
                inter_nodes =
                    depth > DEPTH_2 ?
                        perft<false> (pos, depth - DEPTH_1) :
                        MoveList<LEGAL> (pos).size ();
                pos.undo_move ();
            }

            if (RootNode)
            {
                sync_cout
                    << std::left
                    << std::setw ( 7)
                    << 
                        //move_to_can (vm.move)
                        move_to_san (vm.move, pos)
                    << std::right << std::setfill ('.')
                    << std::setw (16) << inter_nodes
                    << std::setfill (' ') << std::left
                    << sync_endl;
            }

            leaf_nodes += inter_nodes;
        }
        return leaf_nodes;
    }
    // Explicit template instantiations
    template u64 perft<false> (Position&, Depth);
    template u64 perft<true > (Position&, Depth);

    // Initialize various lookup tables during startup
    void initialize ()
    {
        for (i16 d = 0; d < DEPTH_3; ++d)
        {
            RazorMargins[d] = Value(d != 0 ? 64*d + 474 : 570);
        }
        for (i16 d = 0; d < DEPTH_16; ++d)
        {
            FutilityMoveCounts[0][d] = u08(0.773 * pow (d + 0.00, 1.8) + 2.40);
            FutilityMoveCounts[1][d] = u08(1.045 * pow (d + 0.49, 1.8) + 2.90);
        }
        for (u08 imp = 0; imp < 2; ++imp)
        {
            for (i16 d = 1; d < DEPTH_64; ++d)
            {
                for (u08 mc = 1; mc < ReductionMoveCount; ++mc)
                {
                    auto r = log (d) * log (mc) / 2;
                    if (r >= 0.80)
                    {
                        ReductionDepths[0][imp][d][mc] = Depth(i32(std::round (r)));
                        ReductionDepths[1][imp][d][mc] = std::max (ReductionDepths[0][imp][d][mc] - DEPTH_1, DEPTH_0);
                        // If evaluation is not improving increase reduction for non-pv
                        if (   imp == 0
                            && ReductionDepths[0][imp][d][mc] >= DEPTH_2)
                        {
                            ReductionDepths[0][imp][d][mc] += DEPTH_1;
                        }
                    }
                }
            }
        }
    }
    // Resets search state to zero, to obtain reproducible results
    void clear ()
    {
        TT.clear ();
        CounterMoveHistoryValues.clear ();

        for (auto *th : Threadpool)
        {
            th->history_values.clear ();
            th->org_dst_values.clear ();
            th->counter_moves.clear ();
        }
        if (Limits.use_time_management ())
        {
            Threadpool.move_mgr.clear ();
            Threadpool.last_value = VALUE_NONE;
        }
    }
}

namespace Threading {

    using namespace Searcher;

    // Main iterative deepening loop function.
    // It calls depth_search() repeatedly with increasing depth until
    // - the allocated thinking time has been consumed.
    // - the user stops the search.
    // - the maximum search depth is reached.
    void Thread::search ()
    {
        Stack stacks[MaxPlies + 7]; // To allow referencing (ss-5) and (ss+2)
        for (auto s = stacks; s < stacks + MaxPlies + 7; ++s)
        {
            s->ply = i16(s - stacks - 4);
            s->current_move = MOVE_NONE;
            s->exclude_move = MOVE_NONE;
            std::fill_n (s->killer_moves, MaxKillers, MOVE_NONE);
            s->static_eval  = VALUE_ZERO;
            s->move_count   = 0;
            s->skip_pruning = false;
            s->counter_move_values = nullptr;
            assert(s->pv.empty ());
        }

        max_ply = 0;
        running_depth  = DEPTH_0;
        finished_depth = DEPTH_0;

        auto best_value = VALUE_ZERO
           , window     = VALUE_ZERO
           , alfa       = -VALUE_INFINITE
           , beta       = +VALUE_INFINITE;

        // Iterative deepening loop until requested to stop or the target depth is reached.
        while (   !ForceStop
               && ++running_depth < DEPTH_MAX
               && (   Limits.depth == DEPTH_0
                   || Threadpool.main_thread ()->running_depth <= Limits.depth))
        {
            if (Threadpool.main_thread () == this)
            {
                if (Limits.use_time_management ())
                {
                    Threadpool.failed_low = false;
                    // Age out PV variability metric
                    Threadpool.best_move_change *= 0.505;
                }
            }
            else
            {
                static const size_t HalfDensityMapSize = 30;
                // Rotating symmetric patterns with increasing skipsize.
                // Set of rows with half bits set to 1 and half to 0.
                // It is used to allocate the search depths across the threads.
                static const vector<bool> HalfDensityMap[HalfDensityMapSize] =
                {
                    { false, true },
                    { true, false },

                    { false, false, true, true },
                    { false, true, true, false },
                    { true, true, false, false },
                    { true, false, false, true },

                    { false, false, false, true, true, true },
                    { false, false, true, true, true, false },
                    { false, true, true, true, false, false },
                    { true, true, true, false, false, false },
                    { true, true, false, false, false, true },
                    { true, false, false, false, true, true },

                    { false, false, false, false, true, true, true, true },
                    { false, false, false, true, true, true, true, false },
                    { false, false, true, true, true, true, false, false },
                    { false, true, true, true, true, false, false, false },
                    { true, true, true, true, false, false, false, false },
                    { true, true, true, false, false, false, false, true },
                    { true, true, false, false, false, false, true, true },
                    { true, false, false, false, false, true, true, true },

                    { false, false, false, false, false, true, true, true, true, true },
                    { false, false, false, false, true, true, true, true, true, false },
                    { false, false, false, true, true, true, true, true, false, false },
                    { false, false, true, true, true, true, true, false, false, false },
                    { false, true, true, true, true, true, false, false, false, false },
                    { true, true, true, true, true, false, false, false, false, false },
                    { true, true, true, true, false, false, false, false, false, true },
                    { true, true, true, false, false, false, false, false, true, true },
                    { true, true, false, false, false, false, false, true, true, true },
                    { true, false, false, false, false, false, true, true, true, true },
                };

                const auto &hdm = HalfDensityMap[(index - 1) % HalfDensityMapSize];
                if (hdm[(u16(running_depth) + root_pos.ply ()) % hdm.size ()])
                {
                    continue;
                }
            }

            // Save the last iteration's scores before first PV line is searched and
            // all the move scores but the (new) PV are set to -VALUE_INFINITE.
            for (auto &rm : root_moves)
            {
                rm.old_value = rm.new_value;
            }

            // MultiPV loop. Perform a full root search for each PV line
            for (pv_index = 0;
                 !ForceStop
              && pv_index < Threadpool.pv_limit;
                 ++pv_index)
            {
                // Reset aspiration window starting size.
                if (running_depth > DEPTH_4)
                {
                    window = 
                        // Fix window
                        Value(18);
                        //// Increasing window
                        //Value(running_depth <= DEPTH_32 ? 16 + (i32(depth)-1)/4 : 24);

                    alfa = std::max (root_moves[pv_index].old_value - window, -VALUE_INFINITE);
                    beta = std::min (root_moves[pv_index].old_value + window, +VALUE_INFINITE);
                }

                // Start with a small aspiration window and, in case of fail high/low,
                // research with bigger window until not failing high/low anymore.
                do {
                    best_value =
                        root_pos.checkers () != 0 ?
                            depth_search<true, false, true > (root_pos, stacks+5, alfa, beta, running_depth) :
                            depth_search<true, false, false> (root_pos, stacks+5, alfa, beta, running_depth);

                    // Bring the best move to the front. It is critical that sorting is
                    // done with a stable algorithm because all the values but the first
                    // and eventually the new best one are set to -VALUE_INFINITE and
                    // want to keep the same order for all the moves but the new PV
                    // that goes to the front. Note that in case of MultiPV search
                    // the already searched PV lines are preserved.
                    std::stable_sort (root_moves.begin () + pv_index, root_moves.end ());

                    // If search has been stopped, break immediately.
                    // Sorting and writing PV back to TT is safe becuase
                    // root moves is still valid, although refers to the previous iteration.
                    if (ForceStop)
                    {
                        break;
                    }

                    // When failing high/low give some update
                    // (without cluttering the UI) before to re-search.
                    if (   Threadpool.main_thread () == this
                        && Threadpool.pv_limit == 1
                        && (   best_value <= alfa
                            || beta <= best_value)
                        && Threadpool.time_mgr.elapsed_time () > 3*MilliSec)
                    {
                        sync_cout << multipv_info (this, alfa, beta) << sync_endl;
                    }

                    // If failing low/high set new bounds, otherwise exit the loop.

                    if (best_value <= alfa)
                    {
                        beta = (alfa + beta) / 2;
                        alfa = std::max (best_value - window, -VALUE_INFINITE);

                        if (Threadpool.main_thread () == this)
                        {
                            if (Limits.use_time_management ())
                            {
                                Threadpool.failed_low = true;
                            }
                            PonderhitStop = false;
                        }
                    }
                    else
                    if (beta <= best_value)
                    {
                        alfa = (alfa + beta) / 2;
                        beta = std::min (best_value + window, +VALUE_INFINITE);
                    }
                    else
                    {
                        break;
                    }

                    window += window / 4 + 5;
                    
                    assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
                } while (true); // alfa < beta

                // Sort the PV lines searched so far and update the GUI
                std::stable_sort (root_moves.begin (), root_moves.begin () + pv_index + 1);

                if (Threadpool.main_thread () == this)
                {
                    if (ForceStop)
                    {
                        auto total_nodes  = Threadpool.nodes ();
                        auto elapsed_time = std::max (Threadpool.time_mgr.elapsed_time (), TimePoint(1));
                        sync_cout
                            << "info"
                            << " nodes " << total_nodes
                            << " time "  << elapsed_time
                            << " nps "   << total_nodes * MilliSec / elapsed_time
                            << sync_endl;
                    }
                    else
                    if (   Threadpool.pv_limit == pv_index + 1
                        || Threadpool.time_mgr.elapsed_time () > 3*MilliSec)
                    {
                        sync_cout << multipv_info (this, alfa, beta) << sync_endl;
                    }
                }
            }

            if (!ForceStop)
            {
                finished_depth = running_depth;
            }

            if (ContemptValue != 0)
            {
                assert(!root_moves.empty ());
                auto valued_contempt = Value(i32(root_moves[0].new_value)/ContemptValue);
                DrawValue[ root_pos.active ()] = BaseContempt[ root_pos.active ()] - valued_contempt;
                DrawValue[~root_pos.active ()] = BaseContempt[~root_pos.active ()] + valued_contempt;
            }

            if (Threadpool.main_thread () == this)
            {
                // If skill level is enabled and can pick move, pick a sub-optimal best move
                if (   Threadpool.skill_mgr.enabled ()
                    && Threadpool.skill_mgr.can_pick (running_depth))
                {
                    assert(!root_moves.empty ());
                    Threadpool.skill_mgr.clear ();
                    Threadpool.skill_mgr.pick_best_move (Threadpool.pv_limit);
                }

                if (OutputStream.is_open ())
                {
                    OutputStream << pretty_pv_info (this) << std::endl;
                }

                if (   !ForceStop
                    && !PonderhitStop)
                {
                    // Stop the search early:
                    bool stop = false;

                    // Have time for the next iteration? Can stop searching now?
                    if (Limits.use_time_management ())
                    {
                        assert(!root_moves.empty ());
                        // Stop the search
                        // -If there is only one legal move available
                        // -If all of the available time has been used
                        // -If matched an easy move from the previous search and just did a fast verification.
                        if (   root_moves.size () == 1
                            || (Threadpool.time_mgr.elapsed_time () > TimePoint(std::round (Threadpool.time_mgr.optimum_time *
                                                                                    // Improving factor
                                                                                    std::min (1.1385,
                                                                                        std::max (0.3646,
                                                                                                  0.5685
                                                                                                + 0.1895 * (Threadpool.failed_low ? 1 : 0)
                                                                                                - 0.0096 * (Threadpool.last_value != VALUE_NONE ? i32(best_value - Threadpool.last_value) : 0))))))
                            || (Threadpool.easy_played =
                                    (   Threadpool.best_move_change < 0.030
                                     && Threadpool.time_mgr.elapsed_time () > TimePoint(std::round (Threadpool.time_mgr.optimum_time *
                                                                                            // Unstable factor
                                                                                            0.1190 * (1.0 + Threadpool.best_move_change)))
                                     && !root_moves[0].empty ()
                                     &&  root_moves[0] == Threadpool.easy_move), Threadpool.easy_played))
                        {
                            stop = true;
                        }

                        if (root_moves[0].size () >= MoveManager::PVSize)
                        {
                            Threadpool.move_mgr.update (root_pos, root_moves[0]);
                        }
                        else
                        {
                            Threadpool.move_mgr.clear ();
                        }
                    }
                    else
                    // Have found a "mate in <x>"?
                    if (   Limits.mate != 0
                        && best_value >= +VALUE_MATE_IN_MAX_PLY
                        && best_value >= +VALUE_MATE - 2*Limits.mate)
                    {
                        stop = true;
                    }

                    if (stop)
                    {
                        // If allowed to ponder do not stop the search now but
                        // keep pondering until GUI sends "ponderhit" or "stop".
                        if (Limits.ponder)
                        {
                            PonderhitStop = true;
                        }
                        else
                        {
                            ForceStop = true;
                        }
                    }
                }
            }
        }
    }
    // Main thread function when receives the UCI 'go' command.
    // It searches from root position and and outputs the "bestmove" and "ponder".
    void MainThread::search ()
    {
        static Book book; // Defined static to initialize the PRNG only once
        assert(this == Threadpool.main_thread ());

        if (   !white_spaces (OutputFile)
            && OutputFile != Empty)
        {
            OutputStream.open (OutputFile, ios_base::out|ios_base::app);
            OutputStream
                << boolalpha
                << "RootPos  : " << root_pos.fen ()                       << '\n'
                << "RootSize : " << root_moves.size ()                    << '\n'
                << "Infinite : " << Limits.infinite                       << '\n'
                << "Ponder   : " << Limits.ponder                         << '\n'
                << "ClockTime: " << Limits.clock[root_pos.active ()].time << '\n'
                << "Increment: " << Limits.clock[root_pos.active ()].inc  << '\n'
                << "MoveTime : " << Limits.movetime                       << '\n'
                << "MovesToGo: " << u16(Limits.movestogo)                 << '\n'
                << " Depth Score    Time       Nodes  PV\n"
                << "-----------------------------------------------------------"
                << noboolalpha << std::endl;
        }

        if (Limits.use_time_management ())
        {
            // Initialize the time manager before searching.
            Threadpool.time_mgr.initialize (root_pos.active (), root_pos.ply ());
        }

        TT.new_generation (root_pos.ply () + 1);

        bool filtering = false;

        if (root_moves.empty ())
        {
            root_moves += RootMove ();

            sync_cout
                << "info"
                << " depth " << 0
                << " score " << to_string (root_pos.checkers () != 0 ? -VALUE_MATE : VALUE_DRAW)
                << " time "  << 0
                << sync_endl;
        }
        else
        {
            // Check if can play with own book.
            if (   OwnBook
                && !BookFile.empty ()
                && (   BookUptoMove == 0
                    || root_pos.move_num () <= BookUptoMove)
                && Limits.mate == 0
                && !Limits.infinite)
            {
                book.open (BookFile, ios_base::in);
                bool found = false;
                auto book_best_move = book.probe_move (root_pos, BookMoveBest);
                if (   book_best_move != MOVE_NONE
                    && std::find (root_moves.begin (), root_moves.end (), book_best_move) != root_moves.end ())
                {
                    found = true;
                    std::swap (root_moves[0], *std::find (root_moves.begin (), root_moves.end (), book_best_move));
                    StateInfo si;
                    root_pos.do_move (book_best_move, si, root_pos.gives_check (book_best_move, CheckInfo (root_pos)));
                    auto book_ponder_move = book.probe_move (root_pos, BookMoveBest);
                    root_moves[0] += book_ponder_move;
                    root_pos.undo_move ();
                }
                book.close ();
                if (found)
                {
                    goto finish;
                }
            }

            i16 timed_contempt = 0;
            i64 diff_time;
            if (   Limits.use_time_management ()
                && ContemptTime != 0
                && (diff_time = i64(  Limits.clock[ root_pos.active ()].time
                                    - Limits.clock[~root_pos.active ()].time)/MilliSec) != 0)
            {
                timed_contempt = i16(diff_time/ContemptTime);
            }

            auto contempt = cp_to_value ((FixedContempt + timed_contempt) / 100.0);
            DrawValue[ root_pos.active ()] = BaseContempt[ root_pos.active ()] = VALUE_DRAW - contempt;
            DrawValue[~root_pos.active ()] = BaseContempt[~root_pos.active ()] = VALUE_DRAW + contempt;

            if (Limits.use_time_management ())
            {
                Threadpool.easy_move = Threadpool.move_mgr.easy_move (root_pos.posi_key ());
                Threadpool.move_mgr.clear ();
                Threadpool.easy_played = false;
                Threadpool.failed_low  = false;
                Threadpool.best_move_change = 0.000;
            }
            if (Threadpool.skill_mgr.enabled ())
            {
                Threadpool.skill_mgr.clear ();
            }

            // Have to play with skill handicap?
            // In this case enable MultiPV search by skill pv size
            // that will use behind the scenes to get a set of possible moves.
            Threadpool.pv_limit = std::min (std::max (MultiPV, u16(Threadpool.skill_mgr.enabled () ? SkillManager::MinSkillPV : 0)), u16(root_moves.size ()));

            filtering = true;

            for (auto *th : Threadpool)
            {
                if (th != this)
                {
                    th->start_searching (false);
                }
            }

            Thread::search (); // Let's start searching !

            // Clear any candidate easy move that wasn't stable for the last search iterations;
            // the second condition prevents consecutive fast moves.
            if (   Limits.use_time_management ()
                && (   Threadpool.easy_played
                    || Threadpool.move_mgr.stable_count < 6))
            {
                Threadpool.move_mgr.clear ();
            }
            // Swap best PV line with the sub-optimal one if skill level is enabled
            if (Threadpool.skill_mgr.enabled ())
            {
                std::swap (root_moves[0], *std::find (root_moves.begin (), root_moves.end (), Threadpool.skill_mgr.pick_best_move (Threadpool.pv_limit)));
            }
        }

    finish:
        if (Limits.use_time_management ())
        {
            // Update the time manager after searching.
            Threadpool.time_mgr.update (root_pos.active ());
        }
        // When reach max depth arrive here even without Force Stop is raised,
        // but if are pondering or in infinite search, according to UCI protocol,
        // shouldn't print the best move before the GUI sends a "stop" or "ponderhit" command.
        // Simply wait here until GUI sends one of those commands (that raise Force Stop).
        if (   !ForceStop
            && (   Limits.infinite
                || Limits.ponder))
        {
            PonderhitStop = true;
            wait_until (ForceStop);
        }

        if (filtering)
        {
            // Stop the threads if not already stopped.
            ForceStop = true;
            // Wait until all threads have finished.
            for (auto *th : Threadpool)
            {
                if (th != this)
                {
                    th->wait_while_searching ();
                }
            }
            // Check if there are deeper thread than main thread.
            if (   Threadpool.pv_limit == 1
                && !Threadpool.easy_played
                //&& Limits.depth == DEPTH_0 // Depth limit search don't use deeper thread
                && !Threadpool.skill_mgr.enabled ())
            {
                auto *const best_thread = Threadpool.best_thread ();
                // If thread is not main thread then copy to main thread.
                if (best_thread != this)
                {
                    pv_index   = best_thread->pv_index;
                    max_ply    = best_thread->max_ply;
                    root_moves = best_thread->root_moves;
                    running_depth  = best_thread->running_depth;
                    finished_depth = best_thread->finished_depth;
                    // Send new PV.
                    sync_cout << multipv_info (this, -VALUE_INFINITE, +VALUE_INFINITE) << sync_endl;
                }
            }
        }

        assert(!root_moves.empty ()
            && !root_moves[0].empty ());

        if (Limits.use_time_management ())
        {
            Threadpool.last_value = root_moves[0].new_value;
        }

        if (OutputStream.is_open ())
        {
            auto total_nodes  = Threadpool.nodes ();
            auto elapsed_time = std::max (Threadpool.time_mgr.elapsed_time (), TimePoint(1));
            OutputStream
                << "Nodes (N)  : " << total_nodes                               << '\n'
                << "Time (ms)  : " << elapsed_time                              << '\n'
                << "Speed (N/s): " << total_nodes*MilliSec / elapsed_time       << '\n'
                << "Hash-full  : " << TT.hash_full ()                           << '\n'
                << "Best Move  : " << move_to_san (root_moves[0][0], root_pos)  << '\n';
            if (   root_moves[0][0] != MOVE_NONE
                && (   root_moves[0].size () > 1
                    || root_moves[0].extract_ponder_move_from_tt (root_pos)))
            {
                StateInfo si;
                root_pos.do_move (root_moves[0][0], si, root_pos.gives_check (root_moves[0][0], CheckInfo (root_pos)));
                OutputStream << "Ponder Move: " << move_to_san (root_moves[0][1], root_pos) << '\n';
                root_pos.undo_move ();
            }
            OutputStream << std::endl;
            OutputStream.close ();
        }
        // Best move could be MOVE_NONE when searching on a stalemate position.
        sync_cout << "bestmove " << move_to_can (root_moves[0][0]);
        if (   root_moves[0][0] != MOVE_NONE
            && (   root_moves[0].size () > 1
                || root_moves[0].extract_ponder_move_from_tt (root_pos)))
        {
            std::cout << " ponder " << move_to_can (root_moves[0][1]);
        }
        std::cout << sync_endl;
    }
}
