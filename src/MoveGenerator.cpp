#include "MoveGenerator.h"

#include <iostream>
#include <sstream>

#include "BitBoard.h"
#include "Notation.h"

namespace {

    /// Generates piece normal move
    template<bool Checks>
    void generatePieceMoves(ValMoves &moves, Position const &pos, Bitboard targets) {
        for (PieceType pt = NIHT; pt <= QUEN; ++pt) {
            for (Square s : pos.squares(pos.activeSide()|pt)) {

                if (Checks
                 && contains(pos.kingBlockers(~pos.activeSide()), s)) {
                    continue;
                }

                Bitboard attacks{ pos.pieceAttacksFrom(pt, s)
                                & targets };

                if (Checks) {
                    attacks &= pos.checks(pt);
                }

                while (0 != attacks) { moves += makeMove<NORMAL>(s, popLSq(attacks)); }
            }
        }
    }

    /// Generates pawn promotion move
    template<GenType GT>
    void generatePromotionMoves(ValMoves &moves, Position const &pos, Bitboard promotion, Direction dir) {
        while (0 != promotion) {
            auto dst{ popLSq(promotion) };
            auto org{ dst - dir };

            if (GenType::CAPTURE == GT
             || GenType::NATURAL == GT
             || GenType::EVASION == GT) {
                moves += makePromoteMove(org, dst, QUEN);
            }
            if (GenType::QUIET == GT
             || GenType::NATURAL == GT
             || GenType::EVASION == GT) {
                moves += makePromoteMove(org, dst, ROOK);
                moves += makePromoteMove(org, dst, BSHP);
                moves += makePromoteMove(org, dst, NIHT);
            }

            if (GenType::QUIET_CHECK == GT) {
                auto ekSq{ pos.square(~pos.activeSide()|KING) };

                if (contains(PieceAttackBB[NIHT][dst], ekSq)) {
                    moves += makePromoteMove(org, dst, NIHT);
                }
                /*
                Bitboard mocc{ pos.pieces() ^ org };

                Bitboard rookAttacks{ attacksBB<ROOK>(dst, mocc) };
                if (contains(rookAttacks, ekSq)) {
                    moves += makePromoteMove(org, dst, ROOK);
                }

                Bitboard bshpAttacks{ attacksBB<BSHP>(dst, mocc) };
                if (contains(bshpAttacks, ekSq)) {
                    moves += makePromoteMove(org, dst, BSHP);
                }
                */
            }
        }
    }
    /// Generates pawn normal move
    template<GenType GT, Color Own>
    void generatePawnMoves(ValMoves &moves, Position const &pos, Bitboard targets) {
        constexpr auto Opp{ ~Own };

        Bitboard empties{ ~pos.pieces() };
        Bitboard enemies{ pos.pieces(Opp) & targets };

        Bitboard pawns{ pos.pieces(Own, PAWN) };
        // Pawns on 7th Rank only
        Bitboard r7Pawns{ pawns & RankBB[relativeRank(Own, RANK_7)] };
        // Pawns not on 7th Rank
        Bitboard rxPawns{ pawns & ~r7Pawns };

        switch (GT) {
        case GenType::NATURAL:
        case GenType::EVASION:
        case GenType::CAPTURE: {
            // Pawn normal and en-passant captures, no promotions
            Bitboard lAttacks{ enemies & pawnLAttackBB<Own>(rxPawns) };
            Bitboard rAttacks{ enemies & pawnRAttackBB<Own>(rxPawns) };

            Bitboard lChecks{ pos.checks(PAWN) };
            Bitboard rChecks{ pos.checks(PAWN) };
            // Pawns which give discovered check
            Bitboard dscPawns{ rxPawns
                             & pos.kingBlockers(Opp) };
            if (0 != dscPawns) {
                lChecks |= enemies & pawnLAttackBB<Own>(dscPawns);
                rChecks |= enemies & pawnRAttackBB<Own>(dscPawns);
            }

            Bitboard lDirChecks = lAttacks & lChecks;
            Bitboard rDirChecks = rAttacks & rChecks;

            while (0 != lDirChecks) { auto dst{ popLSq(lDirChecks) }; moves += makeMove<NORMAL>(dst - PawnLAtt[Own], dst); }
            while (0 != rDirChecks) { auto dst{ popLSq(rDirChecks) }; moves += makeMove<NORMAL>(dst - PawnRAtt[Own], dst); }

            Bitboard lNonChecks = lAttacks & ~lChecks;
            Bitboard rNonChecks = rAttacks & ~rChecks;

            while (0 != lNonChecks) { auto dst{ popLSq(lNonChecks) }; moves += makeMove<NORMAL>(dst - PawnLAtt[Own], dst); }
            while (0 != rNonChecks) { auto dst{ popLSq(rNonChecks) }; moves += makeMove<NORMAL>(dst - PawnRAtt[Own], dst); }

            if (SQ_NONE != pos.epSquare()) {
                assert(RANK_6 == relativeRank(Own, pos.epSquare()));
                Bitboard epPawns{ rxPawns
                                & pos.pawnAttacksFrom(Opp, pos.epSquare()) };

                // If the checking piece is the double pushed pawn and also is in the target.
                // Otherwise this is a discovery check and are forced to do otherwise.
                if (GenType::EVASION == GT
                 && !contains(enemies /*& pos.pieces(PAWN)*/, pos.epSquare() - PawnPush[Own])) {
                    epPawns = 0;
                }
                assert(2 >= popCount(epPawns));
                while (0 != epPawns) { moves += makeMove<ENPASSANT>(popLSq(epPawns), pos.epSquare()); }
            }
        }
            /* fall through */
        case GenType::QUIET:
        case GenType::QUIET_CHECK: {
            // Promotions (queening and under-promotions)
            if (0 != r7Pawns) {
                Bitboard b;

                b = enemies & pawnLAttackBB<Own>(r7Pawns);
                generatePromotionMoves<GT>(moves, pos, b, PawnLAtt[Own]);

                b = enemies & pawnRAttackBB<Own>(r7Pawns);
                generatePromotionMoves<GT>(moves, pos, b, PawnRAtt[Own]);

                b = empties & pawnSglPushBB<Own>(r7Pawns);
                if (GenType::EVASION == GT) {
                    b &= targets;
                }
                generatePromotionMoves<GT>(moves, pos, b, PawnPush[Own]);
            }

            if (GenType::CAPTURE == GT) {
                break;
            }

            // Pawn single-push and double-push, no promotions
            Bitboard pushs1{ empties & pawnSglPushBB<Own>(rxPawns) };
            Bitboard pushs2{ empties & pawnSglPushBB<Own>(pushs1 & RankBB[relativeRank(Own, RANK_3)]) };

            if (GenType::EVASION == GT) {
                pushs1 &= targets;
                pushs2 &= targets;
            }
            if (GenType::QUIET_CHECK == GT) {
                pushs1 &= pos.checks(PAWN);
                pushs2 &= pos.checks(PAWN);
                // Pawns which give discovered check
                // Add pawn pushes which give discovered check.
                // This is possible only if the pawn is not on the same file as the enemy king, because don't generate captures.
                // Note that a possible discovery check promotion has been already generated among captures.
                Bitboard dscPawns{ rxPawns
                                 & pos.kingBlockers(Opp)
                                 & ~fileBB(pos.square(Opp|KING)) };
                if (0 != dscPawns) {
                    Bitboard dscPushs1{ empties & pawnSglPushBB<Own>(dscPawns) };
                    Bitboard dscPushs2{ empties & pawnSglPushBB<Own>(dscPushs1 & RankBB[relativeRank(Own, RANK_3)]) };
                    pushs1 |= dscPushs1;
                    pushs2 |= dscPushs2;
                }
            }

            while (0 != pushs1) { auto dst{ popLSq(pushs1) }; moves += makeMove<NORMAL>(dst - 1 * PawnPush[Own], dst); }
            while (0 != pushs2) { auto dst{ popLSq(pushs2) }; moves += makeMove<NORMAL>(dst - 2 * PawnPush[Own], dst); }
        }
            break;
        }
    }

    /// Generates king normal move
    template<GenType GT>
    void generateKingMoves(ValMoves &moves, Position const &pos, Bitboard targets) {

        auto fkSq{ pos.square( pos.activeSide()|KING) };
        auto ekSq{ pos.square(~pos.activeSide()|KING) };
        Bitboard attacks{  PieceAttackBB[KING][fkSq]
                        &  targets
                        & ~PieceAttackBB[KING][ekSq] };
        while (0 != attacks) { moves += makeMove<NORMAL>(fkSq, popLSq(attacks)); }

        if (GenType::NATURAL == GT
         || GenType::QUIET == GT) {
            if (pos.canCastle(pos.activeSide())) {
                for (CastleSide cs : { CS_KING, CS_QUEN }) {
                    if (pos.castleExpeded(pos.activeSide(), cs)
                     && pos.canCastle(pos.activeSide(), cs)) {
                        moves += makeMove<CASTLE>(fkSq, pos.castleRookSq(pos.activeSide(), cs));
                    }
                }
            }
        }
    }


    /// Generates all pseudo-legal moves of color for targets.
    template<GenType GT>
    void generateMoves(ValMoves &moves, Position const &pos, Bitboard targets) {

        constexpr bool Checks{ GenType::QUIET_CHECK == GT };

        WHITE == pos.activeSide() ?
            generatePawnMoves<GT, WHITE>(moves, pos, targets) :
            generatePawnMoves<GT, BLACK>(moves, pos, targets);

        generatePieceMoves<Checks>(moves, pos, targets);
    }
}

template<GenType GT>
void generate(ValMoves &moves, Position const &pos) {

    assert(0 == pos.checkers());

    static_assert (GenType::NATURAL == GT
                || GenType::CAPTURE == GT
                || GenType::QUIET   == GT, "GT incorrect");

    moves.clear();
    moves.reserve(64 - 32 * (GenType::CAPTURE == GT));
    Bitboard targets =
        GenType::NATURAL == GT ? ~pos.pieces( pos.activeSide()) :
        GenType::CAPTURE == GT ?  pos.pieces(~pos.activeSide()) :
        GenType::QUIET   == GT ? ~pos.pieces() : 0;

    generateMoves<GT>(moves, pos, targets);
    generateKingMoves<GT>(moves, pos, targets);
}

/// Explicit template instantiations
/// --------------------------------
/// generate<NATURAL>     Generates all pseudo-legal captures and non-captures.
template void generate<GenType::NATURAL>(ValMoves&, Position const&);
/// generate<CAPTURE>     Generates all pseudo-legal captures and queen promotions.
template void generate<GenType::CAPTURE>(ValMoves&, Position const&);
/// generate<QUIET>       Generates all pseudo-legal non-captures and underpromotions.
template void generate<GenType::QUIET>(ValMoves&, Position const&);

/// generate<EVASION>     Generates all pseudo-legal check evasions moves.
template<> void generate<GenType::EVASION>(ValMoves &moves, Position const &pos) {

    Bitboard checkers{ pos.checkers() };
    assert(0 != checkers
        && 2 >= popCount(checkers));

    moves.clear();
    moves.reserve(16);
    auto fkSq{ pos.square(pos.activeSide()|KING) };
    // Double-check, only king move can save the day
    if (!moreThanOne(checkers)) {

        // Generates blocking or captures of the checking piece
        auto checkSq{ scanLSq(checkers) };
        Bitboard targets{ betweenBB(checkSq, fkSq) | checkSq };

        generateMoves<GenType::EVASION>(moves, pos, targets);
    }

    Bitboard checks{ PieceAttackBB[KING][pos.square(~pos.activeSide()|KING)] };
    Bitboard checkersEx{  checkers
                       & ~pos.pieces(PAWN) };
    Bitboard mocc{ pos.pieces() ^ fkSq };
    // Squares attacked by slide checkers will remove them from the king evasions
    // so to skip known illegal moves avoiding useless legality check later.
    while (0 != checkersEx) {
        auto checkSq{ popLSq(checkersEx) };
        checks |= attacksBB(pType(pos[checkSq]), checkSq, mocc);
    }
    // Generate evasions for king, capture and non-capture moves
    Bitboard attacks{  PieceAttackBB[KING][fkSq]
                    & ~checks
                    & ~pos.pieces(pos.activeSide()) };
    while (0 != attacks) { moves += makeMove<NORMAL>(fkSq, popLSq(attacks)); }
}

/// generate<QUIET_CHECK> Generates all pseudo-legal non-captures and knight under promotions check giving moves.
template<> void generate<GenType::QUIET_CHECK>(ValMoves &moves, Position const &pos) {
    assert(0 == pos.checkers());
    moves.clear();
    moves.reserve(16);
    Bitboard targets{ ~pos.pieces() };

    // Pawns is excluded, already generated with direct checks
    Bitboard dscBlockersEx{  pos.kingBlockers(~pos.activeSide())
                          & ~pos.pieces(PAWN)
                          &  pos.pieces(pos.activeSide()) };
    assert(0 == (dscBlockersEx & pos.pieces(QUEN)));
    while (0 != dscBlockersEx) {

        auto org{ popLSq(dscBlockersEx) };
        auto mpt{ pType(pos[org]) };

        Bitboard attacks{ pos.pieceAttacksFrom(mpt, org)
                        & targets };

        if (KING == mpt) {
            attacks &= ~PieceAttackBB[QUEN][pos.square(~pos.activeSide()|KING)];
        }

        while (0 != attacks) { moves += makeMove<NORMAL>(org, popLSq(attacks)); }
    }

    generateMoves<GenType::QUIET_CHECK>(moves, pos, targets);
}

/// generate<LEGAL>       Generates all legal moves.
template<> void generate<GenType::LEGAL>(ValMoves &moves, Position const &pos) {

    0 == pos.checkers() ?
        generate<GenType::NATURAL>(moves, pos) :
        generate<GenType::EVASION>(moves, pos);

    Square fkSq = pos.square(pos.activeSide()|KING);
    Bitboard mocc = pos.pieces() ^ fkSq;
    Bitboard enemies = pos.pieces(~pos.activeSide());
    Bitboard pinneds = pos.kingBlockers(pos.activeSide())
                     & pos.pieces(pos.activeSide());

    // Filter illegal moves
    moves.erase(
        std::remove_if(
            moves.begin(), moves.end(),
            [&](ValMove const &vm) {
                return (NORMAL == mType(vm)
                     && fkSq == orgSq(vm)
                     && 0 != (pos.attackersTo(dstSq(vm), mocc) & enemies))
                    || ((contains(pinneds, orgSq(vm))
                      || CASTLE == mType(vm)
                      || ENPASSANT == mType(vm))
                     && !pos.legal(vm));
            }),
        moves.end());
}

void Perft::operator+=(Perft const &perft) {
    any       += perft.any;
    capture   += perft.capture;
    enpassant += perft.enpassant;
    anyCheck  += perft.anyCheck;
    dscCheck  += perft.dscCheck;
    dblCheck  += perft.dblCheck;
    castle    += perft.castle;
    promotion += perft.promotion;
    checkmate += perft.checkmate;
    //stalemate += perft.stalemate;
}
void Perft::operator-=(Perft const &perft) {
    any       -= perft.any;
    capture   -= perft.capture;
    enpassant -= perft.enpassant;
    anyCheck  -= perft.anyCheck;
    dscCheck  -= perft.dscCheck;
    dblCheck  -= perft.dblCheck;
    castle    -= perft.castle;
    promotion -= perft.promotion;
    checkmate -= perft.checkmate;
    //stalemate -= perft.stalemate;
}

void Perft::classify(Position &pos, Move m) {
    if (ENPASSANT == mType(m)
     || contains(pos.pieces(~pos.activeSide()), dstSq(m))) {
        ++capture;
        if (ENPASSANT == mType(m)) {
            ++enpassant;
        }
    }
    if (pos.giveCheck(m)) {
        ++anyCheck;
        if (!contains(pos.checks(PROMOTE != mType(m) ? pType(pos[orgSq(m)]) : promoteType(m)), dstSq(m))) {
            auto ekSq = pos.square(~pos.activeSide()|KING);
            if (contains(pos.kingBlockers(~pos.activeSide()), orgSq(m))
             && !aligned(orgSq(m), dstSq(m), ekSq)) {
                ++dscCheck;
            }
            else
            if (ENPASSANT == mType(m)) {
                auto epSq{ makeSquare(sFile(dstSq(m)), sRank(orgSq(m))) };
                Bitboard mocc{ (pos.pieces() ^ orgSq(m) ^ epSq) | dstSq(m) };
                if (0 != (pos.pieces(pos.activeSide(), BSHP, QUEN) & attacksBB<BSHP>(ekSq, mocc))
                 || 0 != (pos.pieces(pos.activeSide(), ROOK, QUEN) & attacksBB<ROOK>(ekSq, mocc))) {
                    ++dscCheck;
                }
            }
        }
        StateInfo si;
        pos.doMove(m, si, true);
        assert(0 != pos.checkers()
            && 2 >= popCount(pos.checkers()));
        if (moreThanOne(pos.checkers())) {
            ++dblCheck;
        }
        if (0 == MoveList<GenType::LEGAL>(pos).size()) {
            ++checkmate;
        }
        pos.undoMove(m);
    }
    //else {
    //    StateInfo si;
    //    pos.doMove(m, si, false);
    //    if (0 == MoveList<GenType::LEGAL>(pos).size()) {
    //        ++stalemate;
    //    }
    //    pos.undoMove(m);
    //}
    if (CASTLE == mType(m)) {
        ++castle;
    }
    if (PROMOTE == mType(m)) {
        ++promotion;
    }
}

/// perft() is utility to verify move generation.
/// All the leaf nodes up to the given depth are generated, and the sum is returned.
template<bool RootNode>
Perft perft(Position &pos, Depth depth, bool detail) {
    Perft sumLeaf;
    if (RootNode) {
        std::ostringstream oss;
        oss << std::left
            << std::setw( 3) << "N"
            << std::setw(10) << "Move"
            << std::setw(19) << "Any";
        if (detail) {
            oss << std::setw(17) << "Capture"
                << std::setw(15) << "Enpassant"
                << std::setw(17) << "AnyCheck"
                << std::setw(15) << "DscCheck"
                << std::setw(15) << "DblCheck"
                << std::setw(15) << "Castle"
                << std::setw(15) << "Promote"
                << std::setw(15) << "Checkmate"
                //<< std::setw(15) << "Stalemate"
                ;
        }
        std::cout << oss.str() << std::endl;
    }
    for (auto const &vm : MoveList<GenType::LEGAL>(pos)) {
        Perft leaf;
        if (RootNode
         && DEPTH_ONE >= depth) {
            ++leaf.any;
            if (detail) {
                leaf.classify(pos, vm);
            }
        }
        else {
            StateInfo si;
            pos.doMove(vm, si);

            if (2*DEPTH_ONE >= depth) {
                for (auto &ivm : MoveList<GenType::LEGAL>(pos)) {
                    ++leaf.any;
                    if (detail) {
                        leaf.classify(pos, ivm);
                    }
                }
            }
            else {
                leaf = perft<false>(pos, depth - 1, detail);
            }

            pos.undoMove(vm);
        }
        sumLeaf += leaf;

        if (RootNode) {
            ++sumLeaf.moves;

            std::ostringstream oss;
            oss << std::right << std::setfill('0') << std::setw( 2) << sumLeaf.moves << " "
                << std::left  << std::setfill(' ') << std::setw( 7) << //moveToCAN(vm)
                                                                       moveToSAN(vm, pos)
                << std::right << std::setfill('.') << std::setw(16) << leaf.any;
            if (detail) {
                oss << "   " << std::setw(14) << leaf.capture
                    << "   " << std::setw(12) << leaf.enpassant
                    << "   " << std::setw(14) << leaf.anyCheck
                    << "   " << std::setw(12) << leaf.dscCheck
                    << "   " << std::setw(12) << leaf.dblCheck
                    << "   " << std::setw(12) << leaf.castle
                    << "   " << std::setw(12) << leaf.promotion
                    << "   " << std::setw(12) << leaf.checkmate
                    //<< "   " << std::setw(12) << leaf.stalemate
                    ;
            }
            std::cout << oss.str() << std::endl;
        }
    }
    if (RootNode) {
        std::ostringstream oss;
        oss << "\nTotal:  " << std::right << std::setfill('.')
            << std::setw(18) << sumLeaf.any;
        if (detail) {
            oss << " " << std::setw(16) << sumLeaf.capture
                << " " << std::setw(14) << sumLeaf.enpassant
                << " " << std::setw(16) << sumLeaf.anyCheck
                << " " << std::setw(14) << sumLeaf.dscCheck
                << " " << std::setw(14) << sumLeaf.dblCheck
                << " " << std::setw(14) << sumLeaf.castle
                << " " << std::setw(14) << sumLeaf.promotion
                << " " << std::setw(14) << sumLeaf.checkmate
                //<< " " << std::setw(14) << sumLeaf.stalemate
                ;
        }
        std::cout << oss.str() << std::endl;
    }
    return sumLeaf;
}
/// Explicit template instantiations
/// --------------------------------
template Perft perft<true >(Position&, Depth, bool);
template Perft perft<false>(Position&, Depth, bool);
