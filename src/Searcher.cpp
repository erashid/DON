#include "Searcher.h"

#include <cmath>

#include "Debugger.h"
#include "Evaluator.h"
#include "Helper.h"
#include "Logger.h"
#include "MoveGenerator.h"
#include "MovePicker.h"
#include "Notation.h"
#include "Polyglot.h"
#include "Position.h"
#include "SyzygyTB.h"
#include "Thread.h"
#include "ThreadMarker.h"
#include "TimeManager.h"
#include "Transposition.h"
#include "SkillManager.h"
#include "UCI.h"
#include "Zobrist.h"

/// Pre-loads the given address in L1/L2 cache.
/// This is a non-blocking function that doesn't stall the CPU
/// waiting for data to be loaded from memory, which can be quite slow.
#if defined(PREFETCH)

#   if defined(_MSC_VER) || defined(__INTEL_COMPILER)
#       include <xmmintrin.h> // Microsoft and Intel Header for _mm_prefetch()
#   endif

inline void prefetch(void const *addr) {

#if defined(_MSC_VER) || defined(__INTEL_COMPILER)

#   if defined(__INTEL_COMPILER)

    // This hack prevents prefetches from being optimized away by
    // Intel compiler. Both MSVC and gcc seem not be affected by this.
    __asm__("");

#   endif

    _mm_prefetch((char const*) (addr), _MM_HINT_T0);

#else

    __builtin_prefetch(addr);

#endif

}

#else

inline void prefetch(void const*) {}

#endif // (PREFETCH)

using Evaluator::evaluate;

Limit Limits;

u16  PVCount;

namespace SyzygyTB {

    Depth   DepthLimit;
    i16     PieceLimit;
    bool    Move50Rule;
    bool    HasRoot;
}

namespace {

    /// Stack keeps the information of the nodes in the tree during the search.
    struct Stack {

        i16   ply;
        Move  playedMove{ MOVE_NONE };
        Move  excludedMove{ MOVE_NONE };
        u08   moveCount{ 0 };
        Value staticEval{ VALUE_ZERO };
        i32   stats{ 0 };
        PieceSquareStatsTable *pieceStats;

        Array<Move, 2> killerMoves;
        Moves pv;
    };

    constexpr u64 TTHitAverageWindow{ 4096 };
    constexpr u64 TTHitAverageResolution{ 1024 };

    /// Futility Move Count
    constexpr i16 futilityMoveCount(Depth d, bool imp) {
        return (4 + nSqr(d)) / (2 - imp);
    }

    Array<double, 256> cacheLog{};
    double memoizeLog(i32 x) {
        if (x == 0
         || x == 1) return 0;
        if (//x < cacheLog.size() &&
            cacheLog[x] == 0.0) {
            cacheLog[x] = std::log(x);
        }
        return cacheLog[x];
    }

    Depth reduction(Depth d, u08 mc, bool imp) {
        assert(d >= DEPTH_ZERO);
        auto r{ Threadpool.reductionFactor * memoizeLog(d) * memoizeLog(mc) };
        return Depth((r + 511) / 1024 + (!imp && (r > 1007)));
    }

    /// Add a small random component to draw evaluations to avoid 3-fold-blindness
    Value drawValue(Thread const *th) {
        return VALUE_DRAW + Value(2 * (th->nodes & 1) - 1);
    }

    /// valueToTT() adjusts a mate or TB score from "plies to mate from the root" to
    /// "plies to mate from the current position". standard scores are unchanged.
    constexpr Value valueToTT(Value v, i32 ply) {
        return v >= +VALUE_MATE_2_MAX_PLY ? v + ply :
               v <= -VALUE_MATE_2_MAX_PLY ? v - ply : v;
    }

    /// valueOfTT() adjusts a mate or TB score from the transposition table
    /// (which refers to the plies to mate/be mated from current position)
    /// to "plies to mate/be mated (TB win/loss) from the root".
    /// However, for mate scores, to avoid potentially false mate scores related to the 50 moves rule,
    /// and the graph history interaction, return an optimal TB score instead.
    inline Value valueOfTT(Value v, i32 ply, i32 clockPly) {

        if (v != VALUE_NONE) {

            if (v >= +VALUE_MATE_2_MAX_PLY) // TB win or better
            {
                return v >= +VALUE_MATE_1_MAX_PLY
                    && VALUE_MATE - v >= 100 - clockPly ?
                        // do not return a potentially false mate score
                        +VALUE_MATE_1_MAX_PLY - 1 : v - ply;
            }
            if (v <= -VALUE_MATE_2_MAX_PLY) // TB loss or worse
            {
                return v <= -VALUE_MATE_1_MAX_PLY
                    && VALUE_MATE + v >= 100 - clockPly ?
                        // do not return a potentially false mate score
                        -VALUE_MATE_1_MAX_PLY + 1 : v + ply;
            }
        }
        return v;
    }

    /// statBonus() is the bonus, based on depth
    constexpr i32 statBonus(Depth depth) {
        return depth < 16 ? (19 * depth + 155) * depth - 132 : -8;
    }

    /// updateContinuationStats() updates Stats of the move pairs formed
    /// by moves at ply -1, -2, -4 and -6 with current move.
    void updateContinuationStats(Stack *ss, Piece p, Square dst, i32 bonus) {
        //assert(isOk(p));
        for (auto i : { 1, 2, 4, 6 }) {
            if (isOk((ss-i)->playedMove)) {
                (*(ss-i)->pieceStats)[p][dst] << bonus;
            }
        }
    }

    /// updateQuietStats() updates move sorting heuristics when a new quiet best move is found
    void updateQuietStats(
        Stack *ss,
        Thread *th,
        Position const &pos,
        Color activeSide,
        Move move,
        i32 bonus) {

        th->butterFlyStats[activeSide][mMask(move)] << bonus;
        updateContinuationStats(ss, pos[orgSq(move)], dstSq(move), bonus);
    }

    void updateQuietStatsRefutationMoves(
        Stack *ss,
        Thread *th,
        Position const &pos,
        Color activeSide,
        Move move,
        i32 bonus, Depth depth,
        bool pmOK, Piece pmPiece, Square pmDst) {

        updateQuietStats(ss, th, pos, activeSide, move, bonus);

        if (pType(pos[orgSq(move)]) > PAWN) {
            th->butterFlyStats[activeSide][mMask(reverseMove(move))] << -bonus;
        }

        if (depth > 12
            //&& ss->ply >= 0
            && ss->ply < MAX_LOWPLY) {
            th->lowPlyStats[ss->ply][mMask(move)] << statBonus(depth - 7);
        }

        // Refutation Moves
        if (ss->killerMoves[0] != move) {
            ss->killerMoves[1] = ss->killerMoves[0];
            ss->killerMoves[0] = move;
        }

        if (pmOK) {
            th->counterMoves[pmPiece][pmDst] = move;
        }
    }

    /// updatePV() appends the move and child pv
    void updatePV(Moves &pv, Move move, Moves const &childPV) {
        pv.resize(1);
        pv[0] = move;
        pv.insert(pv.end(), childPV.begin(), childPV.end());

        //assert(pv.front() == move
        //    && ((pv.size() == 1 && childPV.empty())
        //     || (pv.back() == childPV.back())));
    }

    /// multipvInfo() formats PV information according to UCI protocol.
    /// UCI requires that all (if any) un-searched PV lines are sent using a previous search score.
    std::string multipvInfo(Thread const *th, Depth depth, Value alfa, Value beta) {
        auto elapsed{ TimeMgr.elapsed() + 1 };
        auto nodes{ Threadpool.sum(&Thread::nodes) };
        auto tbHits{ Threadpool.sum(&Thread::tbHits)
                   + th->rootMoves.size() * SyzygyTB::HasRoot };

        std::ostringstream oss;
        for (u16 i = 0; i < PVCount; ++i)
        {
            bool updated{ th->rootMoves[i].newValue != -VALUE_INFINITE };
            if (depth == DEPTH_ONE
             && !updated) {
                continue;
            }

            auto v{ updated ?
                    th->rootMoves[i].newValue :
                    th->rootMoves[i].oldValue };

            bool tb{ SyzygyTB::HasRoot
                  && abs(v) < +VALUE_MATE_1_MAX_PLY };
            v = tb ? th->rootMoves[i].tbValue : v;

            //if (oss.rdbuf()->in_avail()) // Not at first line
            //    oss << "\n";
            oss << std::setfill('0')
                << "info"
                << " depth "    << std::setw(2) << (updated ? depth : depth - DEPTH_ONE)
                << " seldepth " << std::setw(2) << th->rootMoves[i].selDepth
                << " multipv "  << i + 1
                << std::setfill(' ')
                << " score "    << v;
            if (!tb && i == th->pvCur) {
            oss << (beta <= v ? " lowerbound" :
                    v <= alfa ? " upperbound" : "");
            }
            oss << " nodes "    << nodes
                << " time "     << elapsed
                << " nps "      << nodes * 1000 / elapsed
                << " tbhits "   << tbHits;
            if (elapsed > 1000) {
            oss << " hashfull " << TT.hashFull();
            }
            oss << " pv "       << th->rootMoves[i];
            if (i + 1 < PVCount) {
                oss << "\n";
            }
        }
        return oss.str();
    }


    /// quienSearch() is quiescence search function, which is called by the main depth limited search function when the remaining depth <= 0.
    template<bool PVNode>
    Value quienSearch(Position &pos, Stack *ss, Value alfa, Value beta, Depth depth = DEPTH_ZERO) {
        assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
        assert(PVNode || (alfa == beta-1));
        assert(depth <= DEPTH_ZERO);

        Value actualAlfa;

        if (PVNode) {
            actualAlfa = alfa; // To flag BOUND_EXACT when eval above alpha and no available moves
            ss->pv.clear();
        }

        bool inCheck{ pos.checkers() != 0 };

        // Check for immediate draw or maximum ply reached.
        if (pos.draw(ss->ply)
         || ss->ply >= MAX_PLY) {
            return !inCheck
                && ss->ply >= MAX_PLY ?
                    evaluate(pos) : VALUE_DRAW;
        }

        assert(ss->ply >= 1
            && ss->ply == (ss-1)->ply + 1
            && ss->ply < MAX_PLY);
        assert(ss->excludedMove == MOVE_NONE);

        Move move;
        // Transposition table lookup.
        Key key{ pos.posiKey() };
        bool ttHit;
        auto *tte   { TT.probe(key, ttHit) };

        auto ttMove { ttHit ?
                        tte->move() : MOVE_NONE };
        auto ttValue{ ttHit ?
                        valueOfTT(tte->value(), ss->ply, pos.clockPly()) : VALUE_NONE };
        //auto ttPV   { ttHit && tte->pv() };

        // Decide whether or not to include checks.
        // Fixes also the type of TT entry depth that are going to use.
        // Note that in quienSearch use only 2 types of depth: DEPTH_QS_CHECK or DEPTH_QS_NO_CHECK.
        auto qsDepth{ inCheck
                    || depth >= DEPTH_QS_CHECK ?
                        DEPTH_QS_CHECK : DEPTH_QS_NO_CHECK };

        if (!PVNode
         && ttHit
         && qsDepth <= tte->depth()
         && ttValue != VALUE_NONE
         && (ttValue >= beta ? tte->bound() & BOUND_LOWER :
                               tte->bound() & BOUND_UPPER)) {
            return ttValue;
        }

        if (ttMove != MOVE_NONE
         && !pos.pseudoLegal(ttMove)) {
            ttMove = MOVE_NONE;
        }


        Value bestValue
            , futilityBase;

        // Evaluate the position statically.
        if (inCheck) {
            ss->staticEval = VALUE_NONE;
            // Starting from the worst case which is checkmate
            bestValue = futilityBase = -VALUE_INFINITE;
        }
        else {
            if (ttHit) {
                // Never assume anything on values stored in TT.
                if ((ss->staticEval = bestValue = tte->eval()) == VALUE_NONE) {
                    ss->staticEval = bestValue = evaluate(pos);
                }

                // Can ttValue be used as a better position evaluation?
                if (ttValue != VALUE_NONE
                 && (tte->bound()
                   & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER))) {
                    bestValue = ttValue;
                }
            }
            else {
                ss->staticEval = bestValue =
                    (ss-1)->playedMove != MOVE_NULL ?
                        evaluate(pos) :
                        -(ss-1)->staticEval + 2 * VALUE_TEMPO;
            }

            if (alfa < bestValue) {
                // Stand pat. Return immediately if static value is at least beta
                if (bestValue >= beta) {
                    if (!ttHit) {
                        tte->save(key,
                                  MOVE_NONE,
                                  valueToTT(bestValue, ss->ply),
                                  ss->staticEval,
                                  DEPTH_NONE,
                                  BOUND_LOWER,
                                  false);
                    }

                    assert(-VALUE_INFINITE < bestValue && bestValue < +VALUE_INFINITE);
                    return bestValue;
                }

                // Update alfa! Always alfa < beta
                if (PVNode) {
                    alfa = bestValue;
                }
            }

            futilityBase = bestValue + 154;
        }

        auto *thread{ pos.thread() };

        auto bestMove{ MOVE_NONE };
        auto activeSide{ pos.activeSide() };

        PieceSquareStatsTable const *pieceStats[]
        {
            (ss-1)->pieceStats, (ss-2)->pieceStats,
            nullptr           , (ss-4)->pieceStats,
            nullptr           , (ss-6)->pieceStats
        };

        // Initialize move-picker(2) for the current position
        MovePicker movePicker{
            pos,
            &thread->butterFlyStats,
            &thread->captureStats,
            pieceStats,
            ttMove, depth, depth <= DEPTH_QS_RECAP ? dstSq((ss-1)->playedMove) : SQ_NONE };

        u08 moveCount{ 0 };
        StateInfo si;
        // Loop through all the pseudo-legal moves until no moves remain or a beta cutoff occurs
        while ((move = movePicker.nextMove()) != MOVE_NONE) {
            assert(isOk(move)
                && (inCheck || pos.pseudoLegal(move)));

            ++moveCount;

            auto org{ orgSq(move) };
            auto dst{ dstSq(move) };
            auto mp{ pos[org] };
            bool giveCheck{ pos.giveCheck(move) };
            bool captureOrPromotion{ pos.captureOrPromotion(move) };

            if (inCheck) {
                // Pruning: Don't search moves with negative SEE
                // Evasion Prunable: Detect non-capture evasions that are candidates to be pruned
                if (((depth < DEPTH_QS_CHECK
                   || moveCount > 2)
                  && bestValue > -VALUE_MATE_2_MAX_PLY
                  && !pos.capture(move))
                 && Limits.mate == 0
                 && !pos.see(move)) {
                    continue;
                }
            }
            else {
                // Futility pruning
                if (!giveCheck
                 && futilityBase > -VALUE_KNOWN_WIN
                 && !(pType(mp) == PAWN
                   && pos.pawnAdvanceAt(activeSide, org))
                 && Limits.mate == 0) {
                    assert(mType(move) != ENPASSANT); // Due to !pos.pawnAdvanceAt
                    // Futility pruning parent node
                    auto futilityValue{ futilityBase
                                      + PieceValues[EG][pType(pos[dst])]
                                      //+ PieceValues[EG][CASTLE != mType(move) ? pType(pos[dst]) : NONE]
                                        };
                    if (futilityValue <= alfa) {
                        if (bestValue < futilityValue) {
                            bestValue = futilityValue;
                        }
                        continue;
                    }
                    // Prune moves with negative or zero SEE
                    if (futilityBase <= alfa
                     && !pos.see(move, VALUE_ZERO + 1)) {
                        if (bestValue < futilityBase) {
                            bestValue = futilityBase;
                        }
                        continue;
                    }
                }

                // Pruning: Don't search moves with negative SEE
                if (Limits.mate == 0
                 && !pos.see(move)) {
                    continue;
                }
            }

            // Check for legality just before making the move
            if (!pos.legal(move)) {
                --moveCount;
                continue;
            }

            // Speculative prefetch as early as possible
            prefetch(TT.cluster(pos.movePosiKey(move))->entryTable);

            // Update the current move
            ss->playedMove = move;
            ss->pieceStats = &thread->continuationStats[inCheck][captureOrPromotion][mp][dst];
            // Do the move
            pos.doMove(move, si, giveCheck);
            auto value{ -quienSearch<PVNode>(pos, ss+1, -beta, -alfa, depth - DEPTH_ONE) };
            // Undo the move
            pos.undoMove(move);

            assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);

            // Check for new best move.
            if (bestValue < value) {
                bestValue = value;

                if (alfa < value) {
                    bestMove = move;

                    if (PVNode) { // Update pv even in fail-high case
                        updatePV(ss->pv, move, (ss+1)->pv);
                    }

                    if (value >= beta) { // Fail high
                        break;
                    }
                    if (PVNode) { // Update alfa! Always alfa < beta
                        alfa = value;
                    }
                }
            }
        }

        // All legal moves have been searched.
        // A special case: If in check and no legal moves were found, it is checkmate.
        if (inCheck
         && bestValue == -VALUE_INFINITE) {
            return matedIn(ss->ply); // Plies to mate from the root
        }

        tte->save(key,
                  bestMove,
                  valueToTT(bestValue, ss->ply),
                  ss->staticEval,
                  qsDepth,
                  bestValue >= beta ? BOUND_LOWER :
                      PVNode
                   && bestValue > actualAlfa ? BOUND_EXACT : BOUND_UPPER,
                  ttHit && tte->pv());

        assert(-VALUE_INFINITE < bestValue && bestValue < +VALUE_INFINITE);
        return bestValue;
    }
    /// depthSearch() is main depth limited search function, which is called when the remaining depth > 0.
    template<bool PVNode>
    Value depthSearch(Position &pos, Stack *ss, Value alfa, Value beta, Depth depth, bool cutNode) {
        bool rootNode = PVNode
                     && ss->ply == 0;

        auto *thread{ pos.thread() };

        // Check if there exists a move which draws by repetition,
        // or an alternative earlier move to this position.
        if (!rootNode
         && alfa < VALUE_DRAW
         && pos.clockPly() >= 3
         && pos.cycled(ss->ply)) {

            alfa = drawValue(thread);
            if (alfa >= beta) {
                return alfa;
            }
        }

        // Dive into quiescence search when the depth reaches zero
        if (depth <= DEPTH_ZERO) {
            return quienSearch<PVNode>(pos, ss, alfa, beta);
        }

        assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
        assert(PVNode || (alfa == beta-1));
        assert(!(PVNode && cutNode));
        assert(DEPTH_ZERO < depth && depth < MAX_PLY);

        // Step 1. Initialize node
        ss->moveCount = 0;

        // Check for the available remaining limit
        if (thread == Threadpool.mainThread()) {
            static_cast<MainThread*>(thread)->doTick();
        }

        if (PVNode) {
            // Used to send selDepth info to GUI (selDepth from 1, ply from 0)
            if (thread->selDepth < ss->ply + 1) {
                thread->selDepth = ss->ply + 1;
            }
        }

        bool inCheck{ pos.checkers() != 0 };

        if (!rootNode)
        {
            // Step 2. Check for aborted search, immediate draw or maximum ply reached.
            if (Threadpool.stop.load(std::memory_order::memory_order_relaxed)
             || pos.draw(ss->ply)
             || ss->ply >= MAX_PLY) {
                return !inCheck
                    && ss->ply >= MAX_PLY ?
                        evaluate(pos) : drawValue(thread);
            }

            // Step 3. Mate distance pruning.
            // Even if mate at the next move our score would be at best matesIn(ss->ply+1),
            // but if alfa is already bigger because a shorter mate was found upward in the tree
            // then there is no need to search further, will never beat current alfa.
            // Same logic but with reversed signs applies also in the opposite condition of
            // being mated instead of giving mate, in this case return a fail-high score.
            alfa = std::max(matedIn(ss->ply+0), alfa);
            beta = std::min(matesIn(ss->ply+1), beta);
            if (alfa >= beta) {
                return alfa;
            }
        }

        Value value;
        auto bestValue{ -VALUE_INFINITE };
        auto maxValue{ +VALUE_INFINITE };

        auto bestMove{ MOVE_NONE };

        assert(ss->ply >= 0
            && ss->ply == (ss-1)->ply + 1
            && ss->ply < MAX_PLY);

        assert((ss+1)->excludedMove == MOVE_NONE);
        (ss+2)->killerMoves.fill(MOVE_NONE);

        // Initialize stats to zero for the grandchildren of the current position.
        // So stats is shared between all grandchildren and only the first grandchild starts with stats = 0.
        // Later grandchildren start with the last calculated stats of the previous grandchild.
        // This influences the reduction rules in LMR which are based on the stats of parent position.
        (ss+2 + 2 * rootNode)->stats = 0;

        Move move;
        Move excludedMove{ ss->excludedMove };

        // Step 4. Transposition table lookup.
        // Don't want the score of a partial search to overwrite
        // a previous full search TT value, so we use a different
        // position key in case of an excluded move.
        Key key{ pos.posiKey()
               ^ Key(excludedMove << 16) };
        bool ttHit;
        auto *tte   { excludedMove == MOVE_NONE ?
                        TT.probe(key, ttHit) :
                        TTEx.probe(key, ttHit) };
        auto ttMove { rootNode ?
                        thread->rootMoves[thread->pvCur][0] :
                        ttHit ?
                            tte->move() : MOVE_NONE };
        auto ttValue{ ttHit ?
                        valueOfTT(tte->value(), ss->ply, pos.clockPly()) : VALUE_NONE };
        auto ttPV   { PVNode
                   || (ttHit && tte->pv()) };

        auto activeSide{ pos.activeSide() };

        bool pmOK       { isOk((ss-1)->playedMove) };
        auto pmDst      { dstSq((ss-1)->playedMove) };
        auto pmPiece    { CASTLE != mType((ss-1)->playedMove) ? pos[pmDst] : ~activeSide|KING };
        bool pmCapOrPro { pos.captured() != NONE
                       || pos.promoted() };

        if (ttPV
         && depth > 12
         &&  pmOK
         && !pmCapOrPro
         //&& (ss-1)->ply >= 0
         && (ss-1)->ply < MAX_LOWPLY) {
            thread->lowPlyStats[(ss-1)->ply][mMask((ss-1)->playedMove)] << statBonus(depth - 5);
        }

        // ttHitAvg can be used to approximate the running average of ttHit
        thread->ttHitAvg = (TTHitAverageWindow - 1) * thread->ttHitAvg / TTHitAverageWindow
                         + TTHitAverageResolution * ttHit;

        // At non-PV nodes we check for an early TT cutoff
        if (!PVNode
         && ttHit
         && depth <= tte->depth()
         && ttValue != VALUE_NONE
         && (ttValue >= beta ? tte->bound() & BOUND_LOWER :
                               tte->bound() & BOUND_UPPER)) {
            if (ttMove != MOVE_NONE
             && contains(pos.pieces(activeSide), orgSq(ttMove))) {
                // Update move sorting heuristics on ttMove
                if (!pos.captureOrPromotion(ttMove)) {
                    auto bonus{ statBonus(depth) };
                    // Bonus for a quiet ttMove that fails high
                    if (ttValue >= beta) {
                        updateQuietStatsRefutationMoves(ss, thread, pos, activeSide, ttMove, bonus, depth, pmOK, pmPiece, pmDst);
                    }
                    // Penalty for a quiet ttMove that fails low
                    else {
                        updateQuietStats(ss, thread, pos, activeSide, ttMove, -bonus);
                    }
                }

                // Extra penalty for early quiet moves in previous ply when it gets refuted
                if (ttValue >= beta
                 &&  pmOK
                 && !pmCapOrPro
                 && (ss-1)->moveCount <= 2) {
                    updateContinuationStats(ss-1, pmPiece, pmDst, -statBonus(depth + 1));
                }
            }

            if (pos.clockPly() < 90) {
                return ttValue;
            }
        }

        // Step 5. Tablebases probe.
        if (!rootNode
         && SyzygyTB::PieceLimit != 0) {
            auto pieceCount{ pos.count() };

            if (( pieceCount < SyzygyTB::PieceLimit
              || (pieceCount == SyzygyTB::PieceLimit
               && depth >= SyzygyTB::DepthLimit))
             && pos.clockPly() == 0
             && pos.castleRights() == CR_NONE) {

                SyzygyTB::ProbeState probeState;
                auto wdlScore{ SyzygyTB::probeWDL(pos, probeState) };

                // Force check of time on the next occasion
                if (thread == Threadpool.mainThread()) {
                    static_cast<MainThread*>(thread)->setTicks(1);
                }

                if (probeState != SyzygyTB::ProbeState::PS_FAILURE) {
                    thread->tbHits.fetch_add(1, std::memory_order::memory_order_relaxed);

                    i16 draw{ SyzygyTB::Move50Rule };

                    value = wdlScore < -draw ? -VALUE_MATE_1_MAX_PLY + (ss->ply + 1) :
                            wdlScore > +draw ? +VALUE_MATE_1_MAX_PLY - (ss->ply + 1) :
                                                VALUE_DRAW + 2 * i32(wdlScore) * draw;

                    auto bound{ wdlScore < -draw ? BOUND_UPPER :
                                wdlScore > +draw ? BOUND_LOWER :
                                                   BOUND_EXACT };

                    if ( bound == BOUND_EXACT
                     || (bound == BOUND_LOWER ? beta <= value : value <= alfa)) {
                        tte->save(key,
                                  MOVE_NONE,
                                  valueToTT(value, ss->ply),
                                  VALUE_NONE,
                                  Depth(std::min(depth + 6, MAX_PLY - 1)),
                                  bound,
                                  ttPV);
                        return value;
                    }

                    if (PVNode) {
                        if (bound == BOUND_LOWER) {
                            bestValue = value;
                            if (alfa < value) {
                                alfa = value;
                            }
                        }
                        else {
                            maxValue = value;
                        }
                    }
                }
            }
        }

        if (!rootNode
         && ttMove != MOVE_NONE
         && !pos.pseudoLegal(ttMove)) {
            ttMove = MOVE_NONE;
        }

        StateInfo si;

        bool improving;
        Value eval;

        // Step 6. Static evaluation of the position
        if (inCheck) {
            ss->staticEval = eval = VALUE_NONE;
            improving = false;
        }
        // Early pruning
        else {
            if (ttHit) {
                // Never assume anything on values stored in TT.
                if ((ss->staticEval = eval = tte->eval()) == VALUE_NONE) {
                    ss->staticEval = eval = evaluate(pos);
                }

                if (eval == VALUE_DRAW) {
                    eval = drawValue(thread);
                }
                // Can ttValue be used as a better position evaluation?
                if (ttValue != VALUE_NONE
                 && (tte->bound()
                   & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER))) {
                    eval = ttValue;
                }
            }
            else {
                ss->staticEval = eval =
                    (ss-1)->playedMove != MOVE_NULL ?
                        evaluate(pos) - (ss-1)->stats / 512 :
                        -(ss-1)->staticEval + 2 * VALUE_TEMPO;

                tte->save(key,
                          MOVE_NONE,
                          VALUE_NONE,
                          eval,
                          DEPTH_NONE,
                          BOUND_NONE,
                          ttPV);
            }

            // Step 7. Razoring (~1 ELO)
            if (!rootNode // The RootNode PV handling is not available in qsearch
             && depth == DEPTH_ONE
             && excludedMove == MOVE_NONE
                // Razor Margin
             && eval <= alfa - 531) {
                return quienSearch<PVNode>(pos, ss, alfa, beta);
            }

            improving = (ss-2)->staticEval != VALUE_NONE ? ss->staticEval > (ss-2)->staticEval :
                        (ss-4)->staticEval != VALUE_NONE ? ss->staticEval > (ss-4)->staticEval : true;

            // Step 8. Futility pruning: child node (~50 ELO)
            // Betting that the opponent doesn't have a move that will reduce
            // the score by more than futility margins if do a null move.
            if (!PVNode
             && depth < 6
                // Futility Margin
             && eval - 217 * (depth - improving) >= beta
             && eval < +VALUE_KNOWN_WIN // Don't return unproven wins.
             && Limits.mate == 0) {
                return eval;
            }

            // Step 9. Null move search with verification search (~40 ELO)
            if (!PVNode
             && eval >= beta
             && (ss-1)->playedMove != MOVE_NULL
             && (ss-1)->stats < 23397
             && eval >= ss->staticEval
             && ss->staticEval >= beta - 32 * depth - 30 * improving + 120 * ttPV + 292
             && pos.nonPawnMaterial(activeSide) != VALUE_ZERO
             && excludedMove == MOVE_NONE
             && (thread->nmpPly <= ss->ply
              || thread->nmpColor != activeSide)
             && Limits.mate == 0) {
                // Null move dynamic reduction based on depth and static evaluation.
                auto nullDepth{ Depth(depth - ((854 + 68 * depth) / 258 + std::min(i32(eval - beta) / 192, 3))) };

                Key nullMoveKey{ key
                               ^ RandZob.side
                               ^ (pos.epSquare() != SQ_NONE ? RandZob.enpassant[sFile(pos.epSquare())] : 0) };

                // Speculative prefetch as early as possible
                prefetch(TT.cluster(nullMoveKey)->entryTable);

                ss->playedMove = MOVE_NULL;
                ss->pieceStats = &thread->continuationStats[0][0][NO_PIECE][0];

                pos.doNullMove(si);

                auto nullValue = -depthSearch<false>(pos, ss+1, -beta, -(beta-1), nullDepth, !cutNode);

                pos.undoNullMove();

                if (nullValue >= beta) {
                    // Skip verification search
                    if (thread->nmpPly != 0 // Recursive verification is not allowed
                     || (depth < 13
                      && abs(beta) < +VALUE_KNOWN_WIN)) {
                        // Don't return unproven wins
                        return nullValue >= +VALUE_MATE_2_MAX_PLY ? beta : nullValue;
                    }

                    // Do verification search at high depths,
                    // with null move pruning disabled for nmpColor until ply exceeds nmpPly
                    thread->nmpColor = activeSide;

                    thread->nmpPly = ss->ply + 3 * nullDepth / 4;
                    value = depthSearch<false>(pos, ss, beta-1, beta, nullDepth, false);
                    thread->nmpPly = 0;

                    if (value >= beta) {
                        // Don't return unproven wins
                        return nullValue >= +VALUE_MATE_2_MAX_PLY ? beta : nullValue;
                    }
                }
            }

            // Step 10. ProbCut. (~10 ELO)
            // If good enough capture and a reduced search returns a value much above beta,
            // then can (almost) safely prune the previous move.
            if (!PVNode
             && depth > 4
             && abs(beta) < +VALUE_MATE_2_MAX_PLY
             && Limits.mate == 0) {
                auto raisedBeta{ std::min(beta + 189 - 45 * improving, +VALUE_INFINITE) };

                u08 probMoveCount{ 0 };
                // Initialize move-picker(3) for the current position
                MovePicker movePicker{
                    pos,
                    &thread->captureStats,
                    ttMove, depth, raisedBeta - ss->staticEval };
                // Loop through all the pseudo-legal moves until no moves remain or a beta cutoff occurs
                while ((move = movePicker.nextMove()) != MOVE_NONE
                    && probMoveCount < (2 + 2 * cutNode)) {
                    assert(isOk(move)
                        && pos.pseudoLegal(move)
                        && pos.captureOrPromotion(move)
                        && CASTLE != mType(move));

                    if (move == excludedMove
                     || !pos.legal(move)) {
                        continue;
                    }

                    ++probMoveCount;

                    // Speculative prefetch as early as possible
                    prefetch(TT.cluster(pos.movePosiKey(move))->entryTable);

                    ss->playedMove = move;
                    // inCheck{ false }, captureOrPromotion{ true }
                    ss->pieceStats = &thread->continuationStats[0][1][pos[orgSq(move)]][dstSq(move)];

                    pos.doMove(move, si);

                    // Perform a preliminary quienSearch to verify that the move holds
                    value = -quienSearch<false>(pos, ss+1, -raisedBeta, -raisedBeta+1);

                    // If the quienSearch held perform the regular search
                    if (value >= raisedBeta) {
                        value = -depthSearch<false>(pos, ss+1, -raisedBeta, -raisedBeta+1, depth - 4, !cutNode);
                    }

                    pos.undoMove(move);

                    if (value >= raisedBeta) {
                        return value;
                    }
                }
            }

            // Step 11. Internal iterative deepening (IID). (~1 ELO)
            if (depth > 6
             && ttMove == MOVE_NONE) {

                depthSearch<PVNode>(pos, ss, alfa, beta, std::max(depth - 7, 1), cutNode);

                tte = excludedMove == MOVE_NONE ?
                        TT.probe(key, ttHit) :
                        TTEx.probe(key, ttHit);
                ttMove = ttHit
                      && (move = tte->move()) != MOVE_NONE
                      && pos.pseudoLegal(move) ?
                            move : MOVE_NONE;
                ttValue = ttHit ?
                            valueOfTT(tte->value(), ss->ply, pos.clockPly()) : VALUE_NONE;
            }
        }

        value = bestValue;

        // Mark this node as being searched.
        ThreadMarker threadMarker{ thread, key, ss->ply };

        bool singularLMR{ false };
        bool moveCountPruning{ false };
        bool ttmCapture{ ttMove != MOVE_NONE
                      && pos.captureOrPromotion(ttMove) };

        PieceSquareStatsTable const *pieceStats[]
        {
            (ss-1)->pieceStats, (ss-2)->pieceStats,
            nullptr           , (ss-4)->pieceStats,
            nullptr           , (ss-6)->pieceStats
        };

        auto counterMove = pos.thread()->counterMoves[pmPiece][pmDst];

        // Initialize move-picker(1) for the current position
        MovePicker movePicker{
            pos,
            &thread->butterFlyStats,
            &thread->lowPlyStats,
            &thread->captureStats,
            pieceStats,
            ttMove, depth,
            depth > 12 ? ss->ply : i16(MAX_PLY),
            ss->killerMoves, counterMove };

        u08 moveCount{ 0 };
        Moves quietMoves;
        quietMoves.reserve(32);
        Moves captureMoves;
        captureMoves.reserve(16);

        // Step 12. Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs.
        while ((move = movePicker.nextMove()) != MOVE_NONE) {
            assert(isOk(move)
                && (inCheck || pos.pseudoLegal(move)));

            // Skip exclusion move
            if (move == excludedMove) {
                continue;
            }

            if (rootNode) {

                assert(ttMove != MOVE_NONE);

                // At root obey the "searchmoves" option and skip moves not listed in
                // RootMove List. As a consequence any illegal move is also skipped.
                // In MultiPV mode we also skip PV moves which have been already searched
                // and those of lower "TB rank" if we are in a TB root position.
                if (!thread->rootMoves.contains(thread->pvCur, thread->pvEnd, move)) {
                    continue;
                }

                if (thread == Threadpool.mainThread()) {
                    auto elapsed{ TimeMgr.elapsed() + 1 };
                    if (elapsed > 3000) {
                        sync_cout << std::setfill('0')
                                  << "info"
                                  << " depth "          << std::setw(2) << depth
                                  << " seldepth "       << std::setw(2) << thread->rootMoves.find(thread->pvCur, thread->pvEnd, move)->selDepth
                                  << " currmove "       << move
                                  << " currmovenumber " << std::setw(2) << thread->pvCur + moveCount + 1
                                  //<< " maxmoves "       << thread->rootMoves.size()
                                  << " time "           << elapsed
                                  << std::setfill(' ')  << sync_endl;
                    }
                }
            }

            ss->moveCount = ++moveCount;

            if (PVNode) {
                (ss+1)->pv.clear();
            }

            auto org{ orgSq(move) };
            auto dst{ dstSq(move) };
            auto mp{ pos[org] };
            bool giveCheck{ pos.giveCheck(move) };
            bool captureOrPromotion{ pos.captureOrPromotion(move) };

            // Calculate new depth for this move
            auto newDepth{ Depth(depth - DEPTH_ONE) };

            // Step 13. Pruning at shallow depth. (~200 ELO)
            if (!rootNode
             && pos.nonPawnMaterial(activeSide) > VALUE_ZERO
             && bestValue > -VALUE_MATE_2_MAX_PLY
             && Limits.mate == 0) {
                // Skip quiet moves if move count exceeds our futilityMoveCount() threshold
                moveCountPruning = (moveCount >= futilityMoveCount(depth, improving));
                movePicker.pickQuiets = !moveCountPruning;

                if (giveCheck
                 || captureOrPromotion) {
                    // SEE based pruning: negative SEE (~25 ELO)
                    if (!pos.see(move, Value(-194 * depth))) {
                        continue;
                    }
                }
                else {
                    // Reduced depth of the next LMR search.
                    i16 lmrDepth = std::max(newDepth - reduction(depth, moveCount, improving), 0);

                    // Counter moves based pruning: (~20 ELO)
                    if (lmrDepth < (4 + ((ss-1)->stats > 0 || (ss-1)->moveCount == 1))
                     && (*pieceStats[0])[mp][dst] < CounterMovePruneThreshold
                     && (*pieceStats[1])[mp][dst] < CounterMovePruneThreshold) {
                        continue;
                    }
                    // Futility pruning: parent node. (~5 ELO)
                    if (!inCheck
                     && lmrDepth < 6
                     && ss->staticEval + 172 * lmrDepth + 235 <= alfa
                     && (*pieceStats[0])[mp][dst]
                      + (*pieceStats[1])[mp][dst]
                      + (*pieceStats[3])[mp][dst] < 27400) {
                        continue;
                    }
                    // SEE based pruning: negative SEE (~20 ELO)
                    if (!pos.see(move, Value(-(32 - std::min(lmrDepth, i16(18))) * nSqr(lmrDepth)))) {
                        continue;
                    }
                }
            }

            // Check for legality just before making the move
            if (!rootNode
             && !pos.legal(move)) {
                ss->moveCount = --moveCount;
                if (move == ttMove) {
                    ttmCapture = false;
                }
                continue;
            }

            // Step 14. Extensions. (~75 ELO)
            auto extension{ DEPTH_ZERO };

            // Singular extension (SE) (~70 ELO)
            // Extend the TT move if its value is much better than its siblings.
            // If all moves but one fail low on a search of (alfa-s, beta-s),
            // and just one fails high on (alfa, beta), then that move is singular and should be extended.
            // To verify this do a reduced search on all the other moves but the ttMove,
            // if result is lower than ttValue minus a margin then extend ttMove.
            if (!rootNode
             && depth > 5
             //&& ttHit
             && move == ttMove
             && excludedMove == MOVE_NONE // Avoid recursive singular search
             // && ttValue != VALUE_NONE  Already implicit in the next condition
             && abs(ttValue) < VALUE_KNOWN_WIN
             && (tte->bound() & BOUND_LOWER)
             && depth < (tte->depth() + 4)) {

                auto singularBeta{ ttValue - ((4 + (!PVNode && ttPV)) * depth) / 2 };
                auto singularDepth{ (depth + 3 * (!PVNode && ttPV) - 1) / 2 };

                ss->excludedMove = move;
                value = depthSearch<false>(pos, ss, singularBeta-1, singularBeta, singularDepth, cutNode);
                ss->excludedMove = MOVE_NONE;

                if (value < singularBeta) {
                    extension = DEPTH_ONE;
                    singularLMR = true;
                }
                // Multi-cut pruning
                // Our ttMove is assumed to fail high, and now failed high also on a reduced
                // search without the ttMove. So assume this expected Cut-node is not singular,
                // multiple moves fail high, and can prune the whole subtree by returning the soft bound.
                else
                if (singularBeta >= beta) {
                    return singularBeta;
                }
            }
            else
            if (// Previous capture extension
                (pos.captured() > PAWN
              && pos.nonPawnMaterial() <= 2 * VALUE_MG_ROOK)
                // Check extension (~2 ELO)
             || (giveCheck
              && (// Discovered check ?
                  contains(pos.kingBlockers(~activeSide), org)
                  // Direct check ?
               || pos.see(move)))
                // Passed pawn extension
             || (ss->killerMoves[0] == move
              && pType(mp) == PAWN
              && pos.pawnAdvanceAt(activeSide, org)
              && pos.pawnPassedAt(activeSide, dst))) {
                extension = DEPTH_ONE;
            }

            // Castle extension
            if (mType(move) == CASTLE) {
                extension = DEPTH_ONE;
            }

            // Add extension to new depth
            newDepth += extension;

            // Speculative prefetch as early as possible
            prefetch(TT.cluster(pos.movePosiKey(move))->entryTable);

            // Update the current move
            ss->playedMove = move;
            ss->pieceStats = &thread->continuationStats[inCheck][captureOrPromotion][mp][dst];

            // Step 15. Do the move
            pos.doMove(move, si, giveCheck);

            bool doLMR{
                depth > 2
             && moveCount > (1 + 2 * rootNode)
             && (!rootNode
                // At root if zero best counter
              || thread->rootMoves.bestCount(thread->pvCur, thread->pvEnd, move) == 0)
             && (!captureOrPromotion
              || moveCountPruning
              || ss->staticEval + PieceValues[EG][pos.captured()] <= alfa
              || cutNode
                // If ttHit running average is small
              || thread->ttHitAvg < 375 * TTHitAverageWindow) };

            bool doFullSearch;
            // Step 16. Reduced depth search (LMR, ~200 ELO).
            // If the move fails high will be re-searched at full depth.
            if (doLMR) {

                auto reductDepth{ reduction(depth, moveCount, improving) };

                reductDepth +=
                    // Increase if other threads are searching this position.
                    +1 * threadMarker.marked
                    // Increase if move count pruning
                    +1 * ((PVNode || !ttPV) && moveCountPruning)
                    // Decrease if the ttHit running average is large
                    -1 * (thread->ttHitAvg > 500 * TTHitAverageWindow)
                    // Decrease if position is or has been on the PV (~10 ELO)
                    -2 * ttPV
                    // Decrease if move has been singularly extended (~3 ELO)
                    -(1 + (!PVNode && ttPV)) * singularLMR
                    // Decrease if opponent's move count is high (~5 ELO)
                    -1 * ((ss-1)->moveCount > 14);

                if (!captureOrPromotion) {
                    // Increase if TT move is a capture (~5 ELO)
                    reductDepth += 1 * ttmCapture;

                    // Increase if cut nodes (~10 ELO)
                    if (cutNode) {
                        reductDepth += 2;
                    }
                    else
                    // Decrease if move escapes a capture in no-cut nodes (~2 ELO)
                    if (mType(move) == SIMPLE
                     && !pos.see(reverseMove(move))) {
                        reductDepth -= 2 + ttPV;
                    }

                    ss->stats =
                          thread->butterFlyStats[activeSide][mMask(move)]
                        + (*pieceStats[0])[mp][dst]
                        + (*pieceStats[1])[mp][dst]
                        + (*pieceStats[3])[mp][dst]
                        - 4926;

                    // Decrease/Increase reduction by comparing opponent's stat score (~10 Elo)
                    reductDepth +=
                        +1 * ((ss-0)->stats <= -155
                           && (ss-1)->stats >= -116)
                        -1 * ((ss-0)->stats >= -102
                           && (ss-1)->stats <= -115);

                    // Decrease/Increase reduction for moves with a good/bad history (~30 Elo)
                    reductDepth -= i16(ss->stats / 16434);
                }
                else {
                    // Increase reduction for captures/promotions at low depth and late move
                    if (depth < 8
                     && moveCount > 2) {
                        reductDepth += 1;
                    }
                }

                auto d{ clamp(Depth(newDepth - reductDepth), DEPTH_ONE, newDepth) };

                value = -depthSearch<false>(pos, ss+1, -(alfa+1), -alfa, d, true);

                doFullSearch = alfa < value
                            && d < newDepth;
            }
            else {
                doFullSearch = !PVNode
                            || moveCount > 1;
            }

            // Step 17. Full depth search when LMR is skipped or fails high.
            if (doFullSearch) {
                value = -depthSearch<false>(pos, ss+1, -(alfa+1), -alfa, newDepth, !cutNode);

                if (doLMR
                 && !captureOrPromotion) {

                    auto bonus{ alfa < value ?
                                +statBonus(newDepth) :
                                -statBonus(newDepth) };
                    if (ss->killerMoves[0] == move) {
                        bonus += bonus / 4;
                    }
                    updateContinuationStats(ss, mp, dst, bonus);
                }
            }

            // Full PV search.
            if (PVNode
             && (moveCount == 1
              || (alfa < value
               && (rootNode
                || value < beta)))) {

                (ss+1)->pv.clear();

                value = -depthSearch<true>(pos, ss+1, -beta, -alfa, newDepth, false);
            }

            // Step 18. Undo the move
            pos.undoMove(move);

            assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);

            // Step 19. Check for the new best move.
            // Finished searching the move. If a stop or a cutoff occurred,
            // the return value of the search cannot be trusted,
            // and return immediately without updating best move, PV and TT.
            if (Threadpool.stop.load(std::memory_order::memory_order_relaxed)) {
                return VALUE_ZERO;
            }

            if (rootNode) {
                assert(thread->rootMoves.contains(move));
                RootMove &rm{ *thread->rootMoves.find(move) };
                // First PV move or new best move?
                if (moveCount == 1
                 || alfa < value) {
                    rm.newValue = value;
                    rm.selDepth = thread->selDepth;
                    rm.resize(1);
                    rm.insert(rm.end(), (ss+1)->pv.begin(), (ss+1)->pv.end());

                    // Record how often the best move has been changed in each iteration.
                    // This information is used for time management:
                    // When the best move changes frequently, allocate some more time.
                    if (moveCount > 1
                     && Limits.useTimeMgmt()) {
                        ++thread->pvChange;
                    }
                }
                else {
                    // All other moves but the PV are set to the lowest value, this
                    // is not a problem when sorting because sort is stable and move
                    // position in the list is preserved, just the PV is pushed up.
                    rm.newValue = -VALUE_INFINITE;
                }
            }

            // Step 20. Check best value.
            if (bestValue < value) {
                bestValue = value;

                if (alfa < value) {
                    bestMove = move;

                    // Update pv even in fail-high case.
                    if (PVNode
                     && !rootNode) {
                        updatePV(ss->pv, move, (ss+1)->pv);
                    }

                    if (value >= beta) { // Fail high
                        ss->stats = 0;
                        break;
                    }
                    if (PVNode) { // Update alfa! Always alfa < beta
                        alfa = value;
                    }
                }
            }

            if (move != bestMove) {
                if (captureOrPromotion) {
                    captureMoves += move;
                }
                else {
                    quietMoves += move;
                }
            }
        }

        // The following condition would detect a stop only after move loop has been
        // completed. But in this case bestValue is valid because we have fully
        // searched our subtree, and we can anyhow save the result in TT.
        /*
        if (Threadpool.stop) {
            return VALUE_DRAW;
        }
        */

        assert(moveCount != 0
            || !inCheck
            || excludedMove != MOVE_NONE
            || MoveList<LEGAL>(pos).size() == 0);

        // Step 21. Check for checkmate and stalemate.
        // If all possible moves have been searched and if there are no legal moves,
        // If in a singular extension search then return a fail low score (alfa).
        // Otherwise it must be a checkmate or a stalemate, so return value accordingly.
        if (moveCount == 0) {
            bestValue =
                excludedMove != MOVE_NONE ?
                    alfa :
                    inCheck ?
                        matedIn(ss->ply) : VALUE_DRAW;
        }
        else
        if (bestMove != MOVE_NONE) {

            auto bonus1{ statBonus(depth + 1) };

            // Quiet best move: update move sorting heuristics.
            if (!pos.captureOrPromotion(bestMove)) {

                auto bonus2{ bestValue > beta + VALUE_MG_PAWN ?
                                bonus1 : statBonus(depth) };

                updateQuietStatsRefutationMoves(ss, thread, pos, activeSide, bestMove, bonus2, depth, pmOK, pmPiece, pmDst);
                // Decrease all the other played quiet moves
                for (auto qm : quietMoves) {
                    updateQuietStats(ss, thread, pos, activeSide, qm, -bonus2);
                }
            }
            else {
                thread->captureStats[pos[orgSq(bestMove)]][dstSq(bestMove)][pos.captured(bestMove)] << bonus1;
            }

            // Decrease all the other played capture moves
            for (auto cm : captureMoves) {
                thread->captureStats[pos[orgSq(cm)]][dstSq(cm)][pos.captured(cm)] << -bonus1;
            }

            // Extra penalty for a quiet TT move or main killer move in previous ply when it gets refuted
            if ( pmOK
             && !pmCapOrPro
             && ((ss-1)->moveCount == 1
              || (ss-1)->killerMoves[0] == (ss-1)->playedMove)) {
                updateContinuationStats(ss-1, pmPiece, pmDst, -bonus1);
            }
        }
        else {
            // Bonus for prior quiet move that caused the fail low.
            if ( pmOK
             && !pmCapOrPro
             && (PVNode
              || depth > 2)) {
                updateContinuationStats(ss-1, pmPiece, pmDst, statBonus(depth));
            }
        }

        if (PVNode
         && bestValue > maxValue) {
            bestValue = maxValue;
        }

        if (excludedMove == MOVE_NONE
         && (!rootNode
          || thread->pvCur == 0)) {
            tte->save(key,
                      bestMove,
                      valueToTT(bestValue, ss->ply),
                      ss->staticEval,
                      depth,
                      bestValue >= beta ?
                          BOUND_LOWER :
                          PVNode
                       && bestMove != MOVE_NONE ?
                              BOUND_EXACT : BOUND_UPPER,
                      ttPV);
        }

        assert(-VALUE_INFINITE < bestValue && bestValue < +VALUE_INFINITE);
        return bestValue;
    }
}

bool Limit::useTimeMgmt() const {
    return !infinite
        && moveTime == 0
        && depth == DEPTH_ZERO
        && nodes == 0
        && mate == 0;
}

void Limit::clear() {
    clock[WHITE].time = 0; clock[WHITE].inc = 0;
    clock[BLACK].time = 0; clock[BLACK].inc = 0;

    movestogo   = 0;
    moveTime    = 0;
    depth       = DEPTH_ZERO;
    nodes       = 0;
    mate        = 0;
    infinite    = false;
    ponder      = false;

    searchMoves.clear();

    startTime   = 0;
}

/// Thread::search() is thread iterative deepening loop function.
/// It calls depthSearch() repeatedly with increasing depth until
/// - Force stop requested.
/// - Allocated thinking time has been consumed.
/// - Maximum search depth is reached.
void Thread::search() {
    ttHitAvg = (TTHitAverageResolution / 2) * TTHitAverageWindow;

    i16 timedContempt{ 0 };
    i32 contemptTime{ Options["Contempt Time"] };
    if (contemptTime != 0
     && Limits.useTimeMgmt()) {
        i64 diffTime{ (i64(Limits.clock[ rootPos.activeSide()].time)
                     - i64(Limits.clock[~rootPos.activeSide()].time)) / 1000 };
        timedContempt = i16(diffTime / contemptTime);
    }
    // Basic Contempt
    i32 bc{ toValue(i16(Options["Fixed Contempt"]) + timedContempt) };
    // In analysis mode, adjust contempt in accordance with user preference
    if (Limits.infinite
     || Options["UCI_AnalyseMode"]) {
        bc = Options["Analysis Contempt"] == "Off"                                    ? 0 :
             Options["Analysis Contempt"] == "White" && rootPos.activeSide() == BLACK ? -bc :
             Options["Analysis Contempt"] == "Black" && rootPos.activeSide() == WHITE ? -bc :
             /*Options["Analysis Contempt"] == "Both"                         ? +bc :*/ +bc;
    }

    contempt = rootPos.activeSide() == WHITE ?
                +makeScore(bc, bc / 2) :
                -makeScore(bc, bc / 2);

    auto *mainThread{ this == Threadpool.mainThread() ?
                        static_cast<MainThread*>(this) : nullptr };

    if (mainThread != nullptr) {
        mainThread->iterValues.fill(mainThread->bestValue);
    }

    i16 iterIdx{ 0 };
    double pvChangeSum{ 0.0 };
    i16 researchCount{ 0 };

    auto bestValue{ -VALUE_INFINITE };
    auto window{ +VALUE_ZERO };
    auto  alfa{ -VALUE_INFINITE }
        , beta{ +VALUE_INFINITE };

    // To allow access to (ss-7) up to (ss+2), the stack must be over-sized.
    // The former is needed to allow updateContinuationStats(ss-1, ...),
    // which accesses its argument at ss-6, also near the root.
    // The latter is needed for stats and killer initialization at ss+2.
    Stack stack[MAX_PLY + 10], *ss;
    for (ss = stack; ss < stack + MAX_PLY + 10; ++ss) {
        ss->ply             = i16(ss - (stack+7));

        bool ssOk = ss->ply >= 0;
        ss->playedMove      = MOVE_NONE;
        ss->excludedMove    = MOVE_NONE;
        ss->moveCount       = 0;
        ss->staticEval      = VALUE_ZERO;
        ss->stats           = 0;
        ss->pieceStats      = ssOk ? nullptr : &this->continuationStats[0][0][NO_PIECE][0];
        ss->killerMoves.fill(MOVE_NONE);
        if (ssOk) {
        //ss->pv.clear();
        ss->pv.reserve(std::max(32 - ss->ply, 4));
        }
    }
    ss = stack + 7;

    // Iterative deepening loop until requested to stop or the target depth is reached.
    while (++rootDepth < MAX_PLY
        && !Threadpool.stop
        && (mainThread == nullptr
         || Limits.depth == DEPTH_ZERO
         || rootDepth <= Limits.depth)) {

        if (mainThread != nullptr
         && Limits.useTimeMgmt()) {
            // Age out PV variability metric
            pvChangeSum *= 0.5;
        }

        // Save the last iteration's values before first PV line is searched and
        // all the move scores except the (new) PV are set to -VALUE_INFINITE.
        rootMoves.saveValues();

        pvBeg = 0;
        pvEnd = 0;

        // MultiPV loop. Perform a full root search for each PV line.
        for (pvCur = 0; pvCur < PVCount && !Threadpool.stop; ++pvCur) {
            if (pvCur == pvEnd) {
                pvBeg = pvEnd;
                while (++pvEnd < rootMoves.size()) {
                    if (rootMoves[pvEnd].tbRank != rootMoves[pvBeg].tbRank) {
                        break;
                    }
                }
            }

            // Reset UCI info selDepth for each depth and each PV line
            selDepth = DEPTH_ZERO;

            // Reset aspiration window starting size.
            if (rootDepth >= 4) {
                window = Value(21);
                auto oldValue{ rootMoves[pvCur].oldValue };
                alfa = std::max(oldValue - window, -VALUE_INFINITE);
                beta = std::min(oldValue + window, +VALUE_INFINITE);

                // Dynamic contempt
                auto dc{ bc };
                i32 contemptValue{ Options["Contempt Value"] };
                if (contemptValue != 0) {
                    dc += ((102 - bc / 2) * oldValue * 100) / ((abs(oldValue) + 157) * contemptValue);
                }
                contempt = rootPos.activeSide() == WHITE ?
                            +makeScore(dc, dc / 2) :
                            -makeScore(dc, dc / 2);
            }

            if (Threadpool.research) {
                ++researchCount;
            }

            i16 failHighCount{ 0 };

            // Start with a small aspiration window and, in case of fail high/low,
            // research with bigger window until not failing high/low anymore.
            do {
                auto adjustedDepth{ Depth(std::max(rootDepth - failHighCount - researchCount, 1)) };
                bestValue = depthSearch<true>(rootPos, ss, alfa, beta, adjustedDepth, false);

                // Bring the best move to the front. It is critical that sorting is
                // done with a stable algorithm because all the values but the first
                // and eventually the new best one are set to -VALUE_INFINITE and
                // want to keep the same order for all the moves but the new PV
                // that goes to the front. Note that in case of MultiPV search
                // the already searched PV lines are preserved.
                rootMoves.stableSort(pvCur, pvEnd);

                // If search has been stopped, break immediately.
                // Sorting is safe because RootMoves is still valid, although it refers to the previous iteration.
                if (Threadpool.stop) {
                    break;
                }

                // Give some update before to re-search.
                if (mainThread != nullptr
                 && PVCount == 1
                 && (bestValue <= alfa
                  || beta <= bestValue)
                 && 3000 < TimeMgr.elapsed()) {

                    sync_cout << multipvInfo(mainThread, rootDepth, alfa, beta) << sync_endl;
                }

                // If fail low set new bounds.
                if (bestValue <= alfa) {
                    beta = (alfa + beta) / 2;
                    alfa = std::max(bestValue - window, -VALUE_INFINITE);

                    failHighCount = 0;
                    if (mainThread != nullptr) {
                        mainThread->stopOnPonderhit = false;
                    }
                }
                else
                // If fail high set new bounds.
                if (beta <= bestValue) {
                    // NOTE:: Don't change alfa = (alfa + beta) / 2
                    beta = std::min(bestValue + window, +VALUE_INFINITE);

                    ++failHighCount;
                }
                // Otherwise exit the loop.
                else {
                    ++rootMoves[pvCur].bestCount;
                    break;
                }

                window += window / 4 + 5;

                assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
            } while (true);

            // Sort the PV lines searched so far and update the GUI.
            rootMoves.stableSort(pvBeg, pvCur + 1);

            if (mainThread != nullptr
             && (Threadpool.stop
              || PVCount - 1 == pvCur
              || 3000 < TimeMgr.elapsed())) {
                sync_cout << multipvInfo(mainThread, rootDepth, alfa, beta) << sync_endl;
            }
        }

        if (Threadpool.stop) {
            break;
        }

        finishedDepth = rootDepth;

        // Has any of the threads found a "mate in <x>"?
        if (Limits.mate != 0
         && !Limits.useTimeMgmt()
         && bestValue >= +VALUE_MATE_1_MAX_PLY
         && bestValue >= +VALUE_MATE - 2 * Limits.mate) {
            Threadpool.stop = true;
        }

        if (mainThread != nullptr) {
            // If skill level is enabled and can pick move, pick a sub-optimal best move.
            if (SkillMgr.enabled()
             && SkillMgr.canPick(rootDepth)) {
                SkillMgr.clear();
                SkillMgr.pickBestMove();
            }

            if (Limits.useTimeMgmt()
             && !Threadpool.stop
             && !mainThread->stopOnPonderhit) {

                if (mainThread->bestMove != rootMoves[0][0]) {
                    mainThread->bestMove = rootMoves[0][0];
                    mainThread->bestDepth = rootDepth;
                }

                // Reduce time if the bestMove is stable over 10 iterations
                // Time Reduction factor
                double timeReduction{ 0.91 + 1.03 * (9 < finishedDepth - mainThread->bestDepth) };
                // Reduction factor - Use part of the gained time from a previous stable move for the current move
                double reduction{ (1.41 + mainThread->timeReduction) / (2.27 * timeReduction) };
                // Eval Falling factor
                double evalFalling{ clamp((332
                                         + 6 * (mainThread->bestValue * i32(mainThread->bestValue != +VALUE_INFINITE) - bestValue)
                                         + 6 * (mainThread->iterValues[iterIdx] * i32(mainThread->iterValues[iterIdx] != +VALUE_INFINITE) - bestValue)) / 704.0,
                                          0.50, 1.50) };

                pvChangeSum += Threadpool.sum(&Thread::pvChange);
                // Reset pv change
                Threadpool.reset(&Thread::pvChange);

                double pvInstability{ 1.00 + pvChangeSum / Threadpool.size() };

                auto availableTime{ TimePoint(TimeMgr.optimum()
                                            * reduction
                                            * evalFalling
                                            * pvInstability) };
                auto elapsed{ TimeMgr.elapsed() + 1 };

                // Stop the search
                // - If all of the available time has been used
                // - If there is less than 2 legal move available
                if (elapsed > availableTime * (1 < rootMoves.size())) {
                    // If allowed to ponder do not stop the search now but
                    // keep pondering until GUI sends "stop"/"ponderhit".
                    if (!mainThread->ponder) {
                        Threadpool.stop = true;
                    }
                    else {
                        mainThread->stopOnPonderhit = true;
                    }
                }
                else
                if (elapsed > availableTime * 0.60) {
                    if (!mainThread->ponder) {
                        Threadpool.research = true;
                    }
                }

                mainThread->timeReduction = timeReduction;

                mainThread->iterValues[iterIdx] = bestValue;
                iterIdx = (iterIdx + 1) % 4;
            }
        }
    }
}

/// MainThread::search() is main thread search function.
/// It searches from root position and outputs the "bestmove"/"ponder".
void MainThread::search() {
    assert(Threadpool.mainThread() == this);

    if (Limits.useTimeMgmt()) {
        // Initialize the time manager before searching.
        TimeMgr.setup(rootPos.activeSide(), rootPos.gamePly());
    }

    TEntry::Generation += 8;

    bool think{ true };

    if (rootMoves.empty()) {
        think = false;

        rootMoves += MOVE_NONE;

        sync_cout << "info"
                  << " depth " << 0
                  << " score " << toString(rootPos.checkers() != 0 ? -VALUE_MATE : VALUE_DRAW)
                  << " time "  << 0 << sync_endl;
    }
    else {
        if (!Limits.infinite
         && Limits.mate == 0
         && Options["Use Book"]) {
            auto bbm{ Book.probe(rootPos, Options["Book Move Num"], Options["Book Pick Best"]) };
            if (bbm != MOVE_NONE
             && rootMoves.contains(bbm)) {
                think = false;

                rootMoves.bringToFront(bbm);
                rootMoves[0].newValue = VALUE_NONE;
                StateInfo si;
                rootPos.doMove(bbm, si);
                auto bpm{ Book.probe(rootPos, Options["Book Move Num"], Options["Book Pick Best"]) };
                if (bpm != MOVE_NONE) {
                    rootMoves[0] += bpm;
                }
                rootPos.undoMove(bbm);
            }
        }

        if (think) {

            if (Limits.useTimeMgmt()) {
                bestMove = MOVE_NONE;
                bestDepth = DEPTH_ZERO;
            }

            auto level = Options["UCI_LimitStrength"] ?
                            clamp(u16(std::pow((double(Options["UCI_Elo"]) - 1346.6) / 143.4, 1.240)), {0}, MaxLevel) :
                            u16(Options["Skill Level"]);
            SkillMgr.setLevel(level);

            // Have to play with skill handicap?
            // In this case enable MultiPV search by skill pv size
            // that will use behind the scenes to get a set of possible moves.
            PVCount = clamp(u16(Options["MultiPV"]),
                            u16(1 + 3 * SkillMgr.enabled()),
                            u16(rootMoves.size()));

            for (auto *th : Threadpool) {
                if (th != this) {
                    th->wakeUp();
                }
            }
            Thread::search(); // Let's start searching !

            // Swap best PV line with the sub-optimal one if skill level is enabled
            if (SkillMgr.enabled()) {
                rootMoves.bringToFront(SkillMgr.pickBestMove());
            }
        }
    }

    // When reach the maximum depth, can arrive here without a raise of Threads.stop.
    // However, if in an infinite search or pondering, shouldn't print the best move
    // before receiving a "stop"/"ponderhit" command. Therefore simply wait here until
    // receives one of those commands (which also raises Threads.stop).
    // Busy wait for a "stop"/"ponderhit" command.
    while (!Threadpool.stop
        && (ponder
         || Limits.infinite))
    {} // Busy wait for a stop or a ponder reset

    Thread *bestThread{ this };
    if (think) {
        // Stop the threads if not already stopped (Also raise the stop if "ponderhit" just reset Threads.ponder).
        Threadpool.stop = true;
        // Wait until all threads have finished.
        for (auto *th : Threadpool) {
            if (th != this) {
                th->waitIdle();
            }
        }
        // Check if there is better thread than main thread.
        if (PVCount == 1
         && Limits.depth == DEPTH_ZERO // Depth limit search don't use deeper thread
         && !SkillMgr.enabled()
         && !Options["UCI_LimitStrength"]
         && rootMoves[0][0] != MOVE_NONE) {

            bestThread = Threadpool.bestThread();
            // If new best thread then send PV info again.
            if (bestThread != this) {
                sync_cout << multipvInfo(bestThread, bestThread->finishedDepth, -VALUE_INFINITE, +VALUE_INFINITE) << sync_endl;
            }
        }
    }

    assert(!bestThread->rootMoves.empty()
        && !bestThread->rootMoves[0].empty());

    auto &rm{ bestThread->rootMoves[0] };

    if (Limits.useTimeMgmt()) {
        if (TimeMgr.timeNodes() != 0) {
            TimeMgr.updateNodes(rootPos.activeSide());
        }

        bestValue = rm.newValue;
    }

    auto bm{ rm[0] };
    auto pm{ MOVE_NONE };
    if (bm != MOVE_NONE) {
        auto itr{ rm.begin() + 1 };
        pm = itr != rm.end() ?
            *itr : TT.extractNextMove(rootPos, bm);
        assert(bm != pm);
    }

    // Best move could be MOVE_NONE when searching on a stalemate position.
    sync_cout << "bestmove " << bm;
    if (pm != MOVE_NONE) {
        std::cout << " ponder " << pm;
    }
    std::cout << sync_endl;
}

/// MainThread::doTick() is used as timer function.
/// Used to detect when out of available limit and thus stop the search, also print debug info.
void MainThread::doTick() {
    static TimePoint InfoTime{ now() };

    if (--_ticks > 0) {
        return;
    }
    // When using nodes, ensure checking rate is in range [1, 1024]
    setTicks(i16(Limits.nodes != 0 ? clamp(i32(Limits.nodes / 1024), 1, 1024) : 1024));

    auto elapsed{ TimeMgr.elapsed() };
    auto time = Limits.startTime + elapsed;

    if (InfoTime + 1000 <= time) {
        InfoTime = time;

        Debugger::print();
    }

    // Do not stop until told so by the GUI.
    if (ponder) {
        return;
    }

    if ((Limits.useTimeMgmt()
      && (stopOnPonderhit
       || TimeMgr.maximum() < elapsed + 10))
     || (Limits.moveTime != 0
      && Limits.moveTime <= elapsed)
     || (Limits.nodes != 0
      && Limits.nodes <= Threadpool.sum(&Thread::nodes))) {
        Threadpool.stop = true;
    }
}

namespace SyzygyTB {

    void rankRootMoves(Position &pos, RootMoves &rootMoves) {

        DepthLimit = Depth(Options["SyzygyDepthLimit"]);
        PieceLimit = Options["SyzygyPieceLimit"];
        Move50Rule = Options["SyzygyMove50Rule"];
        HasRoot    = false;

        bool dtzAvailable{ true };

        // Tables with fewer pieces than SyzygyProbeLimit are searched with DepthLimit == DEPTH_ZERO
        if (PieceLimit > MaxPieceLimit) {
            PieceLimit = MaxPieceLimit;
            DepthLimit = DEPTH_ZERO;
        }

        // Rank moves using DTZ tables
        if (PieceLimit != 0
         && PieceLimit >= pos.count()
         && pos.castleRights() == CR_NONE) {
            // If the current root position is in the table-bases,
            // then RootMoves contains only moves that preserve the draw or the win.
            HasRoot = rootProbeDTZ(pos, rootMoves);
            if (!HasRoot) {
                // DTZ tables are missing; try to rank moves using WDL tables
                dtzAvailable = false;
                HasRoot = rootProbeWDL(pos, rootMoves);
            }
        }

        if (HasRoot) {
            // Sort moves according to TB rank
            std::sort(rootMoves.begin(), rootMoves.end(),
                [](RootMove const &rm1, RootMove const &rm2) {
                    return rm1.tbRank > rm2.tbRank;
                });
            // Probe during search only if DTZ is not available and winning
            if (dtzAvailable
             || rootMoves[0].tbValue <= VALUE_DRAW) {
                PieceLimit = 0;
            }
        }
        else {
            // Clean up if rootProbeDTZ() and rootProbeWDL() have failed
            for (auto &rm : rootMoves) {
                rm.tbRank = 0;
            }
        }
    }

}
