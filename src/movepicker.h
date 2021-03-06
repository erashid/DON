#pragma once

#include <array>
#include <limits>
#include <type_traits>

#include "movegenerator.h"
#include "position.h"
#include "type.h"

/// Table is a generic N-dimensional array
template<typename T, size_t Size, size_t... Sizes>
class Table :
    public std::array<Table<T, Sizes...>, Size> {

    static_assert(Size != 0, "Size incorrect");
private:
    using NestedTable = Table<T, Size, Sizes...>;

public:
    void fill(T const &value) {
        assert(std::is_standard_layout<NestedTable>::value);

        auto *p{ reinterpret_cast<T*>(this) };
        std::fill(p, p + sizeof(*this) / sizeof(T), value);
    }

};
template<typename T, size_t Size>
class Table<T, Size> :
    public std::array<T, Size> {

    static_assert(Size != 0, "Size incorrect");
};



/// Stats stores the value. It is usually a number.
/// We use a class instead of naked value to directly call
/// history update operator<<() on the entry so to use stats
/// tables at caller sites as simple multi-dim arrays.
template<typename T, int32_t D>
class Stats {

public:

    void operator=(T const &e) noexcept { entry = e; }

    T* operator &() noexcept { return &entry; }
    T* operator->() noexcept { return &entry; }

    operator T const&() const noexcept { return entry; }

    void operator<<(int32_t bonus) noexcept {
        static_assert(D <= std::numeric_limits<T>::max(), "D overflows T");
        assert(std::abs(bonus) <= D); // Ensure range is [-D, +D]

        entry += bonus - entry * std::abs(bonus) / D;

        assert(std::abs(entry) <= D);
    }

private:

    T entry;
};

/// StatsTable is a generic N-dimensional array used to store various statistics.
/// The first template T parameter is the base type of the array,
/// the D parameter limits the range of updates (range is [-D, +D]), and
/// the last parameters (Size and Sizes) encode the dimensions of the array.
template<typename T, int32_t D, size_t Size, size_t... Sizes>
class StatsTable :
    public std::array<StatsTable<T, D, Sizes...>, Size> {

    static_assert(Size != 0, "Size incorrect");
private:
    using NestedStatsTable = StatsTable<T, D, Size, Sizes...>;

public:
    void fill(T const &value) {
        // For standard-layout 'this' points to first struct member
        assert(std::is_standard_layout<NestedStatsTable>::value);

        using Entry = Stats<T, D>;
        Entry *p{ reinterpret_cast<Entry*>(this) };
        std::fill(p, p + sizeof(*this) / sizeof(Entry), value);
    }
};
template<typename T, int32_t D, size_t Size>
class StatsTable<T, D, Size> :
    public std::array<Stats<T, D>, Size> {

    static_assert(Size != 0, "Size incorrect");
};

/// ButterFlyStatsTable stores moves history according to color.
/// Used for reduction and move ordering decisions.
/// indexed by [color][moveMask]
using ButterFlyStatsTable       = StatsTable<int16_t, 13365, COLORS, SQUARES*SQUARES>;

/// PlyIndexStatsTable stores moves history according to ply from 0 to MAX_LOWPLY-1
/// At higher depths it records successful quiet moves near the root
/// and quiet moves which were in the PV (ttPv)
/// It is cleared with each new search and filled during iterative deepening.
/// indexed by [0...MAX_LOWPLY-1][moveMask]
constexpr int16_t MAX_LOWPLY{ 4 };
using PlyIndexStatsTable        = StatsTable<int16_t, 10692, MAX_LOWPLY, SQUARES*SQUARES>;

/// PieceSquareTypeStatsTable stores move history according to piece.
/// indexed by [piece][square][captured]
using PieceSquareTypeStatsTable = StatsTable<int16_t, 10692, PIECES, SQUARES, PIECE_TYPES>;

/// PieceSquareStatsTable store moves history according to piece.
/// indexed by [piece][square]
using PieceSquareStatsTable     = StatsTable<int16_t, 29952, PIECES, SQUARES>;
/// ContinuationStatsTable is the combined history of a given pair of moves, usually the current one given a previous one.
/// The nested history table is based on PieceSquareStatsTable, indexed by [piece][square]
using ContinuationStatsTable    = Table<PieceSquareStatsTable, PIECES, SQUARES>;

/// PieceSquareMoveTable stores moves, indexed by [piece][square]
using PieceSquareMoveTable      = Table<Move, PIECES, SQUARES>;


/// MovePicker class is used to pick one legal moves from the current position.
/// nextMove() is the most important method, which returns a new legal move every time until there are no more moves
/// In order to improve the efficiency of the alpha-beta algorithm,
/// MovePicker attempts to return the moves which are most likely to get a cut-off first.
class MovePicker {

public:

    MovePicker(
        Position const&,
        Move, Depth,
        ButterFlyStatsTable       const*,
        PlyIndexStatsTable        const*,
        PieceSquareTypeStatsTable const*,
        PieceSquareStatsTable     const**,
        int16_t, Move const *, Move) noexcept;

    MovePicker(
        Position const&,
        Move, Depth,
        ButterFlyStatsTable       const*,
        PieceSquareTypeStatsTable const*,
        PieceSquareStatsTable     const**,
        Square) noexcept;

    MovePicker(
        Position const&,
        Move, Value,
        PieceSquareTypeStatsTable const*) noexcept;

    MovePicker() = delete;
    MovePicker(MovePicker const&) = delete;
    MovePicker(MovePicker&&) = delete;

    MovePicker& operator=(MovePicker const&) = delete;
    MovePicker& operator=(MovePicker&&) = delete;

    Move nextMove();

    bool pickQuiets;

private:

    template<GenType GT>
    void value() noexcept;

    template<typename Pred>
    bool pick(Pred) noexcept;

    Position const &pos;

    Move ttMove{ MOVE_NONE };
    Depth depth{ DEPTH_ZERO };

    ButterFlyStatsTable       const *mainStats{ nullptr };
    PlyIndexStatsTable        const *lowPlyStats{ nullptr };
    PieceSquareTypeStatsTable const *captureStats{ nullptr };
    PieceSquareStatsTable     const **contStats{ nullptr };

    int16_t ply{ 0 };
    Value threshold{ VALUE_ZERO };
    Square recapSq{ SQ_NONE };

    uint8_t stage{ 0 };

    ValMoves vmoves;
    ValMoves::iterator vmBeg,
                       vmEnd;

    Moves refutationMoves,
          badCaptureMoves;
    Moves::iterator mBeg,
                    mEnd;
};
