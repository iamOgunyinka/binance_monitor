#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace binance {

template <typename Container, typename... IterList>
bool any_of(Container const &container, IterList &&...iter_list) {
  return (... || (std::cend(container) == iter_list));
}

struct twitter_name_t {
  std::string username{};
  std::string twitter_id{};
};

template <typename T> struct locked_set_t {
private:
  std::unordered_set<T> set_{};
  std::mutex mutex_{};

public:
  void insert(T const &item) {
    std::lock_guard<std::mutex> lock_g{mutex_};
    set_.insert(item);
  }

  void insert(T &&item) {
    std::lock_guard<std::mutex> lock_g{mutex_};
    set_.insert(std::move(item));
  }

  template <typename Func> std::vector<T> all_items_matching(Func &&filter) {
    std::lock_guard<std::mutex> lock_g{mutex_};
    std::vector<T> items{};
    for (auto const &item : set_) {
      if (filter(item)) {
        items.push_back(item);
      }
    }
    return items;
  }
  auto size() const { return set_.size(); }
};

template <typename T, typename Container = std::deque<T>>
struct waitable_container_t {
private:
  std::mutex mutex_{};
  Container container_{};
  std::condition_variable cv_{};

public:
  waitable_container_t(Container &&container)
      : container_{std::move(container)} {}
  waitable_container_t() = default;

  waitable_container_t(waitable_container_t &&vec)
      : mutex_{std::move(vec.mutex_)},
        container_{std::move(vec.container_)}, cv_{std::move(vec.cv_)} {}
  waitable_container_t &operator=(waitable_container_t &&) = delete;
  waitable_container_t(waitable_container_t const &) = delete;
  waitable_container_t &operator=(waitable_container_t const &) = delete;

  T get() {
    std::unique_lock<std::mutex> u_lock{mutex_};
    cv_.wait(u_lock, [this] { return !container_.empty(); });
    T value{std::move(container_.front())};
    container_.pop_front();
    return value;
  }

  template <typename U> void append(U &&data) {
    std::lock_guard<std::mutex> lock_{mutex_};
    container_.push_back(std::forward<U>(data));
    cv_.notify_all();
  }

  template <
      typename Container,
      typename = std::enable_if_t<std::is_convertible_v<
          typename decltype(std::declval<Container>().begin())::value_type, T>>>
  void append_list(Container &&new_list) {
    std::lock_guard<std::mutex> lock_g{mutex_};
    // container_.reserve(container_.size() + new_list.size());
    for (auto &&item : new_list) {
      container_.push_back(std::move(item));
    }
    cv_.notify_all();
  }

  bool empty() {
    std::lock_guard<std::mutex> lock_{mutex_};
    return container_.empty();
  }

  std::size_t size() {
    std::lock_guard<std::mutex> lock_{mutex_};
    return container_.size();
  }
};

} // namespace binance
