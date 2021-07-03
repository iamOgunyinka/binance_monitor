#pragma once

#include "containers.hpp"
#include "host_info.hpp"
#include "orders_info.hpp"
#include "subscription_data.hpp"
#include <variant>

namespace binance {

class request_handler_t {

  static waitable_container_t<pushed_subscription_data_t> tokens_container_;
  static locked_set_t<instrument_type_t> all_listed_instruments_;
  static subscription_data_map_t all_pushed_sub_data_;
  static waitable_container_t<scheduled_task_result_t> scheduled_tasks_;

public:
  static auto &get_tokens_container() { return tokens_container_; }
  static auto &get_all_listed_instruments() { return all_listed_instruments_; }
  static auto &get_all_pushed_data() { return all_pushed_sub_data_; }
  static auto &get_all_scheduled_tasks() { return scheduled_tasks_; }
};

} // namespace okex
