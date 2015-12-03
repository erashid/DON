#ifndef _TB_SYZYGY_H_INC_
#define _TB_SYZYGY_H_INC_

#include "Type.h"
#include "Searcher.h"

namespace TBSyzygy {

    extern i32      MaxProbeLimit;
    extern i32      ProbeLimit;
    extern Depth    ProbeDepth;
    extern bool     UseRule50;

    extern u64      Hits;
    extern bool     RootInTB;
    extern Value    ProbeValue;

    i32 probe_dtz (Position &pos, i32 *success);
    i32 probe_wdl (Position &pos, i32 *success);

    bool root_probe_dtz (Position &pos, Searcher::RootMoveVector &root_moves);
    bool root_probe_wdl (Position &pos, Searcher::RootMoveVector &root_moves);

    void initialize (const std::string syzygy_path);
}

#endif // _TB_SYZYGY_H_INC_
