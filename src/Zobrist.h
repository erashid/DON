#ifndef _ZOBRIST_H_INC_
#define _ZOBRIST_H_INC_

#include "Type.h"

class Position;

// Zobrist Random numbers
class Zobrist
{
public:
    //static const Key StartMatlKey = U64(0xB76D8438E5D28230);
    //static const Key StartPawnKey = U64(0x37FC40DA841E1692);
    //static const Key StartPosiKey = U64(0x463B96181691FC9C);

    static Key ExclusionKey;

    // 2*6*64 + 2*2 + 8 + 1
    //=   768 +   4 + 8 + 1
    //=                 781
    Key piece_square[CLR_NO][NONE][SQ_NO];  // [color][piece-type][square]
    Key castle_right[CLR_NO][CS_NO];        // [color][castle-side]
    Key en_passant  [F_NO];                 // [enpassant file]
    Key act_side;                           // color

    Zobrist () = default;
    Zobrist (const Zobrist&) = delete;
    Zobrist& operator= (const Zobrist&) = delete;

    // Hash key of the material situation.
    Key compute_matl_key (const Position &pos) const;
    // Hash key of the pawn structure.
    Key compute_pawn_key (const Position &pos) const;
    // Hash key of the complete position.
    Key compute_posi_key (const Position &pos) const;

    // Hash key of the FEN
    Key compute_fen_key (const std::string &fen, bool c960 = false) const;

};

extern       Zobrist Zob;
extern const Zobrist PolyZob;

#endif // _ZOBRIST_H_INC_
