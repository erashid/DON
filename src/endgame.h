#pragma once

#include <memory>
#include <unordered_map>

#include "position.h"
#include "type.h"

/// EndgameCode lists all supported endgame functions by corresponding codes
enum EndgameCode : uint8_t {

    EVALUATION_FUNCTIONS,

    KXK,   // Generic "mate lone king" eval
    KPK,   // KP vs K
    KNNK,  // KNN vs K
    KBNK,  // KBN vs K
    KRKP,  // KR vs KP
    KRKB,  // KR vs KB
    KRKN,  // KR vs KN
    KQKP,  // KQ vs KP
    KQKR,  // KQ vs KR
    KNNKP, // KNN vs KP

    SCALING_FUNCTIONS,
    KRPKR,   // KRP vs KR
    KRPKB,   // KRP vs KB
    KRPPKRP, // KRPP vs KRP
    KBPKB,   // KBP vs KB
    KBPPKB,  // KBPP vs KB
    KBPKN,   // KBP vs KN

    // Generic Scale functions
    KPsK,    // KPs vs K
    KPKP,    // KP vs KP
    KBPsK,   // KBPs vs K
    KQKRPs,  // KQ vs KRPs
};

/// Endgame functions can be of two category depending on whether they return Value or Scale.
template<EndgameCode C>
using EndgameType = typename std::conditional<C < SCALING_FUNCTIONS, Value, Scale>::type;

/// Base functors for endgame evaluation and scaling functions
template<typename T>
class EndgameBase {

public:
    explicit EndgameBase(Color c) :
        stngColor{  c },
        weakColor{ ~c } {
    }
    virtual ~EndgameBase() = default;

    EndgameBase& operator=(EndgameBase const&) = delete;
    EndgameBase& operator=(EndgameBase&&) = delete;

    virtual T operator()(Position const&) const = 0;

    Color const stngColor,
                weakColor;
};

/// Derived functors for endgame evaluation and scaling functions
template<EndgameCode C, typename T = EndgameType<C>>
class Endgame :
    public EndgameBase<T> {

public:
    using EndgameBase<T>::EndgameBase;

    Endgame& operator=(Endgame const&) = delete;
    Endgame& operator=(Endgame&&) = delete;

    T operator()(Position const&) const override;
};


namespace EndGame {

    template<typename T>  using EGPtr = std::unique_ptr<EndgameBase<T>>;
    template<typename T>  using EGMap = std::unordered_map<Key, EGPtr<T>>;
    template<typename T1,
             typename T2> using EGMapPair = std::pair<EGMap<T1>, EGMap<T2>>;

    extern EGMapPair<Value, Scale> EndGames;

    template<typename T>
    EGMap<T>& mapEG() noexcept {
        return std::get<std::is_same<T, Scale>::value>(EndGames);
    }

    template<typename T>
    EndgameBase<T> const* probe(Key matlKey) noexcept {
        auto const itr{ mapEG<T>().find(matlKey) };
        return itr != mapEG<T>().end() ? itr->second.get() : nullptr;
    }

    extern void initialize();
}
