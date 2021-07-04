#pragma once

#include <string>
#include <unordered_map>
#include <variant>

namespace binance {

struct pushed_subscription_data_t {
  std::string instrument_id{};
  double current_price{};
  double open_24h{};
};

enum class task_state_e : std::size_t {
  unknown,
  initiated,
  running,
  stopped,
  restarted,
  remove
};

enum class task_type_e : std::size_t {
  profit_and_loss,
  price_changes,
  unknown
};

enum class trade_direction_e : std::size_t { none, sell, buy };

struct scheduled_task_t {
  std::string request_id{};
  std::string for_username{};
  std::string token_name{};
  std::string direction{};
  std::size_t monitor_time_secs{};
  std::size_t column_id{};
  std::size_t current_time{};
  task_state_e status{task_state_e::unknown};
  task_type_e task_type{task_type_e::unknown};
  double order_price{};
  // used when the current market price is needed
  double money{};
  double quantity{};

  struct task_result_t {
    /* this struct is created periodically, if there was a way to avoid copying
     * strings every time, we should take it. */
    std::string request_id{};
    std::string token_name{};
    std::string for_username{};
    std::string current_time{};
    trade_direction_e direction{trade_direction_e::none};
    task_type_e task_type{task_type_e::unknown};
    std::size_t column_id{};
    double order_price{};
    double mkt_price{};
    double money{};
    double quantity{};
    double pnl{};
  };
};

struct user_task_t {
  std::string request_id{};
  std::string token_name{};
  std::string direction{};
  std::string created_time{};
  std::string last_begin_time{}; // last time the task started
  std::string last_end_time{};   // last time the task ended
  std::size_t column_id{};
  std::size_t monitor_time_secs{};
  task_state_e status{task_state_e::unknown};
  task_type_e task_type{task_type_e::unknown};
  double money{};
  double order_price{};
  double quantity{};
};

using subscription_data_map_t =
    std::unordered_map<std::string, pushed_subscription_data_t>;
using scheduled_task_result_t =
    std::variant<scheduled_task_t, scheduled_task_t::task_result_t>;

std::string task_state_to_string(task_state_e const);
std::string direction_to_string(trade_direction_e const);
trade_direction_e string_to_direction(std::string const &);

} // namespace binance
