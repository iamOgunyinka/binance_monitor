#pragma once

#include <boost/asio/high_resolution_timer.hpp>
#include <boost/asio/io_context.hpp>
#include <optional>
#include <vector>

#include "subscription_data.hpp"

namespace binance {
namespace net = boost::asio;

class pnl_ticking_timer_t {
  net::io_context &io_context_;
  std::optional<net::high_resolution_timer> timer_;
  scheduled_task_t task_;
  subscription_data_map_t::const_iterator token_iter_;
  trade_direction_e direction_;
  bool price_obtained_ = false;

private:
  void send_price();
  void on_periodic_time_timeout();

public:
  pnl_ticking_timer_t(net::io_context &, scheduled_task_t &&);
  ~pnl_ticking_timer_t() { stop(); }
  std::string request_id() const { return task_.request_id; }
  void run();
  void stop();
};

class task_scheduler_t {
  net::io_context &io_context_;
  std::vector<std::unique_ptr<pnl_ticking_timer_t>> timers_;

public:
  task_scheduler_t(net::io_context &);
  void monitor_new_task(scheduled_task_t &&);
  std::vector<pnl_ticking_timer_t *> get_tickers(std::string const &request_id);
  void remove_tickers(std::string const &request_id);
};

namespace utilities {
std::optional<std::string> timet_to_string(std::size_t const t);
}
} // namespace binance
