#include "Engine.h"

#include <sstream>
#include <iomanip>

#include "BitBoard.h"
#include "BitBases.h"
#include "Pawns.h"
#include "Material.h"
#include "Evaluator.h"
#include "Searcher.h"
#include "Transposition.h"
#include "TB_Syzygy.h"
#include "DebugLogger.h"
#include "Thread.h"
#include "UCI.h"
#include "Tester.h"

namespace Engine {

    using namespace std;

    namespace {

        const string Name      = "DON";

        // Version number.
        // If Version is left empty, then compile date in the format DD-MM-YY.
        const string Version   = "";
        const string Author    = "Ehsan Rashid";

        const string Months ("Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec");

    }

    string info (bool uci)
    {
        ostringstream ss;

        if (uci) ss << "id name ";
        ss << Name << " ";

#if defined (VERSION)
        ss << VERSION << setfill ('0');
#else
        if (Version.empty ())
        {
            // From compiler, format is "Sep 2 2013"
            istringstream sdate (__DATE__);

            string month
                ,  day
                ,  year;

            sdate
                >> month
                >> day
                >> year;

            ss  << setfill ('0')
                << setw (2) << (day) //<< '-'
                << setw (2) << (Months.find (month) / 4 + 1) //<< '-'
                << setw (2) << (year.substr (2));
        }
        else
        {
            ss << Version << setfill ('0');
        }
#endif

#ifdef _64BIT
        ss << " x64";
#else
        ss << " w32";
#endif

#ifdef POPCNT
        ss << "-modern";
#endif

        ss  << "\n" 
            << ((uci) ? "id author " : "(c) 2014 ")
            << Author << "\n";

        return ss.str ();
    }

    void run (const std::string &args)
    {
        cout << Engine::info (false) << endl;

//        cout << "info string Processor(s) found " << cpu_count () << "." << endl;

#ifdef POPCNT
        cout << "info string POPCNT available." << endl;
#endif

#ifdef LPAGES
        cout << "info string LARGE PAGES available." << endl;
        MemoryHandler::initialize ();
#endif

        UCI      ::initialize ();
        BitBoard ::initialize ();
        Zobrist  ::initialize ();
        Position ::initialize ();
        BitBases ::initialize ();
        Searcher ::initialize ();
        Pawns    ::initialize ();
        Evaluator::initialize ();
        Threadpool.initialize ();
        
        TT.resize (int32_t (*(Options["Hash"])), true);

        string syzygy_path = string (*(Options["Syzygy Path"]));
        TBSyzygy::initialize (syzygy_path);

        cout << endl;

#ifndef NDEBUG

        //TT.resize (4, true);
        //TT.new_gen ();
        //TT.store (Key(U64(0x0000000100000001)), Move(123), Depth(7), Bound(1), 1, Value(1), Value(2));
        //TT.store (Key(U64(0x0000000200000001)), Move(124), Depth(5), Bound(1), 1, Value(1), Value(2));
        //TT.store (Key(U64(0x0000000300000001)), Move(125), Depth(1), Bound(1), 1, Value(1), Value(2));
        //string hash_fn = "hash.dat";
        //ofstream ohash_file (hash_fn, ios_base::out | ios_base::binary);
        //ohash_file << TT;
        //ohash_file.close ();

        //TT.master_clear ();
        //
        //ifstream ihash_file (hash_fn, ios_base::in | ios_base::binary);
        //ihash_file >> TT;
        //ihash_file.close ();

        //Tester::main_test ();
        //system ("pause");
        //return;
#endif

        UCI   ::start (args);

    }

    // Exit from engine with exit code. (in case of some crash)
    void exit (int32_t code)
    {
        UCI::stop ();
        
        Threadpool.deinitialize ();
        UCI::deinitialize ();

        ::exit (code);
    }

}