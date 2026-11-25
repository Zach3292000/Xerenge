// Online rankings, standing in for EA's (DirtySock's LobbyRank).
//
// After every online series each player's game reports its own result to the
// lobby as 'rank' (built at 0x8236EE00 from the stats at 0x82221AD0):
//
//   WHEN, REPT (the reporter), AUTH, VENUE, NAME<n>/TEAM<n>/WEIGHT<n> for every
//   player in the series, then SKU=BURNOUT3, GAMEMODE (0 race, 1 road rage,
//   2 traffic attack, 3-5 crash, -1 other), the mode's totals, per stage
//   POSPTS<n> and the mode's stage values, NUMSTAGES and ONLINETAKEDOWNS.
//
// POSPTS is the stage's finishing points: 6 for first, one less for each place
// after it, ties sharing the better one (0x82129398's sort and the loop after
// it). A series is the sum of them.
//
// The reports of one series arrive one by one, a player's at a time. They are
// gathered by the game they came from, and once every named player has
// reported (or a minute has passed) the series is settled: each player who
// reported is rated against each other one by their series points, with an
// Elo rating per board (overall and the mode's), starting at 1000.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ealobby/aries.h"

namespace ealobby {

class Rankings {
 public:
  enum Board : int { kOverall = 0, kRace, kRoadRage, kTrafficAttack, kCrash, kBoardCount };
  // The board's name as the game shows it.
  static const char* BoardName(int board);

  struct Entry {
    std::string persona;
    double rating = 1000.0;
    uint32_t played = 0;  // series
    uint32_t wins = 0;    // series won (most points; ties all win)
    int64_t last = 0;     // when last rated (seconds since the epoch)
  };

  // dir holds ratings.tsv (the boards) and reports.log (every report, as it
  // came). count_unranked: Player Match series count too, not only Ranked.
  Rankings(std::filesystem::path dir, bool count_unranked, std::function<void(const std::string&)> log);

  // One player's 'rank' report: who sent it, the game it is in (0: none) and
  // whether that game is a Ranked one.
  void Report(const std::string& reporter, uint32_t game, bool ranked, const std::string& body);
  // Settles the series that have waited long enough. Called now and then.
  void Tick();

  // A board's players, best first.
  std::vector<Entry> Standings(int board) const;

 private:
  struct Series {
    uint32_t game = 0;
    bool ranked = false;
    int mode = -1;
    std::vector<std::string> names;      // NAME<n>, as the reports list them
    std::map<std::string, int> points;  // per reporter: the series' POSPTS
    std::chrono::steady_clock::time_point first;
  };
  void Settle(Series& series);
  void Load();
  void Save() const;
  void Log(const std::string& line) const;

  std::filesystem::path dir_;
  bool count_unranked_ = false;
  std::function<void(const std::string&)> log_;
  std::map<uint32_t, Series> pending_;  // by game
  std::vector<Entry> boards_[kBoardCount];
};

}  // namespace ealobby
