#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
namespace dy {

template <typename T> class Mutex {
public:
  class SharedGuard {
  public:
    SharedGuard(std::mutex &mtx, T &input_data) : lock(mtx), data(input_data) {}
    T *operator->() { return &data; }
    T &operator*() { return data; }

    SharedGuard(const SharedGuard &) = delete;
    SharedGuard &operator=(const SharedGuard &) = delete;
    SharedGuard(SharedGuard &&) noexcept = default;
    SharedGuard &operator=(SharedGuard &&) noexcept = default;

  private:
    std::unique_lock<std::mutex> lock;
    T &data;
  };

  Mutex() = default;
  explicit Mutex(T &&initial_value) : data(std::move(initial_value)) {}
  SharedGuard lock() { return SharedGuard(mtx, data); }

private:
  std::mutex mtx;
  T data;
};

template <typename MapType> class Cache {
public:
  using KeyType = MapType::key_type;
  using ValueType = MapType::mapped_type;

  template <typename K> void insert(K &&key, ValueType value) {
    auto map = storage.lock();
    map->insert_or_assign(std::forward<K>(key), std::move(value));
  }

  template <typename K> auto get(const K &key) -> std::optional<ValueType> {
    auto map = storage.lock();
    auto it = map->find(key);
    if (it != map->end()) {
      return it->second;
    }
    return std::nullopt;
  }

  template <typename K, typename Func>
  auto get_or_create(const K &key, Func &&factory) -> ValueType {
    auto map = storage.lock();
    auto it = map->find(key);
    if (it != map->end()) {
      return it->second;
    }

    ValueType new_value = factory();
    map->insert({KeyType{key}, new_value});
    return new_value;
  }

  template <typename K> auto contains(const K &key) -> bool {
    auto map = storage.lock();
    return map->find(key) != map->end();
  }

  void clear() { storage.lock()->clear(); }

private:
  Mutex<MapType> storage;
};

} // namespace dy
