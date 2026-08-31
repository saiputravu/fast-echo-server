#pragma once
#include <mutex>
#include <optional>
#include <unordered_map>

template <typename Key, typename Value> class SyncMap {
public:
  auto find(Key k) -> std::optional<std::pair<Key, Value>> {
    std::scoped_lock lk(_lock);
    auto res = _m.find(k);
    if (res == _m.end()) return std::nullopt;
    return std::optional(*res);
  }

  void set(Key k, Value v) {
    std::scoped_lock lk(_lock);
    _m[k] = v;
  }

  void remove(Key k) {
    std::scoped_lock lk(_lock);
    _m.erase(k);
  }

  // Not thread safe. Supposed to be used on copies.
  // Call copy() first.
  auto begin() { return _m.begin(); }
  auto end() { return _m.end(); }

  auto copy() -> std::unordered_map<Key, Value> {
    std::scoped_lock lk(_lock);
    // Return the copy of _m
    return _m;
  }

private:
  std::mutex _lock;
  std::unordered_map<Key, Value> _m;
};
