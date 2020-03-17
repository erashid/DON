#pragma once

#include <array>

#include "Endgame.h"
#include "Position.h"
#include "Type.h"

namespace Material {

    constexpr i32 PhaseResolution = 128;

    /// Material::Entry contains various information about a material configuration.
    struct Entry {

        Key   key;
        i32   phase;
        Score imbalance;
        Array<Scale, COLORS> scale;

        EndgameBase<Value> const *evaluationFunc;
        Array<EndgameBase<Scale> const*, COLORS> scalingFunc;

        void evaluate(Position const&);

    };

    using Table = HashTable<Entry>;

    extern Entry* probe(Position const&);
}
