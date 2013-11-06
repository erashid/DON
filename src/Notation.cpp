#include "Notation.h"

#include <sstream>
#include <iomanip>

#include "xstring.h"
#include "Position.h"
#include "MoveGenerator.h"

using namespace BitBoard;
using namespace MoveGenerator;

// Ambiguity if more then one piece of same type can reach 'dst' with a legal move.
// NOTE: for pawns it is not needed because 'org' file is explicit.
AmbType ambiguity (Move m, const Position &pos)
{
    ASSERT (pos.legal (m));

    Square org = sq_org (m);
    Square dst = sq_dst (m);
    Piece mp   = pos[org];
    PType mpt  = _ptype (mp);

    //MoveList m_list = generate<LEGAL> (pos);
    //uint8_t n = 0;
    //uint8_t f = 0;
    //uint8_t r = 0;
    //MoveList::const_iterator itr = m_list.cbegin ();
    //while (itr != m_list.cend ())
    //{
    //    Move mm = *itr;
    //    if (sq_org (mm) != org)
    //    {
    //        if (pos[sq_org (mm)] == mp && sq_dst (mm) == dst)
    //        {
    //            ++n;
    //            if (_file (sq_org (mm)) == _file (org))
    //            {
    //                ++f;
    //            }
    //            if (_rank (sq_org (mm)) == _rank (org))
    //            {
    //                ++r;
    //            }
    //        }
    //    }
    //    ++itr;
    //}
    //if (!n) return AMB_NONE;
    //if (!f) return AMB_RANK;
    //if (!r) return AMB_FILE;
    //return AMB_SQR;

    // Disambiguation if we have more then one piece with destination 'to'
    // note that for pawns is not needed because starting file is explicit.
    //bool
    //    ambiguousMove = false,
    //    ambiguousFile = false,
    //    ambiguousRank = false;
    //
    //Bitboard attackers = (pos.attacks_bb(pc, to) & pos.pieces (us, pt)) ^ from;
    //while (attackers)
    //{
    //    Square sq = pop_lsq(&attackers);
    //    // Pinned pieces are not included in the possible sub-set
    //    if (!pos.pl_move_is_legal(make_move(sq, to), pos.pinned_pieces()))
    //        continue;
    //    ambiguousFile |= file_of(sq) == file_of(from);
    //    ambiguousRank |= rank_of(sq) == rank_of(from);
    //    ambiguousMove = true;
    //}
    //if (!ambiguousMove) return AMB_NONE;
    //if (!ambiguousFile) return AMB_RANK;
    //if (!ambiguousRank) return AMB_FILE;
    //return AMB_SQR;

    //Bitboard occ = pos.pieces ();
    ////Bitboard friends = pos.pieces (pos.active ());
    //Bitboard ambiguous = pos.pieces (pos.active ()) & ~pos.pinneds () - org;
    //switch (mpt)
    //{
    //case PAWN:
    //case KING: return AMB_NONE; break;
    //case NIHT: ambiguous &= attacks_bb<NIHT> (dst) & pos.pieces (NIHT); break;
    //case BSHP: ambiguous &= attacks_bb<BSHP> (dst, occ) & pos.pieces (BSHP); break;
    //case ROOK: ambiguous &= attacks_bb<ROOK> (dst, occ) & pos.pieces (ROOK); break;
    //case QUEN: ambiguous &= attacks_bb<QUEN> (dst, occ) & pos.pieces (QUEN); break;
    //}
    //if (!(ambiguous)) return AMB_NONE;
    //if (!(ambiguous & file_bb (org))) return AMB_RANK;
    //if (!(ambiguous & rank_bb (org))) return AMB_FILE;
    //return AMB_SQR;

    Bitboard others, b;
    others = b = (pos.attacks_from (mp, dst) & pos.pieces (pos.active (), mpt)) - org;
    Bitboard pinneds = pos.pinneds ();
    while (b)
    {
        Move move = mk_move (pop_lsq (b), dst);
        if (!pos.legal (move, pinneds))
        {
            others -= sq_org (move);
        }
    }

    if (!(others)) return AMB_NONE;
    if (!(others & file_bb (org))) return AMB_RANK;
    if (!(others & rank_bb (org))) return AMB_FILE;
    return AMB_SQR;

}

// move_from_can(can, pos) takes a position and a string representing a move in
// simple coordinate algebraic notation and returns an equivalent legal move if any.
Move move_from_can (std::string &can, const Position &pos)
{
    if (5 == can.length ())
    {
        // promotion piece in lowercase
        if (isupper ((unsigned char) can[4]))
        {
            can[4] = char (tolower (can[4]));
        }
    }

    MoveList m_list = generate<LEGAL>(pos);
    for (MoveList::const_iterator itr = m_list.cbegin (); itr != m_list.cend (); ++itr)
    {
        Move m = *itr;
        if (iequals (can, move_to_can (m, pos.chess960 ())))
        {
            return m;
        }
    }
    return MOVE_NONE;
}

Move move_from_san (std::string &san, const Position &pos)
{
    return MOVE_NONE;
}

Move move_from_lan (std::string &lan, const Position &pos)
{
    return MOVE_NONE;
}

// move_to_can(m, c960) converts a move to a string in coordinate algebraic notation (g1f3, a7a8q, etc.).
// The only special case is castling moves,
//  - e1g1 notation in normal chess mode,
//  - e1h1 notation in chess960 mode.
// Internally castle moves are always coded as "king captures rook".
std::string move_to_can (Move m, bool c960)
{
    if (MOVE_NONE == m) return "(none)";
    if (MOVE_NULL == m) return "(null)";
    if (!_ok (m))      return "(xxxx)";
    Square org = sq_org (m);
    Square dst = sq_dst (m);
    MType mt   = _mtype (m);
    if (!c960 && (CASTLE == mt)) dst = ((dst > org) ? F_G : F_C) | _rank (org);
    std::string can = to_string (org) + to_string (dst);
    if (PROMOTE == mt) can += to_char (BLACK | prom_type (m)); // lower case
    return can;
}

// move_to_san(m, pos) takes a position and a legal move as input
// and returns its short algebraic notation representation.
std::string move_to_san (Move m, Position &pos)
{
    if (MOVE_NONE == m) return "(none)";
    if (MOVE_NULL == m) return "(null)";
    ASSERT (pos.legal (m));
    std::string san;
    Square org = sq_org (m);
    Square dst = sq_dst (m);
    Piece mp   = pos[org];
    PType mpt  = _ptype (mp);

    //    switch (mpt)
    //    {
    //    case PAWN:
    //        san = "";
    //        if (pos.capture (m))
    //        {
    //            san += to_char (_file (org));
    //            san += 'x';
    //        }
    //
    //        san += to_string (dst);
    //
    //        if (PROMOTE == _mtype (m))
    //        {
    //            switch (prom_type (m))
    //            {
    //            case QUEN: san += "Q"; break;
    //            case ROOK: san += "R"; break;
    //            case BSHP: san += "B"; break;
    //            case NIHT: san += "N"; break;
    //            default: ASSERT (false); // "Wrong Promotion Piece"
    //            }
    //        }
    //        goto move_marker;
    //
    //    case KING:
    //        if (CASTLE == _mtype (m))
    //        {
    //            CSide cs = ((WHITE == pos.active ()) ?
    //                (dst == SQ_C1) ? CS_Q : (dst == SQ_G1) ? CS_K : CS_NO :
    //                (dst == SQ_C8) ? CS_Q : (dst == SQ_G8) ? CS_K : CS_NO);
    //
    //            switch (cs)
    //            {
    //            case CS_Q: san  = "O-";
    //            case CS_K: san += "O-O"; break;
    //            }
    //            goto move_marker;
    //        }
    //        // NOTE: no break
    //    default:
    //        // piece notation
    //        san = to_char (mpt);
    //
    //        break;
    //    }
    //
    //    switch (ambiguity (m, pos))
    //    {
    //    case AMB_NONE:                               break;
    //    case AMB_RANK: san += to_char (_file (org)); break;
    //    case AMB_FILE: san += to_char (_rank (org)); break;
    //    case AMB_SQR:  san += to_string (org);       break;
    //    default:       ASSERT (false);               break;
    //    }
    //
    //    if (pos.capture (m))
    //    {
    //        san += 'x';
    //    }
    //
    //    san += to_string (dst);
    //    // promote ????????
    //move_marker:
    //    // Marker for check & checkmate
    //    if (pos.check (m, CheckInfo (pos)))
    //    {
    //        StateInfo sinfo;
    //        Position p = pos;
    //        p.do_move (m, sinfo);
    //        size_t legalmove = generate<LEGAL> (p).size ();
    //
    //        san += (legalmove ? '+' : '#');
    //    }

    MType mt = _mtype (m);
    switch (mt)
    {
    case CASTLE:
        san = (dst > org) ? "O-O" : "O-O-O";
        break;

    default:
        if (PAWN == mpt)
        {
            if (pos.capture (m)) san = to_char (_file (org));
        }
        else
        {
            san = to_char (mpt);
            // Disambiguation if we have more then one piece of type 'mpt'
            // that can reach 'dst' with a legal move.
            switch (ambiguity (m, pos))
            {
            case AMB_NONE:                               break;
            case AMB_RANK: san += to_char (_file (org)); break;
            case AMB_FILE: san += to_char (_rank (org)); break;
            case AMB_SQR:  san += to_string (org);       break;
            default:       ASSERT (false);               break;
            }
        }
        if (pos.capture (m)) san += 'x';
        san += to_string (dst);
        if (PROMOTE == mt && PAWN == mpt) san += "=" + to_char (prom_type (m));
        break;
    }

    // Move marker for check & checkmate
    if (pos.check (m, CheckInfo (pos)))
    {
        pos.do_move (m, StateInfo());
        san += (generate<EVASION> (pos).size () ? "+" : "#");
        pos.undo_move ();
    }

    return san;
}

// move_to_lan(m, pos) takes a position and a legal move as input
// and returns its long algebraic notation representation.
std::string move_to_lan (Move m, Position &pos)
{
    std::string lan;


    return lan;
}

// uci_score() converts a value to a string suitable for use with the UCI
// protocol specifications:
//
// cp <x>     The score from the engine's point of view in centipawns.
// mate <y>   Mate in y moves, not plies. If the engine is getting mated
//            use negative values for y.
std::string uci_score (Value v, Value alpha, Value beta)
{
    std::stringstream s;

    if (abs(v) < VALUE_MATES_IN_MAX_PLY)
    {
        s << "cp " << v * 100 / int32_t (VALUE_MG_PAWN);
    }
    else
    {
        s << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;
    }
    s << (v >= beta ? " lowerbound" : v <= alpha ? " upperbound" : "");

    return s.str();
}

namespace {

    // score to string
    std::string to_string (Value v)
    {
        std::stringstream s;

        if (false);
        else if (v >= VALUE_MATES_IN_MAX_PLY)
        {
            s <<  "#" << (VALUE_MATE - v + 1) / 2;
        }
        else if (v <= VALUE_MATED_IN_MAX_PLY)
        {
            s << "-#" << (VALUE_MATE + v) / 2;
        }
        else
        {
            s << std::setprecision(2) << std::fixed << std::showpos << double(v) / VALUE_MG_PAWN;
        }
        return s.str();
    }

    // time to string
    std::string to_string(int64_t msecs)
    {
        const int MSecMinute = 1000 * 60;
        const int MSecHour   = 1000 * 60 * 60;

        int64_t hours   =   msecs / MSecHour;
        int64_t minutes =  (msecs % MSecHour) / MSecMinute;
        int64_t seconds = ((msecs % MSecHour) % MSecMinute) / 1000;

        std::stringstream s;

        if (hours) s << hours << ':';
        s << std::setfill('0') << std::setw(2) << minutes << ':' << std::setw(2) << seconds;

        return s.str();
    }

}

// pretty_pv() formats human-readable search information, typically to be
// appended to the search log file. It uses the two helpers below to pretty
// format time and score respectively.
std::string pretty_pv(Position& pos, int16_t depth, Value value, int64_t msecs, const MoveList &pv)
{
    const int64_t K = 1000;
    const int64_t M = 1000000;
    
    std::stringstream spv;

    spv << std::setw(2) << depth
        << std::setw(8) << to_string (value)
        << std::setw(8) << to_string (msecs);

    if (pos.game_nodes () < M)
    {
        spv << std::setw(8) << pos.game_nodes () / 1 << "  ";
    }
    else if (pos.game_nodes () < K * M)
    {
        spv << std::setw(7) << pos.game_nodes () / K << "K  ";
    }
    else
    {
        spv << std::setw(7) << pos.game_nodes () / M << "M  ";
    }

    std::string padding = std::string (spv.str().length(), ' ');
    size_t length = padding.length();
    StateInfoStack st;

    std::for_each (pv.cbegin (), pv.cend (), [&] (Move m)
    {
        std::string san = move_to_san (m, pos);

        if (length + san.length() > 80)
        {
            spv << "\n" + padding;
            length = padding.length();
        }

        spv << san << ' ';
        length += san.length() + 1;

        st.push (StateInfo());
        pos.do_move (m, st.top ());

    });

    std::for_each (pv.crbegin (), pv.crend (), [&] (Move m)
    {
        pos.undo_move ();
    });

    return spv.str();
}
