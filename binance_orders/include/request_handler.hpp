#pragma once

#include "containers.hpp"
#include "host_info.hpp"
#include "orders_info.hpp"
#include <variant>

namespace binance {
using user_stream_result_t =
    std::variant<ws_order_info_t, ws_balance_info_t, ws_account_update_t>;

class request_handler_t {
  static waitable_container_t<host_info_t> host_container_;
  static waitable_container_t<user_stream_result_t> user_stream_container_;

public:
  static auto &get_host_container() { return host_container_; }
  static auto &get_stream_container() { return user_stream_container_; }
};

} // namespace binance
