#include "TBsyzygy.h"

#include <fstream>
#include <iostream>
#include <cstdint>
#include <cstring>   // For std::memset
#include <vector>
#include <deque>

#include "BitBoard.h"
#include "Position.h"
#include "MoveGenerator.h"
#include "Thread.h"
#include "Engine.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#else
#   if !defined(NOMINMAX)
#       define NOMINMAX // Disable macros min() and max()
#   endif
#   if !defined(WIN32_LEAN_AND_MEAN)
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#endif

namespace TBSyzygy {

    using namespace std;
    using namespace BitBoard;
    using namespace MoveGen;
    using namespace Threading;
    using namespace Searcher;

    string  PathString      = Empty;
    i32     MaxLimitPiece   = 0;

    namespace {

        // Each table has a set of flags: all of them refer to DTZ tables, the last one to WDL tables
        enum TBFlag : u08
        {
            STM = 1,
            MAPPED = 2,
            WIN_PLIES = 4,
            LOSS_PLIES = 8,
            SINGLE_VALUE = 128
        };

        Piece tb_piece (i32 pc) { return pc != 0 ? Piece(pc - 1) : NO_PIECE; }

        // DTZ tables don't store valid scores for moves that reset the rule50 counter
        // like captures and pawn moves but we can easily recover the correct dtz of the
        // previous move if we know the position's WDL score.
        i32 dtz_before_zeroing (WDLScore wdl)
        {
            return  wdl == WDLWin        ? +1   :
                    wdl == WDLCursedWin  ? +101 :
                    wdl == WDLCursedLoss ? -101 :
                    wdl == WDLLoss       ? -1   : 0;
        }

        // Numbers in little endian used by sparseIndex[] to point into block_length[]
        struct SparseEntry
        {
            char block[4];   // Number of block
            char offset[2];  // Offset within the block
        };

        static_assert(sizeof (SparseEntry) == 6, "SparseEntry must be 6 bytes");

        typedef u16 Sym; // Huffman symbol

        struct LR
        {
        public:
            enum Side : u08
            {
                Left,
                Right,
                Center
            };

            u08 lr[3]; // The first 12 bits is the left-hand symbol, the second 12
                       // bits is the right-hand symbol. If symbol has length 1,
                       // then the first byte is the stored value.
            template<Side S>
            Sym get ()
            {
                return S == Left  ? ((lr[1] & 0xF) << 8) | lr[0] :
                       S == Right ?  (lr[2] << 4) | (lr[1] >> 4) :
                       S == Center ?   lr[0] : (assert(false), Sym (-1));
            }
        };

        static_assert(sizeof (LR) == 3, "LR tree entry must be 3 bytes");

        const i32 TBPIECES = 6;

        struct PairsData
        {
        public:
            i32 flags;
            size_t block_size;              // Block size in bytes
            size_t span;                    // About every span values there is a SparseIndex[] entry
            i32 num_blocks;                 // Number of blocks in the TB file
            i32 max_sym_len;                // Maximum length in bits of the Huffman symbols
            i32 min_sym_len;                // Minimum length in bits of the Huffman symbols
            Sym* lowest_sym;                // lowest_sym[l] is the symbol of length l with the lowest value
            LR* btree;                      // btree[sym] stores the left and right symbols that expand sym
            u16* block_length;              // Number of stored positions (minus one) for each block: 1..65536
            i32 block_length_size;          // Size of block_length[] table: padded so it's bigger than num_blocks
            SparseEntry* sparseIndex;       // Partial indices into block_length[]
            size_t sparse_index_size;       // Size of SparseIndex[] table
            u08 *data;                      // Start of Huffman compressed data
            vector<u64> base64;             // base64[l - min_sym_len] is the 64bit-padded lowest symbol of length l
            vector<u08> sym_len;            // Number of values (-1) represented by a given Huffman symbol: 1..256
            Piece pieces[TBPIECES];         // Position pieces: the order of pieces defines the groups
            u64 group_idx[TBPIECES+1];      // Start index used for the encoding of the group's pieces
            i32 group_len[TBPIECES+1];      // Number of pieces in a given group: KRKN -> (3, 1)
        };

        // Helper struct to avoid to manually define entry copy constructor,
        // because default one is not compatible with std::atomic_bool.
        struct Atomic
        {
        public:
            Atomic () = default;
            Atomic (const Atomic& e) { ready = e.ready.load (); } // MSVC 2013 wants assignment within body
            atomic_bool ready;
        };

        // We define types for the different parts of the WLDEntry and DTZEntry with
        // corresponding specializations for pieces or pawns.

        struct WLDEntryPiece
        {
        public:
            PairsData* precomp;
        };

        struct WDLEntryPawn
        {
        public:
            u08 pawn_count[2];        // [Lead color / other color]
            WLDEntryPiece file[2][4]; // [wtm / btm][FILE_A..FILE_D]
        };

        struct DTZEntryPiece
        {
        public:
            PairsData* precomp;
            u16 map_idx[4]; // WDLWin, WDLLoss, WDLCursedWin, WDLCursedLoss
            u08 *map;
        };

        struct DTZEntryPawn
        {
        public:
            u08 pawn_count[2];
            DTZEntryPiece file[4];
            u08 *map;
        };

        struct TBEntry : public Atomic
        {
        public:
            void* base_address;
            u64 mapping;
            Key key1;
            Key key2;
            i32 piece_count;
            bool has_pawns;
            bool has_unique_pieces;
        };

        // Now the main types: WDLEntry and DTZEntry
        struct WDLEntry : public TBEntry
        {
        public:
            WDLEntry (const std::string &code);
            ~WDLEntry ();
            union
            {
                WLDEntryPiece piece_table[2]; // [wtm / btm]
                WDLEntryPawn  pawn_table;
            };
        };

        struct DTZEntry : public TBEntry
        {
        public:
            DTZEntry (const WDLEntry &wdl);
            ~DTZEntry ();
            union
            {
                DTZEntryPiece piece_table;
                DTZEntryPawn  pawn_table;
            };
        };

        typedef decltype(WDLEntry::piece_table) WDLPieceTable;
        typedef decltype(DTZEntry::piece_table) DTZPieceTable;
        typedef decltype(WDLEntry::pawn_table) WDLPawnTable;
        typedef decltype(DTZEntry::pawn_table) DTZPawnTable;

        auto item (WDLPieceTable& e, i32 stm, i32) -> decltype(e[stm])& { return e[stm]; }
        auto item (DTZPieceTable& e, i32, i32) -> decltype(e)& { return e; }
        auto item (WDLPawnTable&  e, i32 stm, i32 f) -> decltype(e.file[stm][f])& { return e.file[stm][f]; }
        auto item (DTZPawnTable&  e, i32, i32 f) -> decltype(e.file[f])& { return e.file[f]; }

        template<typename E> struct Ret { typedef i32 type; };
        template<> struct Ret<WDLEntry> { typedef WDLScore type; };

        i32 MapPawns[SQ_NO];
        i32 MapB1H1H7[SQ_NO];
        i32 MapA1D1D4[SQ_NO];
        i32 MapKK[10][SQ_NO]; // [MapA1D1D4][SQ_NO]

        // Comparison function to sort leading pawns in ascending MapPawns[] order
        bool pawns_comp (Square i, Square j)
        {
            return MapPawns[i] < MapPawns[j];
        }
        i32 off_A1H8 (Square sq)
        {
            return i32(_rank (sq)) - i32(_file (sq));
        }

        const Value WDL_To_Value[] =
        {
            -VALUE_MATE + i32(MaxPlies) + 1,
            VALUE_DRAW - 2,
            VALUE_DRAW,
            VALUE_DRAW + 2,
            +VALUE_MATE - i32(MaxPlies) - 1
        };

        const string PieceToChar = "PNBRQK  pnbrqk";

        i32 Binomial[6][SQ_NO];    // [k][n] k elements from a set of n elements
        i32 LeadPawnIdx[5][SQ_NO]; // [lead_pawn_count][SQ_NO]
        i32 LeadPawnsSize[5][4];   // [lead_pawn_count][F_A..F_D]

        enum
        { 
            BigEndian,
            LittleEndian
        };

        template<typename T, i32 Half = sizeof (T) / 2, i32 End = sizeof (T) - 1>
        inline void swap_byte (T &x)
        {
            char *c = (char*) &x;
            for (i32 i = 0; i < Half; ++i)
            {
                char tmp = c[i];
                c[i] = c[End - i];
                c[End - i] = tmp;
            }
        }
        template<> inline void swap_byte<u08, 0, 0> (u08 &) {}

        template<typename T, i32 LE> T number (void* addr)
        {
            const union { u32 i; char c[4]; } Le = { 0x01020304 };
            const bool IsLittleEndian = (Le.c[0] == 4);

            T v;
            if ((uintptr_t) addr & (alignof(T) -1)) // Unaligned pointer (very rare)
            {
                std::memcpy (&v, addr, sizeof (T));
            }
            else
            {
                v = *((T*) addr);
            }
            if (LE != IsLittleEndian)
            {
                swap_byte (v);
            }
            return v;
        }

        class HashTable
        {
        private:
            typedef pair<WDLEntry*, DTZEntry*> EntryPair;
            typedef pair<Key, EntryPair> Entry;

            static const i32 TBHASHBITS = 10;
            static const i32 HSHMAX     = 5;

            Entry table[1 << TBHASHBITS][HSHMAX];

            deque<WDLEntry> wdl_table;
            deque<DTZEntry> dtz_table;

            void insert (Key key, WDLEntry *wdl, DTZEntry *dtz)
            {
                Entry *entry = table[key >> (64 - TBHASHBITS)];

                for (i32 i = 0; i < HSHMAX; ++i, ++entry)
                {
                    if (   entry->second.first == nullptr
                        || entry->first == key)
                    {
                        *entry = std::make_pair (key, std::make_pair (wdl, dtz));
                        return;
                    }
                }
                std::cerr << "HSHMAX too low!" << std::endl;
                Engine::stop (EXIT_FAILURE);
            }

        public:
            template<typename E, i32 I = is_same<E, WDLEntry>::value ? 0 : 1>
            E* get (Key key)
            {
                auto *entry = table[key >> (64 - TBHASHBITS)];
                for (i32 i = 0; i < HSHMAX; ++i)
                {
                    if (entry->first == key)
                    {
                        return std::get<I> (entry->second);
                    }
                    ++entry;
                }
                return nullptr;
            }

            void clear ()
            {
                std::memset (table, 0, sizeof (table));
                wdl_table.clear ();
                dtz_table.clear ();
            }
            size_t size () const { return wdl_table.size (); }
            void insert (const vector<PieceType>& pieces);
        };

        HashTable EntryTable;

        class TBFile
            : public ifstream
        {
        public:
            // Look for and open the file among the Paths directories where the .rtbw and .rtbz files can be found.
            static vector<string> Paths;

            string filename;

            TBFile (const string &code, const string &ext)
            {
                auto file = code;
                file.insert (file.find ('K', 1), "v");
                file += ext;
                for (const auto &path : Paths)
                {
                    auto file_path = append_path (path, file);
                    open (file_path);
                    if (is_open ())
                    {
                        filename = file_path;
                        close ();
                        break;
                    }
                }
            }

            // Memory map the file and check it. File should be already open and will be
            // closed after mapping.
            u08* map (void **base_address, u64 *mapping, const u08 *TB_MAGIC)
            {
                assert(!white_spaces (filename));

#ifndef _WIN32
                struct stat statbuf;
                i32 fd = ::open (filename.c_str (), O_RDONLY);
                fstat (fd, &statbuf);
                *mapping = statbuf.st_size;
                *base_address = mmap (nullptr, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
                ::close (fd);

                if (*base_address == MAP_FAILED)
                {
                    std::cerr << "Could not mmap() " << filename << std::endl;
                    return nullptr;
                }
#else
                HANDLE fd = CreateFile (filename.c_str (), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                DWORD size_high;
                DWORD size_low = GetFileSize (fd, &size_high);
                HANDLE mmap = CreateFileMapping (fd, nullptr, PAGE_READONLY, size_high, size_low, nullptr);
                CloseHandle (fd);

                if (mmap == 0)
                {
                    std::cerr << "CreateFileMapping() failed, name = " << filename << ", error = " << GetLastError () << std::endl;
                    return nullptr;
                }

                *mapping = u64(mmap);
                *base_address = MapViewOfFile (mmap, FILE_MAP_READ, 0, 0, 0);

                if (*base_address == nullptr)
                {
                    std::cerr << "MapViewOfFile() failed, name = " << filename << ", error = " << GetLastError () << std::endl;
                    return nullptr;
                }
#endif
                u08 *data = (u08*) *base_address;

                if (   *data++ != *TB_MAGIC++
                    || *data++ != *TB_MAGIC++
                    || *data++ != *TB_MAGIC++
                    || *data++ != *TB_MAGIC)
                {
                    std::cerr << "Corrupted table in file " << filename << std::endl;
                    unmap (*base_address, *mapping);
                    *base_address = nullptr;
                    return nullptr;
                }

                return data;
            }

            static void unmap (void* base_address, u64 mapping)
            {
#ifndef _WIN32
                munmap (base_address, mapping);
#else
                UnmapViewOfFile (base_address);
                CloseHandle (HANDLE(mapping));
#endif
            }
        };

        vector<string> TBFile::Paths;

        WDLEntry::WDLEntry (const string &code)
        {
            memset (this, 0, sizeof (*this));
            ready = false;
            StateInfo si;
            Position pos;
            key1 = pos.setup (code, si, WHITE).si->matl_key;
            piece_count = pos.count<NONE> ();
            has_pawns = pos.count<PAWN> () != 0;
            for (Color c = WHITE; c <= BLACK; ++c)
            {    
                for (PieceType pt = PAWN; pt < KING; ++pt)
                {
                    if (pos.count (c, pt) == 1)
                    {
                        has_unique_pieces = true;
                        goto done;
                    }
                }
            }
            done:
            if (has_pawns)
            {
                // Set the leading color. In case both sides have pawns the leading color
                // is the side with less pawns because this leads to better compression.
                auto lead_color =  pos.count<PAWN> (BLACK) == 0
                                || (   pos.count<PAWN> (WHITE)
                                    && pos.count<PAWN> (BLACK) >= pos.count<PAWN> (WHITE)) ? WHITE : BLACK;

                pawn_table.pawn_count[0] = u08(pos.count<PAWN> ( lead_color));
                pawn_table.pawn_count[1] = u08(pos.count<PAWN> (~lead_color));
            }
            key2 = pos.setup (code, si, BLACK).si->matl_key;
        }

        WDLEntry::~WDLEntry ()
        {
            if (base_address != nullptr)
            {
                TBFile::unmap (base_address, mapping);
            }
            for (i32 i = 0; i < 2; ++i)
            {
                if (has_pawns)
                {
                    for (auto f = F_A; f <= F_D; ++f)
                    {
                        delete pawn_table.file[i][f].precomp;
                    }
                }
                else
                {
                    delete piece_table[i].precomp;
                }
            }
        }

        DTZEntry::DTZEntry (const WDLEntry& wdl)
        {
            memset (this, 0, sizeof (*this));

            ready = false;
            key1 = wdl.key1;
            key2 = wdl.key2;
            piece_count = wdl.piece_count;
            has_pawns = wdl.has_pawns;
            has_unique_pieces = wdl.has_unique_pieces;

            if (has_pawns)
            {
                pawn_table.pawn_count[0] = wdl.pawn_table.pawn_count[0];
                pawn_table.pawn_count[1] = wdl.pawn_table.pawn_count[1];
            }
        }

        DTZEntry::~DTZEntry ()
        {
            if (base_address != nullptr)
            {
                TBFile::unmap (base_address, mapping);
            }
            if (has_pawns)
            {
                for (auto f = F_A; f <= F_D; ++f)
                {
                    delete pawn_table.file[f].precomp;
                }
            }
            else
            {
                delete piece_table.precomp;
            }
        }

        void HashTable::insert (const vector<PieceType> &pieces)
        {
            string code;
            for (auto pt : pieces)
            {
                code += PieceToChar[pt];
            }
            TBFile file (code, ".rtbw");
            if (!file.filename.empty ())
            {
                if (MaxLimitPiece < i32(pieces.size ()))
                {
                    MaxLimitPiece = i32(pieces.size ());
                }

                wdl_table.push_back (WDLEntry (code));
                dtz_table.push_back (DTZEntry (wdl_table.back ()));

                insert (wdl_table.back ().key1, &wdl_table.back (), &dtz_table.back ());
                insert (wdl_table.back ().key2, &wdl_table.back (), &dtz_table.back ());
            }
        }

        // TB tables are compressed with canonical Huffman code. The compressed data is divided into
        // blocks of size d->block_size, and each block stores a variable number of symbols.
        // Each symbol represents either a WDL or a (remapped) DTZ value, or a pair of other symbols
        // (recursively). If you keep expanding the symbols in a block, you end up with up to 65536
        // WDL or DTZ values. Each symbol represents up to 256 values and will correspond after
        // Huffman coding to at least 1 bit. So a block of 32 bytes corresponds to at most
        // 32 x 8 x 256 = 65536 values. This maximum is only reached for tables that consist mostly
        // of draws or mostly of wins, but such tables are actually quite common. In principle, the
        // blocks in WDL tables are 64 bytes long (and will be aligned on cache lines). But for
        // mostly-draw or mostly-win tables this can leave many 64-byte blocks only half-filled, so
        // in such cases blocks are 32 bytes long. The blocks of DTZ tables are up to 1024 bytes long.
        // The generator picks the size that leads to the smallest table. The "book" of symbols and
        // Huffman codes is the same for all blocks in the table. A non-symmetric pawnless TB file
        // will have one table for wtm and one for btm, a TB file with pawns will have tables per
        // file a,b,c,d also in this case one set for wtm and one for btm.
        i32 decompress_pairs (PairsData* d, u64 idx)
        {
            // Special case where all table positions store the same value
            if (d->flags & SINGLE_VALUE)
            {
                return d->min_sym_len;
            }

            // First we need to locate the right block that stores the value at index "idx".
            // Because each block n stores block_length[n] + 1 values, the index i of the block
            // that contains the value at position idx is:
            //
            //                    for (i = -1, sum = 0; sum <= idx; i++)
            //                        sum += block_length[i + 1] + 1;
            //
            // This can be slow, so we use SparseIndex[] populated with a set of SparseEntry that
            // point to known indices into block_length[]. Namely SparseIndex[k] is a SparseEntry
            // that stores the block_length[] index and the offset within that block of the value
            // with index I(k), where:
            //
            //       I(k) = k * d->span + d->span / 2      (1)

            // First step is to get the 'k' of the I(k) nearest to our idx, using defintion (1)
            u32 k = u32(idx / d->span);

            // Then we read the corresponding SparseIndex[] entry
            u32 block  = number<u32, LittleEndian> (&d->sparseIndex[k].block);
            i32 offset = number<u16, LittleEndian> (&d->sparseIndex[k].offset);

            // Now compute the difference idx - I(k). From defintion of k we know that
            //
            //       idx = k * d->span + idx % d->span    (2)
            //
            // So from (1) and (2) we can compute idx - I(K):
            i32 diff = i32(idx % d->span - d->span / 2);

            // Sum the above to offset to find the offset corresponding to our idx
            offset += diff;

            // Move to previous/next block, until we reach the correct block that contains idx,
            // that is when 0 <= offset <= d->block_length[block]
            while (offset < 0)
            {
                offset += d->block_length[--block] + 1;
            }

            while (offset > d->block_length[block])
            {
                offset -= d->block_length[block++] + 1;
            }

            // Finally, we find the start address of our block of canonical Huffman symbols
            u32* ptr = (u32*) (d->data + block * d->block_size);

            // Read the first 64 bits in our block, this is a (truncated) sequence of
            // unknown number of symbols of unknown length but we know the first one
            // is at the beginning of this 64 bits sequence.
            u64 buf64 = number<u64, BigEndian> (ptr);
            ptr += 2;
            i32 buf64Size = 64;
            Sym sym;

            while (true)
            {
                i32 len = 0; // This is the symbol length - d->min_sym_len

                             // Now get the symbol length. For any symbol s64 of length l right-padded
                             // to 64 bits we know that d->base64[l-1] >= s64 >= d->base64[l] so we
                             // can find the symbol length iterating through base64[].
                while (buf64 < d->base64[len])
                {
                    ++len;
                }

                // All the symbols of a given length are consecutive integers (numerical
                // sequence property), so we can compute the offset of our symbol of
                // length len, stored at the beginning of buf64.
                sym = Sym((buf64 - d->base64[len]) >> (64 - len - d->min_sym_len));

                // Now add the value of the lowest symbol of length len to get our symbol
                sym += number<Sym, LittleEndian> (&d->lowest_sym[len]);

                // If our offset is within the number of values represented by symbol sym
                if (offset < d->sym_len[sym] + 1)
                {
                    break;
                }

                // ...otherwise update the offset and continue to iterate
                offset -= d->sym_len[sym] + 1;
                len += d->min_sym_len;  // Get the real length
                buf64 <<= len;          // Consume the just processed symbol
                buf64Size -= len;

                if (buf64Size <= 32)
                { 
                    // Refill the buffer
                    buf64Size += 32;
                    buf64 |= u64(number<u32, BigEndian> (ptr++)) << (64 - buf64Size);
                }
            }

            // Ok, now we have our symbol that expands into d->sym_len[sym] + 1 symbols.
            // We binary-search for our value recursively expanding into the left and
            // right child symbols until we reach a leaf node where sym_len[sym] + 1 == 1
            // that will store the value we need.
            while (d->sym_len[sym] != 0)
            {
                Sym left = d->btree[sym].get<LR::Left> ();

                // If a symbol contains 36 sub-symbols (d->sym_len[sym] + 1 = 36) and
                // expands in a pair (d->sym_len[left] = 23, d->sym_len[right] = 11), then
                // we know that, for instance the ten-th value (offset = 10) will be on
                // the left side because in Recursive Pairing child symbols are adjacent.
                if (offset < d->sym_len[left] + 1)
                {
                    sym = left;
                }
                else
                {
                    offset -= d->sym_len[left] + 1;
                    sym = d->btree[sym].get<LR::Right> ();
                }
            }

            return d->btree[sym].get<LR::Center> ();
        }

        bool check_dtz_stm (WDLEntry *     , i32    , File  ) { return true; }

        bool check_dtz_stm (DTZEntry *entry, i32 stm, File f)
        {
            i32 flags = entry->has_pawns ?
                entry->pawn_table.file[f].precomp->flags :
                entry->piece_table.precomp->flags;

            return (flags & STM) == stm
                || ((entry->key1 == entry->key2) && !entry->has_pawns);
        }

        // DTZ scores are sorted by frequency of occurrence and then assigned the
        // values 0, 1, 2, ... in order of decreasing frequency. This is done for each
        // of the four WDLScore values. The mapping information necessary to reconstruct
        // the original values is stored in the TB file and read during map[] init.
        WDLScore map_score (WDLEntry*, File, i32 value, WDLScore) { return WDLScore(value - 2); }

        i32 map_score (DTZEntry* entry, File f, i32 value, WDLScore wdl)
        {
            const i32 WDLMap[] = { 1, 3, 0, 2, 0 };

            i32 flags = entry->has_pawns ?
                entry->pawn_table.file[f].precomp->flags :
                entry->piece_table.precomp->flags;

            u08 *map = entry->has_pawns ?
                entry->pawn_table.map :
                entry->piece_table.map;

            u16* idx = entry->has_pawns ?
                entry->pawn_table.file[f].map_idx :
                entry->piece_table.map_idx;
            if (flags & MAPPED)
            {
                value = map[idx[WDLMap[wdl + 2]] + value];
            }

            // DTZ tables store distance to zero in number of moves or plies. We
            // want to return plies, so we have convert to plies when needed.
            if (   (wdl == WDLWin  && !(flags & WIN_PLIES))
                || (wdl == WDLLoss && !(flags & LOSS_PLIES))
                ||  wdl == WDLCursedWin
                ||  wdl == WDLCursedLoss)
            {
                value *= 2;
            }

            return value + 1;
        }

        // Compute a unique index out of a position and use it to probe the TB file. To
        // encode k pieces of same type and color, first sort the pieces by square in
        // ascending order s1 <= s2 <= ... <= sk then compute the unique index as:
        //
        //      idx = Binomial[1][s1] + Binomial[2][s2] + ... + Binomial[k][sk]
        //
        template<typename Entry, typename T = typename Ret<Entry>::type>
        T do_probe_table (const Position &pos, Entry *entry, WDLScore wdl, ProbeState &state)
        {
            const bool IsWDL = is_same<Entry, WDLEntry>::value;

            Square squares[TBPIECES];
            Piece pieces[TBPIECES];
            u64 idx;
            i32 next = 0, size = 0, lead_pawn_count = 0;
            PairsData* d;
            Bitboard b, lead_pawns = 0;
            File tbFile = F_A;

            bool flip =
                // Black Symmetric
                // A given TB entry like KRK has associated two material keys: KRvk and Kvkr.
                // If both sides have the same pieces keys are equal. In this case TB tables
                // only store the 'white to move' case, so if the position to lookup has black
                // to move, we need to switch the color and flip the squares before to lookup.
                        (   pos.active == BLACK
                         && entry->key1 == entry->key2)
                // Black Stronger
                // TB files are calculated for white as stronger side. For instance we have
                // KRvK, not KvKR. A position where stronger side is white will have its
                // material key == entry->key, otherwise we have to switch the color and
                // flip the squares before to lookup.
                    || (pos.si->matl_key != entry->key1);

            // For pawns, TB files store 4 separate tables according if leading pawn is on
            // file a, b, c or d after reordering. The leading pawn is the one with maximum
            // MapPawns[] value, that is the one most toward the edges and with lowest rank.
            if (entry->has_pawns)
            {
                // In all the 4 tables, pawns are at the beginning of the piece sequence and
                // their color is the reference one. So we just pick the first one.
                Piece pc = flip ?
                    ~item (entry->pawn_table, 0, 0).precomp->pieces[0] :
                     item (entry->pawn_table, 0, 0).precomp->pieces[0];

                assert(ptype (pc) == PAWN);

                lead_pawns = b = pos.pieces (color (pc), PAWN);
                while (b != 0)
                {
                    squares[size++] = flip ? ~pop_lsq (b) : pop_lsq (b);
                }
                lead_pawn_count = size;

                std::swap (squares[0], *std::max_element (squares, squares + lead_pawn_count, pawns_comp));

                tbFile = _file (squares[0]);
                if (tbFile > F_D)
                {
                    tbFile = _file (!squares[0]); // Horizontal flip: SQ_H1 -> SQ_A1
                }

                d = item (entry->pawn_table, flip ? ~pos.active : pos.active, tbFile).precomp;
            }
            else
            {
                d = item (entry->piece_table, flip ? ~pos.active : pos.active, tbFile).precomp;
            }

            // DTZ tables are one-sided, i.e. they store positions only for white to
            // move or only for black to move, so check for side to move to be color,
            // early exit otherwise.
            if (!IsWDL && !check_dtz_stm (entry, flip ? ~pos.active : pos.active, tbFile))
            {
                return state = CHANGE_STM, T ();
            }

            // Now we are ready to get all the position pieces (but the lead pawns) and
            // directly map them to the correct color and square.
            b = pos.pieces () ^ lead_pawns;
            while (b != 0)
            {
                auto s = pop_lsq (b);
                squares[size] = flip ? ~s : s;
                pieces[size] = Piece(flip ? ~pos[s] : pos[s]);
                ++size;
            }

            // Then we reorder the pieces to have the same sequence as the one stored
            // in precomp->pieces[i]: the sequence that ensures the best compression.
            for (i32 i = lead_pawn_count; i < size; ++i)
            {
                for (i32 j = i; j < size; ++j)
                {
                    if (d->pieces[i] == pieces[j])
                    {
                        std::swap (pieces[i], pieces[j]);
                        std::swap (squares[i], squares[j]);
                        break;
                    }
                }
            }

            // Now we map again the squares so that the square of the lead piece is in
            // the triangle A1-D1-D4.
            if (_file (squares[0]) > F_D)
            {
                for (i32 i = 0; i < size; ++i)
                {
                    squares[i] = !squares[i]; // Horizontal flip: SQ_H1 -> SQ_A1
                }
            }

            // Encode leading pawns starting with the one with minimum MapPawns[] and
            // proceeding in ascending order.
            if (entry->has_pawns)
            {
                idx = LeadPawnIdx[lead_pawn_count][squares[0]];

                std::sort (squares + 1, squares + lead_pawn_count, pawns_comp);

                for (i32 i = 1; i < lead_pawn_count; ++i)
                {
                    idx += Binomial[i][MapPawns[squares[i]]];
                }

                goto encode_remaining; // With pawns we have finished special treatments
            }

            // In positions withouth pawns, we further flip the squares to ensure leading
            // piece is below RANK_5.
            if (_rank (squares[0]) > R_4)
            {
                for (i32 i = 0; i < size; ++i)
                {
                    squares[i] = ~squares[i]; // Vertical flip: SQ_A8 -> SQ_A1
                }
            }
            // Look for the first piece of the leading group not on the A1-D4 diagonal
            // and ensure it is mapped below the diagonal.
            for (i32 i = 0; i < d->group_len[0]; ++i)
            {
                if (!off_A1H8 (squares[i]))
                {
                    continue;
                }

                if (off_A1H8 (squares[i]) > 0) // A1-H8 diagonal flip: SQ_A3 -> SQ_C3
                {
                    for (i32 j = i; j < size; ++j)
                    {
                        squares[j] = Square(((squares[j] >> 3) | (squares[j] << 3)) & 63);
                    }
                }
                break;
            }

            // Encode the leading group.
            //
            // Suppose we have KRvK. Let's say the pieces are on square numbers wK, wR
            // and bK (each 0...63). The simplest way to map this position to an index
            // is like this:
            //
            //   index = wK * 64 * 64 + wR * 64 + bK;
            //
            // But this way the TB is going to have 64*64*64 = 262144 positions, with
            // lots of positions being equivalent (because they are mirrors of each
            // other) and lots of positions being invalid (two pieces on one square,
            // adjacent kings, etc.).
            // Usually the first step is to take the wK and bK together. There are just
            // 462 ways legal and not-mirrored ways to place the wK and bK on the board.
            // Once we have placed the wK and bK, there are 62 squares left for the wR
            // Mapping its square from 0..63 to available squares 0..61 can be done like:
            //
            //   wR -= (wR > wK) + (wR > bK);
            //
            // In words: if wR "comes later" than wK, we deduct 1, and the same if wR
            // "comes later" than bK. In case of two same pieces like KRRvK we want to
            // place the two Rs "together". If we have 62 squares left, we can place two
            // Rs "together" in 62 * 61 / 2 ways (we divide by 2 because rooks can be
            // swapped and still get the same position.)
            //
            // In case we have at least 3 unique pieces (inlcuded kings) we encode them
            // together.
            if (entry->has_unique_pieces)
            {
                i32 adjust1 =  squares[1] > squares[0];
                i32 adjust2 = (squares[2] > squares[0]) + (squares[2] > squares[1]);

                // First piece is below a1-h8 diagonal. MapA1D1D4[] maps the b1-d1-d3
                // triangle to 0...5. There are 63 squares for second piece and and 62
                // (mapped to 0...61) for the third.
                if (off_A1H8 (squares[0]))
                {
                    idx = MapA1D1D4[squares[0]]  * 63 * 62
                        + (squares[1] - adjust1) * 62
                        +  squares[2] - adjust2;
                }
                // First piece is on a1-h8 diagonal, second below: map this occurence to
                // 6 to differentiate from the above case, rank() maps a1-d4 diagonal
                // to 0...3 and finally MapB1H1H7[] maps the b1-h1-h7 triangle to 0..27.
                else
                if (off_A1H8 (squares[1]))
                {
                    idx = 6 * 63 * 62
                        + _rank (squares[0]) * 28 * 62
                        + MapB1H1H7[squares[1]] * 62
                        + squares[2] - adjust2;
                }
                // First two pieces are on a1-h8 diagonal, third below
                else
                if (off_A1H8 (squares[2]))
                {
                    idx =  6 * 63 * 62 + 4 * 28 * 62
                        +  _rank (squares[0]) * 7 * 28
                        + (_rank (squares[1]) - adjust1) * 28
                        +  MapB1H1H7[squares[2]];
                }
                // All 3 pieces on the diagonal a1-h8
                else
                {
                    idx = 6 * 63 * 62 + 4 * 28 * 62 + 4 * 7 * 28
                        +  _rank (squares[0]) * 7 * 6
                        + (_rank (squares[1]) - adjust1) * 6
                        + (_rank (squares[2]) - adjust2);
                }
            }
            else
            {
                // We don't have at least 3 unique pieces, like in KRRvKBB, just map
                // the kings.
                idx = MapKK[MapA1D1D4[squares[0]]][squares[1]];
            }

        encode_remaining:
            idx *= d->group_idx[0];
            auto *group_sq = squares + d->group_len[0];

            // Encode remainig pawns then pieces according to square, in ascending order
            bool pawn_remain = entry->has_pawns && entry->pawn_table.pawn_count[1];

            while (d->group_len[++next] != 0)
            {
                std::sort (group_sq, group_sq + d->group_len[next]);
                u64 n = 0;

                // Map down a square if "comes later" than a square in the previous
                // groups (similar to what done earlier for leading group pieces).
                for (i32 i = 0; i < d->group_len[next]; ++i)
                {
                    auto f = [&](Square s) { return group_sq[i] > s; };
                    auto adjust = std::count_if (squares, group_sq, f);
                    n += Binomial[i + 1][group_sq[i] - adjust - 8 * pawn_remain];
                }

                pawn_remain = false;
                idx += n * d->group_idx[next];
                group_sq += d->group_len[next];
            }

            // Now that we have the index, decompress the pair and get the score
            return map_score (entry, tbFile, decompress_pairs (d, idx), wdl);
        }

        // Group together pieces that will be encoded together. The general rule is that
        // a group contains pieces of same type and color. The exception is the leading
        // group that, in case of positions withouth pawns, can be formed by 3 different
        // pieces (default) or by the king pair when there is not a unique piece apart
        // from the kings. When there are pawns, pawns are always first in pieces[].
        //
        // As example KRKN -> KRK + N, KNNK -> KK + NN, KPPKP -> P + PP + K + K
        //
        // The actual grouping depends on the TB generator and can be inferred from the
        // sequence of pieces in piece[] array.
        template<typename T>
        void set_groups (T& e, PairsData* d, i32 order[], File f)
        {
            i32 n = 0, firstLen = e.has_pawns ? 0 : e.has_unique_pieces ? 3 : 2;
            d->group_len[n] = 1;

            // Number of pieces per group is stored in group_len[], for instance in KRKN
            // the encoder will default on '111', so group_len[] will be (3, 1).
            for (i32 i = 1; i < e.piece_count; ++i)
            {
                if (--firstLen > 0 || d->pieces[i] == d->pieces[i - 1])
                {
                    d->group_len[n]++;
                }
                else
                {
                    d->group_len[++n] = 1;
                }
            }
            d->group_len[++n] = 0; // Zero-terminated

            // The sequence in pieces[] defines the groups, but not the order in which
            // they are encoded. If the pieces in a group g can be combined on the board
            // in N(g) different ways, then the position encoding will be of the form:
            //
            //           g1 * N(g2) * N(g3) + g2 * N(g3) + g3
            //
            // This ensures unique encoding for the whole position. The order of the
            // groups is a per-table parameter and could not follow the canonical leading
            // pawns/pieces -> remainig pawns -> remaining pieces. In particular the
            // first group is at order[0] position and the remaining pawns, when present,
            // are at order[1] position.
            bool pp = e.has_pawns && e.pawn_table.pawn_count[1]; // Pawns on both sides
            i32 next = pp ? 2 : 1;
            i32 free_squares = 64 - d->group_len[0] - (pp ? d->group_len[1] : 0);
            u64 idx = 1;

            for (i32 k = 0; next < n || k == order[0] || k == order[1]; ++k)
            {
                if (k == order[0]) // Leading pawns or pieces
                {
                    d->group_idx[0] = idx;
                    idx *= e.has_pawns ?
                        LeadPawnsSize[d->group_len[0]][f] :
                        e.has_unique_pieces ? 31332 : 462;
                }
                else
                if (k == order[1]) // Remaining pawns
                {
                    d->group_idx[1] = idx;
                    idx *= Binomial[d->group_len[1]][48 - d->group_len[0]];
                }
                else // Remainig pieces
                {
                    d->group_idx[next] = idx;
                    idx *= Binomial[d->group_len[next]][free_squares];
                    free_squares -= d->group_len[next++];
                }
            }
            d->group_idx[n] = idx;
        }

        // In Recursive Pairing each symbol represents a pair of childern symbols. So
        // read d->btree[] symbols data and expand each one in his left and right child
        // symbol until reaching the leafs that represent the symbol value.
        u08 set_symlen (PairsData* d, Sym s, vector<bool>& visited)
        {
            visited[s] = true; // We can set it now because tree is acyclic
            Sym sr = d->btree[s].get<LR::Right> ();

            if (sr == 0xFFF)
            {
                return 0;
            }

            Sym sl = d->btree[s].get<LR::Left> ();

            if (!visited[sl])
            {
                d->sym_len[sl] = set_symlen (d, sl, visited);
            }
            if (!visited[sr])
            {
                d->sym_len[sr] = set_symlen (d, sr, visited);
            }

            return d->sym_len[sl] + d->sym_len[sr] + 1;
        }

        u08* set_sizes (PairsData* d, u08 *data)
        {
            d->flags = *data++;

            if (d->flags & SINGLE_VALUE)
            {
                d->num_blocks = 0;
                d->span = 0;
                d->block_length_size = 0;
                d->sparse_index_size = 0; // Broken MSVC zero-init
                d->min_sym_len = *data++; // Here we store the single value
                return data;
            }

            // group_len[] is a zero-terminated list of group lengths, the last group_idx[]
            // element stores the biggest index that is the tb size.
            u64 tbSize = d->group_idx[std::find (d->group_len, d->group_len + 7, 0) - d->group_len];

            d->block_size = 1ULL << *data++;
            d->span = 1ULL << *data++;
            d->sparse_index_size = (tbSize + d->span - 1) / d->span; // Round up
            i32 padding = number<u08, LittleEndian> (data++);
            d->num_blocks = number<u32, LittleEndian> (data); data += sizeof (u32);
            d->block_length_size = d->num_blocks + padding; // Padded to ensure SparseIndex[]
                                                         // does not point out of range.
            d->max_sym_len = *data++;
            d->min_sym_len = *data++;
            d->lowest_sym = (Sym*) data;
            d->base64.resize (d->max_sym_len - d->min_sym_len + 1);

            // The canonical code is ordered such that longer symbols (in terms of
            // the number of bits of their Huffman code) have lower numeric value,
            // so that d->lowest_sym[i] >= d->lowest_sym[i+1] (when read as LittleEndian).
            // Starting from this we compute a base64[] table indexed by symbol length
            // and containing 64 bit values so that d->base64[i] >= d->base64[i+1].
            // See http://www.eecs.harvard.edu/~michaelm/E210/huffman.pdf
            for (i32 i = i32(d->base64.size () - 2); i >= 0; --i)
            {
                d->base64[i] = (  d->base64[i + 1]
                                + number<Sym, LittleEndian> (&d->lowest_sym[i])
                                - number<Sym, LittleEndian> (&d->lowest_sym[i + 1])) / 2;

                assert(d->base64[i] * 2 >= d->base64[i+1]);
            }

            // Now left-shift by an amount so that d->base64[i] gets shifted 1 bit more
            // than d->base64[i+1] and given the above assert condition, we ensure that
            // d->base64[i] >= d->base64[i+1]. Moreover for any symbol s64 of length i
            // and right-padded to 64 bits holds d->base64[i-1] >= s64 >= d->base64[i].
            for (size_t i = 0; i < d->base64.size (); ++i)
            {
                d->base64[i] <<= 64 - i - d->min_sym_len; // Right-padding to 64 bits
            }
            data += d->base64.size () * sizeof (Sym);
            d->sym_len.resize (number<u16, LittleEndian> (data)); data += sizeof (u16);
            d->btree = (LR*) data;

            // The comrpession scheme used is "Recursive Pairing", that replaces the most
            // frequent adjacent pair of symbols in the source message by a new symbol,
            // reevaluating the frequencies of all of the symbol pairs with respect to
            // the extended alphabet, and then repeating the process.
            // See http://www.larsson.dogma.net/dcc99.pdf
            vector<bool> visited (d->sym_len.size ());

            for (Sym sym = 0; sym < d->sym_len.size (); ++sym)
            {
                if (!visited[sym])
                {
                    d->sym_len[sym] = set_symlen (d, sym, visited);
                }
            }
            return data + d->sym_len.size () * sizeof (LR) + (d->sym_len.size () & 1);
        }

        template<typename T>
        u08* set_dtz_map (WDLEntry&, T&, u08*, File)
        {
            return nullptr;
        }

        template<typename T>
        u08* set_dtz_map (DTZEntry&, T& p, u08 *data, File maxFile)
        {
            p.map = data;
            for (auto f = F_A; f <= maxFile; ++f)
            {
                if (item (p, 0, f).precomp->flags & MAPPED)
                {
                    for (i32 i = 0; i < 4; ++i)
                    { // Sequence like 3,x,x,x,1,x,0,2,x,x
                        item (p, 0, f).map_idx[i] = u16(data - p.map + 1);
                        data += *data + 1;
                    }
                }
            }
            return data += (uintptr_t) data & 1; // Word alignment
        }

        template<typename Entry, typename T>
        void do_init (Entry& e, T& p, u08 *data)
        {
            enum
            {
                Split = 1,
                HasPawns = 2
            };

            const bool IsWDL = is_same<Entry, WDLEntry>::value;
            assert(e.has_pawns        == !!(*data & HasPawns));
            assert((e.key1 != e.key2) == !!(*data & Split));

            data++; // First byte stores flags

            const i32  Sides   = IsWDL && (e.key1 != e.key2) ? 2 : 1;
            const File MaxFile = e.has_pawns ? F_D : F_A;

            bool pp = e.has_pawns && e.pawn_table.pawn_count[1]; // Pawns on both sides

            assert(!pp || e.pawn_table.pawn_count[0]);

            for (auto f = F_A; f <= MaxFile; ++f)
            {
                for (i32 i = 0; i < Sides; i++)
                {
                    item (p, i, f).precomp = new PairsData ();
                }

                i32 order[][2] = 
                { 
                    { *data & 0xF, pp ? *(data + 1) & 0xF : 0xF },
                    { *data >>  4, pp ? *(data + 1) >>  4 : 0xF }
                };
                data += 1 + pp;

                for (i32 k = 0; k < e.piece_count; ++k, ++data)
                {
                    for (i32 i = 0; i < Sides; i++)
                    {
                        item (p, i, f).precomp->pieces[k] = tb_piece (i ? *data >>  4 : *data & 0xF);
                    }
                }

                for (i32 i = 0; i < Sides; ++i)
                {
                    set_groups (e, item (p, i, f).precomp, order[i], f);
                }
            }

            data += (uintptr_t) data & 1; // Word alignment

            for (auto f = F_A; f <= MaxFile; ++f)
            {
                for (i32 i = 0; i < Sides; i++)
                {
                    data = set_sizes (item (p, i, f).precomp, data);
                }
            }
            if (!IsWDL)
            {
                data = set_dtz_map (e, p, data, MaxFile);
            }

            PairsData *d;
            for (auto f = F_A; f <= MaxFile; ++f)
            {
                for (i32 i = 0; i < Sides; i++)
                {
                    (d = item (p, i, f).precomp)->sparseIndex = (SparseEntry*) data;
                    data += d->sparse_index_size * sizeof (SparseEntry);
                }
            }
            for (auto f = F_A; f <= MaxFile; ++f)
            {
                for (i32 i = 0; i < Sides; i++)
                {
                    (d = item (p, i, f).precomp)->block_length = (u16*) data;
                    data += d->block_length_size * sizeof (u16);
                }
            }
            for (auto f = F_A; f <= MaxFile; ++f)
            {
                for (i32 i = 0; i < Sides; i++)
                {
                    data = (u08*)(((uintptr_t) data + 0x3F) & ~0x3F); // 64 byte alignment
                    (d = item (p, i, f).precomp)->data = data;
                    data += d->num_blocks * d->block_size;
                }
            }
        }

        template<typename Entry>
        void* init (Entry &e, const Position &pos)
        {
            const bool IsWDL = is_same<Entry, WDLEntry>::value;

            static Mutex mutex;

            // Avoid a thread reads 'ready' == true while another is still in do_init(),
            // this could happen due to compiler reordering.
            if (e.ready.load (memory_order_acquire))
            {
                return e.base_address;
            }

            unique_lock<Mutex> lk (mutex);

            if (e.ready.load (memory_order_relaxed)) // Recheck under lock
            {
                return e.base_address;
            }

            // Pieces strings in decreasing order for each color, like ("KPP","KR")
            string w, b;
            for (auto pt = KING; pt >= PAWN; --pt)
            {
                w += string(pos.count (WHITE, pt), PieceToChar[pt]);
                b += string(pos.count (BLACK, pt), PieceToChar[pt]);
            }

            const u08 TB_MAGIC[][4] =
            {
                { 0xD7, 0x66, 0x0C, 0xA5 },
                { 0x71, 0xE8, 0x23, 0x5D }
            };

            u08 *data = TBFile ((e.key1 == pos.si->matl_key ? w + b : b + w), IsWDL ? ".rtbw" : ".rtbz").map (&e.base_address, &e.mapping, TB_MAGIC[IsWDL]);
            if (data != nullptr)
            {
                e.has_pawns ?
                    do_init (e, e.pawn_table, data) :
                    do_init (e, e.piece_table, data);
            }

            e.ready.store (true, memory_order_release);
            return e.base_address;
        }

        template<typename E, typename T = typename Ret<E>::type>
        T probe_table (const Position &pos, ProbeState &state, WDLScore wdl = WDLDraw)
        {
            if ((pos.pieces () ^ pos.pieces (KING)) == 0)
            {
                return T(WDLDraw); // KvK
            }

            E *entry = EntryTable.get<E> (pos.si->matl_key);

            if (   entry == nullptr
                || init (*entry, pos) == nullptr)
            {
                return state = FAIL, T();
            }

            return do_probe_table (pos, entry, wdl, state);
        }

        // For a position where the side to move has a winning capture it is not necessary
        // to store a winning value so the generator treats such positions as "don't cares"
        // and tries to assign to it a value that improves the compression ratio. Similarly,
        // if the side to move has a drawing capture, then the position is at least drawn.
        // If the position is won, then the TB needs to store a win value. But if the
        // position is drawn, the TB may store a loss value if that is better for compression.
        // All of this means that during probing, the engine must look at captures and probe
        // their results and must probe the position itself. The "best" state of these
        // probes is the correct state for the position.
        // DTZ table don't store values when a following move is a zeroing winning move
        // (winning capture or winning pawn move). Also DTZ store wrong values for positions
        // where the best move is an ep-move (even if losing). So in all these cases set
        // the state to ZEROING_BEST_MOVE.
        template<bool CheckZeroingMoves = false>
        WDLScore search (Position &pos, ProbeState &state)
        {
            auto move_list = MoveList<LEGAL> (pos);
            size_t move_count = 0
                ,  total_count = move_list.size ();

            WDLScore value, best_value = WDLLoss;
            StateInfo si;

            for (const Move &move : move_list)
            {
                if (   !pos.capture (move)
                    && (!CheckZeroingMoves || ptype (pos[org_sq (move)]) != PAWN))
                {
                    continue;
                }

                move_count++;

                pos.do_move (move, si);
                value = -search (pos, state);
                pos.undo_move (move);

                if (state == FAIL)
                {
                    return WDLDraw;
                }

                if (value > best_value)
                {
                    best_value = value;

                    if (value >= WDLWin)
                    {
                        state = ZEROING_BEST_MOVE; // Winning DTZ-zeroing move
                        return value;
                    }
                }
            }

            // In case we have already searched all the legal moves we don't have to probe
            // the TB because the stored score could be wrong. For instance TB tables
            // do not contain information on position with ep rights, so in this case
            // the state of probe_wdl_table is wrong. Also in case of only capture
            // moves, for instance here 4K3/4q3/6p1/2k5/6p1/8/8/8 w - - 0 7, we have to
            // return with ZEROING_BEST_MOVE set.
            bool no_more_moves = (move_count != 0 && move_count == total_count);

            if (no_more_moves)
            {
                value = best_value;
            }
            else
            {
                value = probe_table<WDLEntry> (pos, state);

                if (state == FAIL)
                {
                    return WDLDraw;
                }
            }

            // DTZ stores a "don't care" value if best_value is a win
            if (best_value >= value)
            {
                return state = (   best_value > WDLDraw
                                || no_more_moves ?
                                   ZEROING_BEST_MOVE : OK), best_value;
            }

            return state = OK, value;
        }

        // Check whether there has been at least one repetition of position since the last capture or pawn move.
        bool has_repeated (StateInfo *si)
        {
            while (si != nullptr)
            {
                auto p = std::min (si->clock_ply, si->null_ply);
                if (p < 4)
                {
                    break;
                }

                const auto *psi = si->ptr->ptr;
                do
                {
                    psi = psi->ptr->ptr;
                    // Check first repetition
                    if (psi->posi_key == si->posi_key)
                    {
                        return true;
                    }
                    p -= 2;
                }
                while (p >= 4);
                si = si->ptr;
            }
            return false;
        }

    } // namespace


    // Probe the DTZ table for a particular position.
    // If *result != FAIL, the probe was successful.
    // The return value is from the point of view of the side to move:
    //         n < -100 : loss, but draw under 50-move rule
    // -100 <= n < -1   : loss in n ply (assuming 50-move counter == 0)
    //         0        : draw
    //     1 < n <= 100 : win in n ply (assuming 50-move counter == 0)
    //   100 < n        : win, but draw under 50-move rule
    //
    // The return value n can be off by 1: a return value -n can mean a loss
    // in n+1 ply and a return value +n can mean a win in n+1 ply. This
    // cannot happen for tables with positions exactly on the "edge" of
    // the 50-move rule.
    //
    // This implies that if dtz > 0 is returned, the position is certainly
    // a win if dtz + 50-move-counter <= 99. Care must be taken that the engine
    // picks moves that preserve dtz + 50-move-counter <= 99.
    //
    // If n = 100 immediately after a capture or pawn move, then the position
    // is also certainly a win, and during the whole phase until the next
    // capture or pawn move, the inequality to be preserved is
    // dtz + 50-movecounter <= 100.
    //
    // In short, if a move is available resulting in dtz + 50-move-counter <= 99,
    // then do not accept moves leading to dtz + 50-move-counter == 100.
    i32      probe_dtz (Position &pos, ProbeState &state)
    {
        state = OK;
        WDLScore wdl = search<true> (pos, state);

        if (state == FAIL || wdl == WDLDraw) // DTZ tables don't store draws
        {
            return 0;
        }

        // DTZ stores a 'don't care' value in this case, or even a plain wrong
        // one as in case the best move is a losing ep, so it cannot be probed.
        if (state == ZEROING_BEST_MOVE)
        {
            return dtz_before_zeroing (wdl);
        }

        i32 dtz = probe_table<DTZEntry> (pos, state, wdl);

        if (state == FAIL)
        {
            return 0;
        }

        if (state != CHANGE_STM)
        {
            return (dtz + 100 * (wdl == WDLCursedLoss || wdl == WDLCursedWin)) * sign (wdl);
        }

        // DTZ stores results for the other side, so we need to do a 1-ply search and
        // find the winning move that minimizes DTZ.
        StateInfo si;
        i32 minDTZ = 0xFFFF;

        for (const Move &move : MoveList<LEGAL> (pos))
        {
            bool zeroing = pos.capture (move) || ptype (pos[org_sq (move)]) == PAWN;

            pos.do_move (move, si);

            // For zeroing moves we want the dtz of the move _before_ doing it,
            // otherwise we will get the dtz of the next move sequence. Search the
            // position after the move to get the score sign (because even in a
            // winning position we could make a losing capture or going for a draw).
            dtz = zeroing ?
                -dtz_before_zeroing (search (pos, state)) :
                -probe_dtz (pos, state);

            pos.undo_move (move);

            if (state == FAIL)
            {
                return 0;
            }

            // Convert state from 1-ply search. Zeroing moves are already accounted
            // by dtz_before_zeroing() that returns the DTZ of the previous move.
            if (!zeroing)
            {
                dtz += sign (dtz);
            }

            // Skip the draws and if we are winning only pick positive dtz
            if (   minDTZ > dtz
                && sign (dtz) == sign (wdl))
            {
                minDTZ = dtz;
            }
        }

        // Special handle a mate position, when there are no legal moves, in this
        // case return value is somewhat arbitrary, so stick to the original TB code
        // that returns -1 in this case.
        return minDTZ == 0xFFFF ? -1 : minDTZ;
    }

    // Probe the WDL table for a particular position.
    // If *result != FAIL, the probe was successful.
    // The return value is from the point of view of the side to move:
    // -2 : loss
    // -1 : loss, but draw under 50-move rule
    //  0 : draw
    //  1 : win, but draw under 50-move rule
    //  2 : win
    WDLScore probe_wdl (Position &pos, ProbeState &state)
    {
        state = OK;
        return search (pos, state);
    }

    // Use the DTZ tables to filter out moves that don't preserve the win or draw.
    // If the position is lost, but DTZ is fairly high, only keep moves that
    // maximise DTZ.
    //
    // A return value false indicates that not all probes were successful and that
    // no moves were filtered out.
    bool root_probe_dtz (Position &root_pos, RootMoveVector &root_moves, Value &value)
    {
        ProbeState state;
        i32 dtz = probe_dtz (root_pos, state);

        if (state == FAIL)
        {
            return false;
        }

        StateInfo si;
        // Probe each move
        for (size_t i = 0; i < root_moves.size (); ++i)
        {
            Move move = root_moves[i][0];
            root_pos.do_move (move, si);
            i32 v = 0;

            if (   root_pos.si->checkers != 0
                && dtz > 0)
            {
                vector<ValMove> moves;
                generate<LEGAL> (moves, root_pos);
                if (moves.size () == 0)
                {
                    v = 1;
                }
            }

            if (v == 0)
            {
                if (si.clock_ply != 0)
                {
                    v = -probe_dtz (root_pos, state);

                    if (v > 0)
                    {
                        ++v;
                    }
                    else
                    if (v < 0)
                    {
                        --v;
                    }
                }
                else
                {
                    v = -probe_wdl (root_pos, state);
                    v = dtz_before_zeroing (WDLScore(v));
                }
            }

            root_pos.undo_move (move);

            if (state == FAIL)
            {
                return false;
            }

            root_moves[i].new_value = Value(v);
        }

        // Obtain 50-move counter for the root position.
        // In Stockfish there seems to be no clean way, so we do it like this:
        i32 cnt50 = si.ptr != nullptr ? si.ptr->clock_ply : 0;

        // Use 50-move counter to determine whether the root position is
        // won, lost or drawn.
        i32 wdl = 0;
        if (dtz > 0)
        {
            wdl = +dtz + cnt50 <= 100 ? +2 : +1;
        }
        else
        if (dtz < 0)
        {
            wdl = -dtz + cnt50 <= 100 ? -2 : -1;
        }

        // Determine the score to report to the user.
        value = WDL_To_Value[wdl + 2];

        // If the position is winning or losing, but too few moves left, adjust the
        // score to show how close it is to winning or losing.
        // NOTE: i32(PawnValueEg) is used as scaling factor in score_to_uci().
        if (wdl == +1 && dtz <= +100)
        {
            value = +Value(((200 - dtz - cnt50) * i32(VALUE_EG_PAWN)) / 200);
        }
        else
        if (wdl == -1 && dtz >= -100)
        {
            value = -Value(((200 + dtz - cnt50) * i32(VALUE_EG_PAWN)) / 200);
        }

        // Now be a bit smart about filtering out moves.
        size_t size = 0;
        if (dtz > 0)
        { // winning (or 50-move rule draw)
            i32 best = 0xffff;

            for (size_t i = 0; i < root_moves.size (); ++i)
            {
                i32 v = root_moves[i].new_value;
                if (   v > 0
                    && best > v)
                {
                    best = v;
                }
            }

            i32 max = best;

            // If the current phase has not seen repetitions, then try all moves
            // that stay safely within the 50-move budget, if there are any.
            if (   !has_repeated (si.ptr)
                && best + cnt50 <= 99)
            {
                max = 99 - cnt50;
            }
            for (size_t i = 0; i < root_moves.size (); ++i)
            {
                i32 v = root_moves[i].new_value;

                if (   v > 0
                    && v <= max)
                {
                    root_moves[size++] = root_moves[i];
                }
            }
        }
        else
        if (dtz < 0)
        { // losing (or 50-move rule draw)
            i32 best = 0;

            for (size_t i = 0; i < root_moves.size (); ++i)
            {
                i32 v = root_moves[i].new_value;
                if (best > v)
                {
                    best = v;
                }
            }

            // Try all moves, unless we approach or have a 50-move rule draw.
            if (-best * 2 + cnt50 < 100)
            {
                return true;
            }

            for (size_t i = 0; i < root_moves.size (); ++i)
            {
                if (root_moves[i].new_value == best)
                {
                    root_moves[size++] = root_moves[i];
                }
            }
        }
        else
        { // drawing
          // Try all moves that preserve the draw.
            for (size_t i = 0; i < root_moves.size (); ++i)
            {
                if (root_moves[i].new_value == 0)
                {
                    root_moves[size++] = root_moves[i];
                }
            }
        }
        root_moves.resize (size, RootMove (MOVE_NONE));

        return true;
    }

    // Use the WDL tables to filter out moves that don't preserve the win or draw.
    // This is a fallback for the case that some or all DTZ tables are missing.
    //
    // A return value false indicates that not all probes were successful and that
    // no moves were filtered out.
    bool root_probe_wdl (Position &root_pos, RootMoveVector &root_moves, Value &value)
    {
        ProbeState state;

        WDLScore wdl = probe_wdl (root_pos, state);

        if (state == FAIL)
        {
            return false;
        }

        value = WDL_To_Value[wdl + 2];

        StateInfo si;

        i32 best = WDLLoss;

        // Probe each move
        for (size_t i = 0; i < root_moves.size (); ++i)
        {
            Move move = root_moves[i][0];
            root_pos.do_move (move, si);
            WDLScore v = -probe_wdl (root_pos, state);
            root_pos.undo_move (move);

            if (state == FAIL)
            {
                return false;
            }

            root_moves[i].new_value = Value(v);

            if (best < v)
            {
                best = v;
            }
        }

        size_t size = 0;
        for (size_t i = 0; i < root_moves.size (); ++i)
        {
            if (root_moves[i].new_value == best)
            {
                root_moves[size++] = root_moves[i];
            }
        }
        root_moves.resize (size, RootMove (MOVE_NONE));

        return true;
    }

    // Initialize TB
    void initialize ()
    {
        static bool initialized = false;
        if (!initialized)
        {
            // MapB1H1H7[] encodes a square below a1-h8 diagonal to 0..27
            i32 code = 0;
            for (auto s = SQ_A1; s <= SQ_H8; ++s)
            {
                if (off_A1H8 (s) < 0)
                {
                    MapB1H1H7[s] = code++;
                }
            }
            // MapA1D1D4[] encodes a square in the a1-d1-d4 triangle to 0..9
            vector<Square> diagonal;
            code = 0;
            for (auto s = SQ_A1; s <= SQ_D4; ++s)
            {
                if (off_A1H8 (s) < 0 && _file (s) <= F_D)
                {
                    MapA1D1D4[s] = code++;
                }
                else
                if (!off_A1H8 (s) && _file (s) <= F_D)
                {
                    diagonal.push_back (s);
                }
            }
            // Diagonal squares are encoded as last ones
            for (auto s : diagonal)
            {
                MapA1D1D4[s] = code++;
            }
            // MapKK[] encodes all the 461 possible legal positions of two kings where the first is in the a1-d1-d4 triangle.
            // If the first king is on the a1-d4 diagonal, the other one shall not to be above the a1-h8 diagonal.
            vector<pair<i32, Square>> both_on_diagonal;
            code = 0;
            for (i32 idx = 0; idx < 10; idx++)
            {
                for (auto s1 = SQ_A1; s1 <= SQ_D4; ++s1)
                {
                    if (MapA1D1D4[s1] == idx && (idx || s1 == SQ_B1)) // SQ_B1 is mapped to 0
                    {
                        for (auto s2 = SQ_A1; s2 <= SQ_H8; ++s2)
                        {
                            if (contains (PieceAttacks[KING][s1] | s1, s2))
                            {
                                continue; // Illegal position
                            }
                            else
                            if (!off_A1H8 (s1) && off_A1H8 (s2) > 0)
                            {
                                continue; // First on diagonal, second above
                            }
                            else
                            if (!off_A1H8 (s1) && !off_A1H8 (s2))
                            {
                                both_on_diagonal.push_back (std::make_pair (idx, s2));
                            }
                            else
                            {
                                MapKK[idx][s2] = code++;
                            }
                        }
                    }
                }
            }

            // Legal positions with both kings on diagonal are encoded as last ones
            for (auto p : both_on_diagonal)
            {
                MapKK[p.first][p.second] = code++;
            }

            // Binomial[] stores the Binomial Coefficents using Pascal rule. There
            // are Binomial[k][n] ways to choose k elements from a set of n elements.
            Binomial[0][0] = 1;

            for (i32 n = 1; n < 64; n++) // Squares
            {
                for (i32 k = 0; k < 6 && k <= n; ++k) // Pieces
                {
                    Binomial[k][n] =
                          (k > 0 ? Binomial[k - 1][n - 1] : 0)
                        + (k < n ? Binomial[k][n - 1] : 0);
                }
            }

            // MapPawns[s] encodes squares a2-h7 to 0..47. This is the number of possible
            // available squares when the leading one is in 's'. Moreover the pawn with
            // highest MapPawns[] is the leading pawn, the one nearest the edge and,
            // among pawns with same file, the one with lowest rank.
            i32 availableSquares = 47; // Available squares when lead pawn is in a2

                                        // Init the tables for the encoding of leading pawns group: with 6-men TB we
                                        // can have up to 4 leading pawns (KPPPPK).
            for (i32 lead_pawn_count = 1; lead_pawn_count <= 4; ++lead_pawn_count)
            {
                for (auto f = F_A; f <= F_D; ++f)
                {
                    // Restart the index at every file because TB table is splitted
                    // by file, so we can reuse the same index for different files.
                    i32 idx = 0;

                    // Sum all possible combinations for a given file, starting with
                    // the leading pawn on rank 2 and increasing the rank.
                    for (auto r = R_2; r <= R_7; ++r)
                    {
                        auto sq = f|r;

                        // Compute MapPawns[] at first pass.
                        // If sq is the leading pawn square, any other pawn cannot be
                        // below or more toward the edge of sq. There are 47 available
                        // squares when sq = a2 and reduced by 2 for any rank increase
                        // due to mirroring: sq == a3 -> no a2, h2, so MapPawns[a3] = 45
                        if (lead_pawn_count == 1)
                        {
                            MapPawns[sq] = availableSquares--;
                            MapPawns[sq ^ 7] = availableSquares--; // Horizontal flip
                        }
                        LeadPawnIdx[lead_pawn_count][sq] = idx;
                        idx += Binomial[lead_pawn_count - 1][MapPawns[sq]];
                    }
                    // After a file is traversed, store the cumulated per-file index
                    LeadPawnsSize[lead_pawn_count][f] = idx;
                }
            }
            initialized = true;
        }

        EntryTable.clear ();
        MaxLimitPiece = 0;

        if (   white_spaces (PathString)
            || PathString == Empty)
        {
            return;
        }

        // Multiple directories are separated by ";" on Windows and by ":" on Unix-based operating systems.
        //
        // Example:
        // C:\tb\wdl345;C:\tb\wdl6;D:\tb\dtz345;D:\tb\dtz6

        const char SepChar =
#       if !defined(_WIN32)
            ':';
#       else
            ';';
#       endif

        //TBFile::Paths = split (PathString, SepChar, false, true);
        TBFile::Paths.clear ();
        stringstream ss (PathString);
        string path;
        while (std::getline (ss, path, SepChar))
        {
            if (!white_spaces (path))
            {
                convert_path (path);
                TBFile::Paths.push_back (path);
            }
        }

        for (auto wp1 = PAWN; wp1 < KING; ++wp1)
        {
            EntryTable.insert ({ KING, wp1, KING });

            for (auto bp1 = PAWN; bp1 < KING; ++bp1)
            {
                EntryTable.insert ({ KING, wp1, KING, bp1 });
            }
            for (auto wp2 = PAWN; wp2 <= wp1; ++wp2)
            {
                EntryTable.insert ({ KING, wp1, wp2, KING });

                for (auto bp1 = PAWN; bp1 < KING; ++bp1)
                {
                    EntryTable.insert ({ KING, wp1, wp2, KING, bp1 });
                }
                for (auto wp3 = PAWN; wp3 <= wp2; ++wp3)
                {
                    EntryTable.insert ({ KING, wp1, wp2, wp3, KING });

                    for (auto bp1 = PAWN; bp1 < KING; ++bp1)
                    {
                        EntryTable.insert ({ KING, wp1, wp2, wp3, KING, bp1 });
                    }
                    for (auto wp4 = PAWN; wp4 <= wp3; ++wp4)
                    {
                        EntryTable.insert ({ KING, wp1, wp2, wp3, wp4, KING });

                        for (auto bp1 = PAWN; bp1 < KING; ++bp1)
                        {
                            EntryTable.insert ({ KING, wp1, wp2, wp3, wp4, KING, bp1 });
                        }
                        for (auto wp5 = PAWN; wp5 <= wp4; ++wp5)
                        {
                            EntryTable.insert ({ KING, wp1, wp2, wp3, wp4, wp5, KING });
                        }
                    }
                    for (auto bp1 = PAWN; bp1 < KING; ++bp1)
                    {
                        for (auto bp2 = PAWN; bp2 <= bp1; ++bp2)
                        {
                            EntryTable.insert ({ KING, wp1, wp2, wp3, KING, bp1, bp2 });
                        }
                    }
                }
                for (auto bp1 = PAWN; bp1 <= wp1; ++bp1)
                {
                    for (auto bp2 = PAWN; bp2 <= (wp1 == bp1 ? wp2 : bp1); ++bp2)
                    {
                        EntryTable.insert ({ KING, wp1, wp2, KING, bp1, bp2 });
                    }
                }
            }
        }

        sync_cout << "info string found " << EntryTable.size () << " tablebases" << sync_endl;
    }
}