#pragma once

#include "position.h"
#include "type.h"

/// Zobrist class
struct Zobrist {

    Zobrist() = default;
    Zobrist(Zobrist const&) = delete;
    Zobrist(Zobrist&&) = delete;

    Zobrist& operator=(Zobrist const&) = delete;
    Zobrist& operator=(Zobrist&&) = delete;

    Key computeMatlKey(Position const&) const noexcept;
    Key computePawnKey(Position const&) const noexcept;
    Key computePosiKey(Position const&) const noexcept;


    Key enpassantKey(Square epSq) const noexcept { return epSq != SQ_NONE ? enpassant[sFile(epSq)] : 0; }

    // 15*64 + 16 + 8 + 1 = 985
    // 12*64 + 16 + 8 + 1 = 793
    //                    = 192 extra
    Key psq[PIECES][SQUARES];
    Key castling[CASTLE_RIGHTS];
    Key enpassant[FILES];
    Key side;
    Key nopawn;
};

namespace Zobrists {

    extern void initialize() noexcept;
}

extern Zobrist RandZob;
extern Zobrist const PolyZob;
