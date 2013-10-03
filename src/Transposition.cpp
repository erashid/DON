#include "Transposition.h"

#include <iostream>
//#include <cmath>
#include "BitScan.h"
#include "Engine.h"


TranspositionTable TT;

// resize(mb) sets the size of the table, measured in mega-bytes.
// Transposition table consists of a power of 2 number of clusters and
// each cluster consists of NUM_TENTRY_CLUSTER number of entry.
void TranspositionTable::resize (uint32_t size_mb)
{
    ASSERT (size_mb <= MAX_SIZE_TT);
    if (size_mb > MAX_SIZE_TT)
    {
        std::cerr << "ERROR: TT size too large " << size_mb << " MB..." << std::endl;
        return;
    }

    uint64_t size_byte      = uint64_t (size_mb) << 20;
    uint64_t total_entry    = (size_byte) / SIZE_TENTRY;
    //uint64_t total_cluster  = total_entry / NUM_TENTRY_CLUSTER;

    uint8_t bit_hash = scan_msb (total_entry);
    ASSERT (bit_hash < MAX_BIT_HASH);
    if (bit_hash >= MAX_BIT_HASH) return;

    total_entry     = uint64_t (1) << bit_hash;
    if (_mask_hash == (total_entry - NUM_TENTRY_CLUSTER)) return;

    erase ();

    _mem = std::calloc (total_entry * SIZE_TENTRY + SIZE_CACHE_LINE - 1, 1);
    if (!_mem)
    {
        std::cerr << "ERROR: TT failed to allocate " << size_mb << " MB..." << std::endl;
        Engine::exit(EXIT_FAILURE);
    }

    _table_entry = (TranspositionEntry*) 
        ((uintptr_t (_mem) + (SIZE_CACHE_LINE - 1)) & ~(SIZE_CACHE_LINE - 1));

    _mask_hash      = (total_entry - NUM_TENTRY_CLUSTER);
    _store_entry    = 0;
    _generation     = 0;
}

// store() writes a new entry in the transposition table.
// It contains folowing valuable information.
//  - key
//  - move.
//  - score.
//  - depth.
//  - bound.
//  - nodes.
// The lowest order bits of position key are used to decide on which cluster the position will be placed.
// When a new entry is written and there are no empty entries available in cluster,
// it replaces the least valuable of these entries.
// An entry e1 is considered to be more valuable than a entry e2
// * if e1 is from the current search and e2 is from a previous search.
// * if e1 & e2 is from a current search then EXACT bound is valuable.
// * if the depth of e1 is bigger than the depth of e2.
void TranspositionTable::store (Key key, Move move, Depth depth, Bound bound, Score score, uint16_t nodes)
{
    uint32_t key32 = uint32_t (key); // 32 lower-bit of key

    TranspositionEntry *te = get_entry (key);
    // By default replace first entry
    TranspositionEntry *re = te;

    for (uint8_t i = 0; i < NUM_TENTRY_CLUSTER; ++i, ++te)
    {
        if (!te->key () || te->key () == key32) // empty or overwrite old
        {
            // Do not overwrite when new type is EVAL_LOWER
            if (te->key() && EVAL_LOWER == bound) return;

            if (MOVE_NONE == move)
            {
                move = te->move (); // preserve any existing TT move
            }

            re = te;
            break;
        }
        else
        {
            // replace would be a no-op in this common case
            if (0 == i) continue;
        }

        // implement replacement strategy
        int8_t c1 = ((re->gen () == _generation) ? +2 : 0);
        int8_t c2 = ((te->gen () == _generation) || (te->bound () == EXACT) ? -2 : 0);
        int8_t c3 = ((te->depth () < re->depth ()) ? +1 : 0);
        //int8_t c4 = ((te->nodes () < re->nodes ()) ? +1 : 0);

        if ((c1 + c2 + c3) > 0)
        {
            re = te;
        }
    }

    re->save (key32, move, depth, bound, nodes, _generation, score, SCORE_ZERO, SCORE_ZERO);
    ++_store_entry;
}

// retrieve() looks up the entry in the transposition table.
// Returns a pointer to the entry found or NULL if not found.
const TranspositionEntry* TranspositionTable::retrieve (Key key) const
{
    const TranspositionEntry* te = get_entry (key);
    uint32_t key32 = uint32_t (key);

    for (uint8_t i = 0; i < NUM_TENTRY_CLUSTER; ++i, ++te)
    {
        if (te->key () == key32)
        {
            return te;
        }
    }
    return NULL;
}

// permill_full() returns the per-mille of the all transposition entries
// which have received at least one write during the current search.
// It is used to display the "info hashfull ..." information in UCI.
// "the hash is <x> permill full", the engine should send this info regularly.
// hash, are using <x>%. of the state of full.
double TranspositionTable::permill_full () const
{
    uint64_t total_entry = (_mask_hash + NUM_TENTRY_CLUSTER);

    //return (0 != total_entry) ?
    //    //(1 - exp (_store_entry * log (1.0 - 1.0/total_entry))) * 1000 :
    //    (1 - exp (log (1.0 - double (_store_entry) / double (total_entry)))) * 1000 :
    //    //exp (log (1000.0 + _store_entry - total_entry)) :
    //    0.0;

    return (0 != total_entry) ?
        double (_store_entry) * 1000 / double (total_entry) : 0.0;

}
