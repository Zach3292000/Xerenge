#include "ealobby/rankings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace ealobby {
namespace {

constexpr auto kSeriesWait = std::chrono::seconds(60);
constexpr double kStartRating = 1000.0;
constexpr double kK = 32.0;  // a two-player series moves a rating by up to 32

int64_t Now() {
  return static_cast<int64_t>(std::time(nullptr));
}

int Number(const Fields& fields, const std::string& key, int fallback) {
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  char* end = nullptr;
  const long value = std::strtol(it->second.c_str(), &end, 10);
  return end != it->second.c_str() ? static_cast<int>(value) : fallback;
}

// The board a GAMEMODE counts on besides the overall one, or -1.
int ModeBoard(int mode) {
  switch (mode) {
    case 0:
      return Rankings::kRace;
    case 1:
      return Rankings::kRoadRage;
    case 2:
      return Rankings::kTrafficAttack;
    case 3:
    case 4:
    case 5:
      return Rankings::kCrash;
    default:
      return -1;
  }
}

std::string Clean(std::string text) {
  for (char& c : text) {
    if (c == '\t' || c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return text;
}

}  // namespace

const char* Rankings::BoardName(int board) {
  static const char* const kNames[kBoardCount] = {"OVERALL", "RACE", "ROAD RAGE", "TRAFFIC ATTACK", "CRASH"};
  return board >= 0 && board < kBoardCount ? kNames[board] : "";
}

Rankings::Rankings(std::filesystem::path dir, bool count_unranked, std::function<void(const std::string&)> log)
    : dir_(std::move(dir)), count_unranked_(count_unranked), log_(std::move(log)) {
  std::error_code error;
  std::filesystem::create_directories(dir_, error);
  Load();
  size_t players = boards_[kOverall].size();
  Log("rankings: " + std::to_string(players) + " ranked player(s), files in " + dir_.string() +
      (count_unranked_ ? " (Player Match series count too)" : " (Ranked series only)"));
}

void Rankings::Log(const std::string& line) const {
  if (log_) {
    log_(line);
  }
}

void Rankings::Report(const std::string& reporter, uint32_t game, bool ranked, const std::string& body) {
  const Fields fields = ParseFields(body);
  {
    std::ofstream out(dir_ / "reports.log", std::ios::app | std::ios::binary);
    out << Now() << '\t' << game << '\t' << (ranked ? "ranked" : "unranked") << '\t' << Clean(reporter) << '\t'
        << Printable(body) << '\n';
  }

  std::vector<std::string> names;
  for (int n = 0; n < 16; ++n) {
    const auto it = fields.find("NAME" + std::to_string(n));
    if (it == fields.end()) {
      break;
    }
    if (!it->second.empty()) {
      names.push_back(it->second);
    }
  }
  std::sort(names.begin(), names.end());
  const int mode = Number(fields, "GAMEMODE", -1);
  const int stages = Number(fields, "NUMSTAGES", 32);
  int points = 0;
  for (int n = 0; n < stages && n < 32; ++n) {
    const int stage = Number(fields, "POSPTS" + std::to_string(n), -1);
    if (stage < 0) {
      if (fields.count("NUMSTAGES") == 0) {
        break;
      }
      continue;
    }
    points += stage;
  }
  Log("rankings: " + reporter + " reports game " + std::to_string(game) + (ranked ? " (ranked)" : " (player match)") +
      ", mode " + std::to_string(mode) + ", " + std::to_string(points) + " point(s) over " + std::to_string(stages) +
      " stage(s), " + std::to_string(names.size()) + " player(s)");

  // Series in a game that is not one: grouped by who played.
  uint32_t key = game;
  if (key == 0) {
    std::string all;
    for (const std::string& name : names) {
      all += name + '\n';
    }
    key = 0x80000000u | PlayerId(all);
  }
  auto it = pending_.find(key);
  if (it != pending_.end() && (it->second.names != names || it->second.points.count(reporter) != 0)) {
    // The same game's next series: the last one is as complete as it gets.
    Settle(it->second);
    pending_.erase(it);
    it = pending_.end();
  }
  if (it == pending_.end()) {
    Series series;
    series.game = game;
    series.mode = mode;
    series.names = names;
    series.first = std::chrono::steady_clock::now();
    it = pending_.emplace(key, std::move(series)).first;
  }
  Series& series = it->second;
  series.ranked = series.ranked || ranked;
  series.points[reporter] = points;
  const bool all_in = std::all_of(names.begin(), names.end(),
                                  [&](const std::string& name) { return series.points.count(name) != 0; });
  if (all_in) {
    Settle(series);
    pending_.erase(it);
  }
}

void Rankings::Tick() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (now - it->second.first >= kSeriesWait) {
      Settle(it->second);
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
}

void Rankings::Settle(Series& series) {
  const std::string what = "rankings: series in game " + std::to_string(series.game);
  if (!series.ranked && !count_unranked_) {
    Log(what + " was a Player Match; not counted");
    return;
  }
  if (series.points.size() < 2) {
    Log(what + ": only " + std::to_string(series.points.size()) + " report(s) came; not rated");
    return;
  }
  std::vector<std::pair<std::string, int>> players(series.points.begin(), series.points.end());
  int best = 0;
  for (const auto& [name, points] : players) {
    best = std::max(best, points);
  }
  const double k = kK / double(players.size() - 1);
  const int64_t now = Now();
  std::string summary;
  for (int board : {static_cast<int>(kOverall), ModeBoard(series.mode)}) {
    if (board < 0) {
      continue;
    }
    std::vector<Entry>& entries = boards_[board];
    const auto find = [&](const std::string& persona) {
      return std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.persona == persona; });
    };
    for (const auto& player : players) {
      if (find(player.first) == entries.end()) {
        Entry entry;
        entry.persona = player.first;
        entry.rating = kStartRating;
        entries.push_back(entry);
      }
    }
    // Pointers only once every entry is in: a push_back may move them.
    std::vector<Entry*> who;
    for (const auto& player : players) {
      who.push_back(&*find(player.first));
    }
    std::vector<double> change(players.size(), 0.0);
    for (size_t i = 0; i < players.size(); ++i) {
      for (size_t j = 0; j < players.size(); ++j) {
        if (i == j) {
          continue;
        }
        const double expected = 1.0 / (1.0 + std::pow(10.0, (who[j]->rating - who[i]->rating) / 400.0));
        const double scored = players[i].second > players[j].second    ? 1.0
                              : players[i].second == players[j].second ? 0.5
                                                                       : 0.0;
        change[i] += k * (scored - expected);
      }
    }
    for (size_t i = 0; i < players.size(); ++i) {
      const double before = who[i]->rating;
      who[i]->rating += change[i];
      who[i]->played += 1;
      who[i]->wins += players[i].second == best ? 1 : 0;
      who[i]->last = now;
      if (board == kOverall) {
        char line[160];
        std::snprintf(line, sizeof(line), " %s %d pts %.0f->%.0f;", players[i].first.c_str(), players[i].second,
                      before, who[i]->rating);
        summary += line;
      }
    }
  }
  Log(what + " (mode " + std::to_string(series.mode) + ") rated:" + summary);
  Save();
}

std::vector<Rankings::Entry> Rankings::Standings(int board) const {
  if (board < 0 || board >= kBoardCount) {
    return {};
  }
  std::vector<Entry> out = boards_[board];
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    if (a.rating != b.rating) {
      return a.rating > b.rating;
    }
    if (a.wins != b.wins) {
      return a.wins > b.wins;
    }
    return a.persona < b.persona;
  });
  return out;
}

void Rankings::Load() {
  std::ifstream in(dir_ / "ratings.tsv", std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::vector<std::string> cells;
    std::stringstream stream(line);
    std::string cell;
    while (std::getline(stream, cell, '\t')) {
      cells.push_back(cell);
    }
    if (cells.size() < 6) {
      continue;
    }
    const int board = std::atoi(cells[0].c_str());
    if (board < 0 || board >= kBoardCount || cells[1].empty()) {
      continue;
    }
    Entry entry;
    entry.persona = cells[1];
    entry.rating = std::atof(cells[2].c_str());
    entry.played = static_cast<uint32_t>(std::strtoul(cells[3].c_str(), nullptr, 10));
    entry.wins = static_cast<uint32_t>(std::strtoul(cells[4].c_str(), nullptr, 10));
    entry.last = std::strtoll(cells[5].c_str(), nullptr, 10);
    boards_[board].push_back(entry);
  }
}

void Rankings::Save() const {
  const std::filesystem::path path = dir_ / "ratings.tsv";
  const std::filesystem::path temp = dir_ / "ratings.tsv.new";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << "# board (0 overall, 1 race, 2 road rage, 3 traffic attack, 4 crash)\tpersona\trating\tseries\twins\t"
           "last rated (unix time)\n";
    for (int board = 0; board < kBoardCount; ++board) {
      for (const Entry& e : boards_[board]) {
        char rating[32];
        std::snprintf(rating, sizeof(rating), "%.2f", e.rating);
        out << board << '\t' << Clean(e.persona) << '\t' << rating << '\t' << e.played << '\t' << e.wins << '\t'
            << e.last << '\n';
      }
    }
    if (!out) {
      Log("rankings: cannot write " + temp.string());
      return;
    }
  }
  std::error_code error;
  std::filesystem::rename(temp, path, error);
  if (error) {
    std::filesystem::remove(path, error);
    std::filesystem::rename(temp, path, error);
    if (error) {
      Log("rankings: cannot replace " + path.string() + ": " + error.message());
    }
  }
}

}  // namespace ealobby
