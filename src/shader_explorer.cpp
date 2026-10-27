#include "shader_explorer.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace akfsr::explorer {
namespace {

std::atomic<bool> g_enabled{false};
std::mutex g_mutex;
std::vector<entry> g_frame, g_last;       // frame en cours / dernière frame complète (thread de rendu)
std::unordered_map<uint32_t, size_t> g_index;
bool g_after_insertion = false;
std::unordered_set<uint32_t> g_hidden;

}  // namespace

void set_enabled(bool enabled) {
  if (g_enabled.exchange(enabled) && !enabled) clear_hidden();
}
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

bool on_draw(uint32_t ps_hash, uint32_t vertices, bool indirect) {
  if (!enabled() || ps_hash == 0) return false;
  std::lock_guard lock(g_mutex);
  auto it = g_index.find(ps_hash);
  if (it == g_index.end()) {
    it = g_index.emplace(ps_hash, g_frame.size()).first;
    g_frame.push_back({ps_hash, 0, 0, 0, g_after_insertion, false});
  }
  entry &e = g_frame[it->second];
  ++e.draws;
  e.vertices += vertices;
  if (indirect) ++e.indirect;
  return g_hidden.count(ps_hash) != 0;
}

void on_insertion() { g_after_insertion = true; }

void on_present() {
  if (!enabled()) return;
  std::lock_guard lock(g_mutex);
  for (auto &e : g_frame) e.hidden = g_hidden.count(e.hash) != 0;
  g_last.swap(g_frame);
  g_frame.clear();
  g_index.clear();
  g_after_insertion = false;
}

std::vector<entry> get_entries() {
  std::lock_guard lock(g_mutex);
  return g_last;
}

void set_hidden(uint32_t hash, bool hidden) {
  std::lock_guard lock(g_mutex);
  if (hidden) g_hidden.insert(hash);
  else g_hidden.erase(hash);
}

void clear_hidden() {
  std::lock_guard lock(g_mutex);
  g_hidden.clear();
}

}  // namespace akfsr::explorer
