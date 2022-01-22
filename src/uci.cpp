/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2021 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <stdlib.h>

#include "evaluate.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"

using namespace std;

extern vector<string> setup_bench(const Position&, istream&);

namespace {

  // FEN string of the initial position, normal chess
  const char* StartFEN = "rnsmksnr/8/pppppppp/8/8/PPPPPPPP/8/RNSKMSNR w 0 1";


  // position() is called when engine receives the "position" UCI command.
  // The function sets up the position described in the given FEN string ("fen")
  // or the starting position ("startpos") and then makes the moves given in the
  // following move list ("moves").

  void position(Position& pos, istringstream& is, StateListPtr& states) {

    Move m;
    string token, fen;

    is >> token;

    if (token == "startpos")
    {
        fen = StartFEN;
        is >> token; // Consume "moves" token if any
    }
    else if (token == "fen")
        while (is >> token && token != "moves")
            fen += token + " ";
    else
        return;

    states = StateListPtr(new std::deque<StateInfo>(1)); // Drop old and create a new one
    pos.set(fen, Options["UCI_Chess960"], &states->back(), Threads.main());

    // Parse move list (if any)
    while (is >> token && (m = UCI::to_move(pos, token)) != MOVE_NONE)
    {
        states->emplace_back();
        pos.do_move(m, states->back());
    }
  }


  // setoption() is called when engine receives the "setoption" UCI command. The
  // function updates the UCI option ("name") to the given value ("value").

  void setoption(istringstream& is) {

    string token, name, value;

    is >> token; // Consume "name" token

    // Read option name (can contain spaces)
    while (is >> token && token != "value")
        name += (name.empty() ? "" : " ") + token;

    // Read option value (can contain spaces)
    while (is >> token)
        value += (value.empty() ? "" : " ") + token;

    if (Options.count(name))
        Options[name] = value;
    else
        sync_cout << "No such option: " << name << sync_endl;
  }


  // go() is called when engine receives the "go" UCI command. The function sets
  // the thinking time and other parameters from the input string, then starts
  // the search.

  void go(Position& pos, istringstream& is, StateListPtr& states) {

    Search::LimitsType limits;
    string token;
    bool ponderMode = false;

    limits.startTime = now(); // As early as possible!

    while (is >> token)
        if (token == "searchmoves")
            while (is >> token)
                limits.searchmoves.push_back(UCI::to_move(pos, token));

        else if (token == "wtime")     is >> limits.time[WHITE];
        else if (token == "btime")     is >> limits.time[BLACK];
        else if (token == "winc")      is >> limits.inc[WHITE];
        else if (token == "binc")      is >> limits.inc[BLACK];
        else if (token == "movestogo") is >> limits.movestogo;
        else if (token == "depth")     is >> limits.depth;
        else if (token == "nodes")     is >> limits.nodes;
        else if (token == "movetime")  is >> limits.movetime;
        else if (token == "mate")      is >> limits.mate;
        else if (token == "perft")     is >> limits.perft;
        else if (token == "infinite")  limits.infinite = 1;
        else if (token == "ponder")    ponderMode = true;

    Threads.start_thinking(pos, states, limits, ponderMode);
  }


  // bench() is called when engine receives the "bench" command. Firstly
  // a list of UCI commands is setup according to bench parameters, then
  // it is run one by one printing a summary at the end.

  void bench(Position& pos, istream& args, StateListPtr& states) {

    string token;
    uint64_t num, nodes = 0, cnt = 1;

    vector<string> list = setup_bench(pos, args);
    num = count_if(list.begin(), list.end(), [](string s) { return s.find("go ") == 0; });

    TimePoint elapsed = now();

    for (const auto& cmd : list)
    {
        istringstream is(cmd);
        is >> skipws >> token;

        if (token == "go")
        {
            cerr << "\nPosition: " << cnt++ << '/' << num << endl;
            go(pos, is, states);
            Threads.main()->wait_for_search_finished();
            nodes += Threads.nodes_searched();
        }
        else if (token == "setoption")  setoption(is);
        else if (token == "position")   position(pos, is, states);
        else if (token == "ucinewgame") { Search::clear(); elapsed = now(); } // Search::clear() may take some while
    }

    elapsed = now() - elapsed + 1; // Ensure positivity to avoid a 'divide by zero'

    dbg_print(); // Just before exiting

    cerr << "\n==========================="
         << "\nTotal time (ms) : " << elapsed
         << "\nNodes searched  : " << nodes
         << "\nNodes/second    : " << 1000 * nodes / elapsed << endl;
  }

} // namespace


/// UCI::loop() waits for a command from stdin, parses it and calls the appropriate
/// function. Also intercepts EOF from stdin to ensure gracefully exiting if the
/// GUI dies unexpectedly. When called with some command line arguments, e.g. to
/// run 'bench', once the command is executed the function returns immediately.
/// In addition to the UCI ones, also some additional debug commands are supported.

void UCI::loop(int argc, char* argv[]) {

  Position pos;
  string token, cmd;
  StateListPtr states(new std::deque<StateInfo>(1));
  auto uiThread = std::make_shared<Thread>(0);

  pos.set(StartFEN, false, &states->back(), uiThread.get());

  for (int i = 1; i < argc; ++i)
      cmd += std::string(argv[i]) + " ";

  do {
      if (argc == 1 && !getline(cin, cmd)) // Block here waiting for input or EOF
          cmd = "quit";

      istringstream is(cmd);

      token.clear(); // Avoid a stale if getline() returns empty or blank line
      is >> skipws >> token;

      if (    token == "quit"
          ||  token == "stop")
          Threads.stop = true;

      // The GUI sends 'ponderhit' to tell us the user has played the expected move.
      // So 'ponderhit' will be sent if we were told to ponder on the same move the
      // user has played. We should continue searching but switch from pondering to
      // normal search.
      else if (token == "ponderhit")
          Threads.main()->ponder = false; // Switch to normal search

      else if (token == "uci")
          sync_cout << "id name " << engine_info(true)
                    << "\n"       << Options
                    << "\nuciok"  << sync_endl;

      else if (token == "setoption")  setoption(is);
      else if (token == "go")         go(pos, is, states);
      else if (token == "position")   position(pos, is, states);
      else if (token == "ucinewgame") Search::clear();
      else if (token == "isready")    sync_cout << "readyok" << sync_endl;
      else if (token == "dev")        sync_cout << "Welcome to BlueCat-Dev Console 1.0! namespace psqt:: if don't know command > type 'devhelp' it will show command you need, use." << sync_endl;
      else if (token == "devhelp")    sync_cout << "BlueCat-Dev Console 1.0 Developers Command\n"
                                                << "1. 'psqt' this will show array, bonus, score\n"
                                                << "2. 'type' this will show piece value\n"
                                                << "3. 'evaluate' this will show evaluate setting and mobility bonus\n" << sync_endl;
      else if (token == "psqt")       sync_cout << "Bluecat-Stockfish 64 'Main Array' Array by nongbossmakrukthai UCI Engine for Makruk\n"
                                                <<  "\n"
                                                <<  "// Pawn\n"
                                                <<  "{ S(  0, 0), S( 0, 0), S( 0, 0), S( 0, 0) },\n"
                                                <<  "{ S(  0, 0), S( 0, 0), S( 0, 0), S( 0, 0) },\n"
                                                <<  "{ S( -8,-4), S( 1,-5), S( 7, 5), S(15, 4) },\n"
                                                <<  "{ S(-17, 3), S( 5, 3), S( 2,-8), S( 3,-3) },\n"
                                                <<  "{ S( -6, 8), S( 1, 9), S( 8, 7), S( 9,-6) }\n"
                                                <<  "\n"
                                                <<  "// Queen\n"
                                                <<  "{ S(-175, -96), S(-92,-65), S(-74,-49), S(-73,-21) },\n"
                                                <<  "{ S( -77, -67), S(-41,-54), S( -7,-18), S(-15,  8) },\n"
                                                <<  "{ S( -61, -40), S(-22,-27), S(151, -8), S(257, 29) },\n"
                                                <<  "{ S(  -1, -35), S( 68, -2), S( 86, 13), S( 87, 28) },\n"
                                                <<  "{ S( -14, -45), S( 73,-16), S( 78,  9), S( 78, 39) },\n"
                                                <<  "{ S(  -9, -51), S( 82,-44), S(267,-16), S(290, 17) },\n"
                                                <<  "{ S( -67, -69), S(-27,-50), S(  4,-51), S( 37, 12) },\n"
                                                <<  "{ S(-201,-100), S(-83,-88), S(-56,-56), S(-26,-17) }\n"
                                                <<  "\n"
                                                <<  "// Bishop"
                                                <<  "{ S(-175, -96), S(-92,-65), S(-74,-49), S(-73,-21) },\n"
                                                <<  "{ S( -37, -67), S(-21,-54), S(  0,-18), S(  0,  8) },\n"
                                                <<  "{ S(  -3, -40), S( 65,-27), S(151, -8), S(187, 29) },\n"
                                                <<  "{ S(  45, -35), S( 68, -2), S(170, 13), S(179, 28) },\n"
                                                <<  "{ S(  -2, -45), S( 73,-16), S(174,  9), S(181, 39) },\n"
                                                <<  "{ S(  -1, -51), S( 82,-44), S(183,-16), S(188, 17) },\n"
                                                <<  "{ S( -67, -69), S(-27,-50), S(  4,-51), S( 37, 12) },\n"
                                                <<  "{ S(-201,-100), S(-83,-88), S(-56,-56), S(-26,-17) }\n"
                                                <<  "\n"
                                                <<  "// Knight\n"
                                                <<  "{ S(-175, -96), S(-92,-65), S(-74,-49), S(-73,-21) },\n"
                                                <<  "{ S( -77, -67), S(-41,-54), S(-27,-18), S( 15,  8) },\n"
                                                <<  "{ S( -61, -40), S(  1,-27), S(  0, -8), S( 12, 29) },\n"
                                                <<  "{ S( -35, -35), S(  8, -2), S( 40, 13), S( 49, 28) },\n"
                                                <<  "{ S( -34, -45), S( 13,-16), S( 44,  9), S( 51, 39) },\n"
                                                <<  "{ S(  -9, -51), S( 22,-44), S( 58,-16), S( 53, 17) },\n"
                                                <<  "{ S( -67, -69), S(-27,-50), S(  4,-51), S( 37, 12) },\n"
                                                <<  "{ S(-201,-100), S(-83,-88), S(-56,-56), S(-26,-17) }\n"
                                                <<  "\n"
                                                <<  "// Rook\n"
                                                <<  "{ S(-31, -9), S(-20,-13), S(-14,-10), S(-5, -9) },\n"
                                                <<  "{ S(-21,-12), S(-13, -9), S( -8, -1), S( 6, -2) },\n"
                                                <<  "{ S(-25,  6), S(-11, -8), S( -1, -2), S( 3, -6) },\n"
                                                <<  "{ S(-13, -6), S( -5,  1), S( -4, -9), S(-6,  7) },\n"
                                                <<  "{ S(-27, -5), S(-15,  8), S( -4,  7), S( 3, -6) },\n"
                                                <<  "{ S(-22,  6), S( -2,  1), S(  6, -7), S(12, 10) },\n"
                                                <<  "{ S( -2,  4), S( 12,  5), S( 16, 20), S(18, -5) },\n"
                                                <<  "{ S(-17, 18), S(-19,  0), S( -1, 19), S( 9, 13) }\n"
                                                <<  "\n"
                                                <<  "// King\n"
                                                <<  "{ S( 0,  1), S(  0, 45), S( 32, 85), S(285, 76) },\n"
                                                <<  "{ S(91, 53), S(158,100), S(120,133), S( 98,135) },\n"
                                                <<  "{ S(99, 88), S(126,130), S( 84,169), S( 60,175) },\n"
                                                <<  "{ S(84,103), S( 95,156), S( 68,172), S( 54,172) },\n"
                                                <<  "{ S(72, 96), S( 88,166), S( 56,199), S( 34,199) },\n"
                                                <<  "{ S(61, 92), S( 79,172), S( 42,184), S( 18,191) },\n"
                                                <<  "{ S(43, 47), S( 60,121), S( 32,116), S( 12,131) },\n"
                                                <<  "{ S( 0, 11), S( 44, 59), S( 24, 73), S( 10, 78) }\n" << sync_endl;
      else if (token == "type")       sync_cout <<  "PawnValueMg   = 199,   PawnValueEg   = 206,\n"
                                                <<  "QueenValueMg  = 354,   QueenValueEg  = 430,\n"
                                                <<  "BishopValueMg = 595,   BishopValueEg = 665,\n"
                                                <<  "KnightValueMg = 812,   KnightValueEg = 925,\n"
                                                <<  "RookValueMg   = 1389,  RookValueEg   = 1538,\n"
                                                <<  "\n"
                                                    "MidgameLimit  = 15258, EndgameLimit  = 3915\n" << sync_endl;
      else if (token == "evaluate")   sync_cout << "constexpr Score MobilityBonus[][32] = {\n"
                                                << "{ S(-59,-59), S(-23,-23), S( -3, -3), S( 13, 13), S( 24, 24) },           // Queens\n"
                                                << "{ S(-59,-59), S(-23,-23), S( -3, -3), S( 13, 13), S( 24, 24), S( 42, 42) }, // Bishops\n"
                                                << "{ S(-61,-80), S(-57,-47), S(-11,-34), S( -4,-20), S(  3,  2), S( 15, 13), // Knights\n"
                                                << "S( 23, 28), S( 27, 24), S( 35, 26) },\n"
                                                << "{ S(-58,-76), S(-27,-18), S(-15, 28), S(-10, 55), S( -5, 69), S( -2, 82), // Rooks\n"
                                                << "S(  9,112), S( 16,118), S( 30,132), S( 29,142), S( 32,155), S( 38,165),\n"
                                                << "S( 46,166), S( 48,169), S( 58,171) }\n"
                                                << "};\n"
                                                << "\n"
                                                << "// Assorted bonuses and penalties\n"
                                                << "constexpr Score BishopPawns        = S(  3,  0);\n"
                                                << "constexpr Score FlankAttacks       = S(  6,  0);\n"
                                                << "constexpr Score Hanging            = S( 69, 36);\n"
                                                << "constexpr Score HinderPassedPawn   = S(  1,  0);\n"
                                                << "constexpr Score KingProtector      = S(  3,  3);\n"
                                                << "constexpr Score LongDiagonalBishop = S( 45,  0);\n"
                                                << "constexpr Score MinorBehindPawn    = S( 16,  0);\n"
                                                << "constexpr Score PassedFile         = S( 11,  8);\n"
                                                << "constexpr Score PawnlessFlank      = S( 17, 95);\n"
                                                << "constexpr Score RestrictedPiece    = S(  7,  7);\n"
                                                << "constexpr Score RookOnPawn         = S( 10, 32);\n"
                                                << "constexpr Score ThreatByKing       = S( 24, 89);\n"
                                                << "constexpr Score ThreatByPawnPush   = S( 48, 39);\n"
                                                << "constexpr Score ThreatByRank       = S( 13,  0);\n"
                                                << "constexpr Score ThreatBySafePawn   = S(173, 94);\n"
                                                << "constexpr Score TrappedRook        = S( 47,  4);\n" << sync_endl;
      else if (token == "clear")      system("cls"), engine_info(true);

      // Additional custom non-UCI commands, mainly for debugging
      else if (token == "flip")  pos.flip();
      else if (token == "bench") bench(pos, is, states);
      else if (token == "d")     sync_cout << pos << sync_endl;
      else if (token == "eval")  sync_cout << Eval::trace(pos) << sync_endl;
      else
          sync_cout << "Unknown command: " << cmd << sync_endl;

  } while (token != "quit" && argc == 1); // Command line args are one-shot
}


/// UCI::value() converts a Value to a string suitable for use with the UCI
/// protocol specification:
///
/// cp <x>    The score from the engine's point of view in centipawns.
/// mate <y>  Mate in y moves, not plies. If the engine is getting mated
///           use negative values for y.

string UCI::value(Value v) {

  assert(-VALUE_INFINITE < v && v < VALUE_INFINITE);

  stringstream ss;

  if (abs(v) < VALUE_MATE - MAX_PLY)
      ss << "cp " << v * 100 / PawnValueEg;
  else
      ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;

  return ss.str();
}


/// UCI::square() converts a Square to a string in algebraic notation (g1, a7, etc.)

std::string UCI::square(Square s) {
  return std::string{ char('a' + file_of(s)), char('1' + rank_of(s)) };
}


/// UCI::move() converts a Move to a string in coordinate notation (g1f3, a7a8q).

string UCI::move(Move m) {

  Square from = from_sq(m);
  Square to = to_sq(m);

  if (m == MOVE_NONE)
      return "(none)";

  if (m == MOVE_NULL)
      return "0000";

  string move = UCI::square(from) + UCI::square(to);

  if (type_of(m) == PROMOTION)
      move += " pmsnrk"[promotion_type(m)];

  return move;
}


/// UCI::to_move() converts a string representing a move in coordinate notation
/// (g1f3, a7a8q) to the corresponding legal Move, if any.

Move UCI::to_move(const Position& pos, string& str) {

  if (str.length() == 5) // Junior could send promotion piece in uppercase
      str[4] = char(tolower(str[4]));

  for (const auto& m : MoveList<LEGAL>(pos))
      if (str == UCI::move(m))
          return m;

  return MOVE_NONE;
}
