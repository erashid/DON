#include "Notation.h"

#include "MoveGenerator.h"
#include "Position.h"

using namespace std;

namespace Notation {

    using namespace BitBoard;
    using namespace MoveGen;
    using namespace Threading;

    namespace {

        // Type of the Ambiguity
        enum AmbiguityType : u08
        {
            AMB_NONE,
            AMB_RANK,
            AMB_FILE,
            AMB_SQR,
        };

        // Ambiguity if more then one piece of same type can reach 'dst' with a legal move.
        // NOTE: for pawns it is not needed because 'org' file is explicit.
        AmbiguityType ambiguity (Move m, const Position &pos)
        {
            assert(pos.legal (m));

            auto org = org_sq (m);
            auto dst = dst_sq (m);
            auto mpt = ptype (pos[org]);
            // Disambiguation if have more then one piece with destination 'dst'
            // note that for pawns is not needed because starting file is explicit.
            Bitboard attacks = 0;
            switch (mpt)
            {
            case NIHT: attacks = PieceAttacks[NIHT][dst];               break;
            case BSHP: attacks = attacks_bb<BSHP> (dst, pos.pieces ()); break;
            case ROOK: attacks = attacks_bb<ROOK> (dst, pos.pieces ()); break;
            case QUEN: attacks = attacks_bb<QUEN> (dst, pos.pieces ()); break;
            default: assert(false); break;
            }

            Bitboard amb = (attacks & pos.pieces (pos.active, mpt)) ^ org;
            Bitboard pcs = amb; // & ~pos.abs_blockers (pos.active); // If pinned piece is considered as ambiguous
            while (pcs != 0)
            {
                auto sq = pop_lsq (pcs);
                if (!pos.legal (mk_move<NORMAL> (sq, dst)))
                {
                    amb ^= sq;
                }
            }
            if (amb != 0)
            {
                if ((amb & file_bb (org)) == 0) return AMB_RANK;
                if ((amb & rank_bb (org)) == 0) return AMB_FILE;
                return AMB_SQR;
            }
            return AMB_NONE;
        }

        // Value to string
        string pretty_value (Value v, Color c)
        {
            ostringstream oss;
            if (abs (v) < +VALUE_MATE - i32(MaxPlies))
            {
                oss << std::setprecision (2) << std::fixed << std::showpos << value_to_cp (c == WHITE ? +v : -v);
            }
            else
            {
                oss << "#" << std::showpos << i32(v > VALUE_ZERO ?
                                                    +(VALUE_MATE - v + 1) :
                                                    -(VALUE_MATE + v + 0)) / 2;
            }
            return oss.str ();
        }
        // Time to string
        string pretty_time (TimePoint time)
        {
            const u32 MilliSec       = 1000;
            const u32 MinuteMilliSec = 60*MilliSec;
            const u32 HourMilliSec   = 60*MinuteMilliSec;

            u32 hours  = u32(time / HourMilliSec);
            time      %= HourMilliSec;
            u32 minutes= u32(time / MinuteMilliSec);
            time      %= MinuteMilliSec;
            u32 seconds= u32(time / MilliSec);
            time      %= MilliSec;
            time      /= 10;

            ostringstream oss;
            oss << std::setfill ('0')
                << std::setw (2) << hours   << ":"
                << std::setw (2) << minutes << ":"
                << std::setw (2) << seconds << "."
                << std::setw (2) << time
                << std::setfill (' ');
            return oss.str ();
        }
    }

    // Converts a move to a string in coordinate algebraic notation representation.
    // The only special case is castling moves,
    //  - e1g1 notation in normal chess mode,
    //  - e1h1 notation in chess960 mode.
    // Internally castle moves are always coded as "king captures rook".
    string move_to_can (Move m)
    {
        if (MOVE_NONE == m) return "(none)";
        if (MOVE_NULL == m) return "(null)";

        auto can = to_string (org_sq (m))
                 + to_string (fix_dst_sq (m));
        if (mtype (m) == PROMOTE)
        {
            can += char(tolower (PieceChar[promote (m)]));
        }
        return can;
    }
    // Converts a string representing a move in coordinate algebraic notation
    // to the corresponding legal move, if any.
    Move move_from_can (string &can, const Position &pos)
    {
        if (   can.length () == 5
            && isupper (can[4]))
        {
            can[4] = char(tolower (can[4])); // Promotion piece in lowercase
        }
        for (const auto &vm : MoveList<GenType::LEGAL> (pos))
        {
            if (can == move_to_can (vm.move))
            {
                return vm.move;
            }
        }
        return MOVE_NONE;
    }

    // Converts a move to a string in short algebraic notation representation.
    string move_to_san (Move m, Position &pos)
    {
        if (MOVE_NONE == m) return "(none)";
        if (MOVE_NULL == m) return "(null)";
        assert(MoveList<GenType::LEGAL> (pos).contains (m));

        string san;
        auto org = org_sq (m);
        auto dst = dst_sq (m);

        if (mtype (m) != CASTLE)
        {
            auto pt = ptype (pos[org]);

            if (pt != PAWN)
            {
                san = PieceChar[pt];
                if (pt != KING)
                {
                    // Disambiguation if have more then one piece of type 'pt'
                    // that can reach 'dst' with a legal move.
                    switch (ambiguity (m, pos))
                    {
                    case AMB_NONE:                               break;
                    case AMB_RANK: san += to_char (_file (org)); break;
                    case AMB_FILE: san += to_char (_rank (org)); break;
                    case AMB_SQR:  san += to_string (org);       break;
                    default:       assert(false);                break;
                    }
                }
            }

            if (pos.capture (m))
            {
                if (pt == PAWN)
                {
                    san += to_char (_file (org));
                }
                san += "x";
            }

            san += to_string (dst);

            if (   pt == PAWN
                && mtype (m) == PROMOTE)
            {
                san += "=";
                san += PieceChar[promote (m)];
            }
        }
        else
        {
            san = (dst > org ? "O-O" : "O-O-O");
        }

        // Move marker for check & checkmate
        if (pos.gives_check (m))
        {
            StateInfo si;
            pos.do_move (m, si, true);
            san += (MoveList<GenType::LEGAL> (pos).size () != 0 ? "+" : "#");
            pos.undo_move (m);
        }

        return san;
    }
    // Converts a string representing a move in short algebraic notation
    // to the corresponding legal move, if any.
    Move move_from_san (const string &san, Position &pos)
    {
        for (const auto &vm : MoveList<GenType::LEGAL> (pos))
        {
            if (san == move_to_san (vm.move, pos))
            {
                return vm.move;
            }
        }
        return MOVE_NONE;
    }

    //// Converts a move to a string in long algebraic notation representation.
    //string move_to_lan (Move m, Position &pos)
    //{
    //    string lan;
    //    return lan;
    //}
    //// Converts a string representing a move in long algebraic notation
    //// to the corresponding legal move, if any.
    //Move move_from_lan (const string &lan, Position &pos)
    //{
    //    for (const auto &vm : MoveList<GenType::LEGAL> (pos))
    //    {
    //        if (lan == move_to_lan (vm.move, pos))
    //        {
    //            return vm.move;
    //        }
    //    }
    //    return MOVE_NONE;
    //}

    // Returns formated human-readable search information,
    // typically to be appended to the search log file.
    // It uses the two helpers to pretty format the value and time respectively.
    string pretty_pv_info (Thread *const &th)
    {
        const double K = 1000.0;

        ostringstream oss;
        const auto &root_move = th->root_moves[0];
        u64 nodes = Threadpool.nodes ();
        oss << std::setw ( 4) << th->finished_depth
            << std::setw ( 8) << pretty_value (root_move.new_value, th->root_pos.active)
            << std::setw (12) << pretty_time (Threadpool.main_thread ()->time_mgr.elapsed_time ());
        if (nodes < 10*(K))
        {
            oss << std::setw (8) << u16(nodes);
        }
        else
        if (nodes < 10*(K*K))
        {
            oss << std::setw (7) << u16(std::round (nodes / (K))) << 'K';
        }
        else
        if (nodes < 10*(K*K*K))
        {
            oss << std::setw (7) << u16(std::round (nodes / (K*K))) << 'M';
        }
        else
        {
            oss << std::setw (7) << u16(std::round (nodes / (K*K*K))) << 'G';
        }
        oss << ' ';

        StateList states;
        u08 ply = 0;
        for (auto m : root_move)
        {
            oss <<
                //move_to_can (m)
                move_to_san (m, th->root_pos) << ' ';
            states.push_back (StateInfo ());
            th->root_pos.do_move (m, states.back ());
            ++ply;
        }
        while (ply != 0)
        {
            th->root_pos.undo_move (root_move[--ply]);
            states.pop_back ();
        }

        return oss.str ();
    }
}
