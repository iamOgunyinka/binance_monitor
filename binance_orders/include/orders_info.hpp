#pragma once

#include <string>

namespace binance {

struct ws_order_info_t {
  std::string instrument_id{};
  std::string order_side{};
  std::string order_type{};
  std::string time_in_force{};
  std::string quantity_purchased{};
  std::string order_price{};
  std::string stop_price{};
  std::string execution_type{};
  std::string order_status{};
  std::string reject_reason{};
  std::string order_id{};
  std::string last_filled_quantity{};
  std::string cummulative_filled_quantity{};
  std::string last_executed_price{};
  std::string commission_amount{};
  std::string commission_asset{};
  std::string trade_id{};

  std::string event_time{};
  std::string transaction_time{};
  std::string created_time{};

  std::string for_aliased_account{};
  std::string telegram_group{};
};

} // namespace binance
