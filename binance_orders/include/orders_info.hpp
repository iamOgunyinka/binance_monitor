#pragma once

#include <string>
#include <map>

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

struct ws_balance_info_t {
  std::string instrument_id{};
  std::string balance{};
  std::string event_time{};
  std::string clear_time{};
  std::string for_aliased_account{};
  std::string telegram_group{};
};

struct ws_account_update_t {
  std::string instrument_id{};
  std::string free_amount{};
  std::string locked_amount{};

  std::string event_time{};
  std::string last_account_update{};
  std::string for_aliased_account{};
  std::string telegram_group{};
};

using chat_name_t = std::string;
struct tg_chat_id_t {
  int database_id{};
  std::string telegram_chat_id{};
};

using tg_ccached_map_t = std::map<chat_name_t, tg_chat_id_t>;
} // namespace binance
