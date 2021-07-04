#pragma once

#include "containers.hpp"
#include "host_info.hpp"
#include "orders_info.hpp"
#include <variant>

namespace binance {

using host_other_data_t = std::variant<host_info_t, ws_order_info_t>;

class request_handler_t {
  static waitable_container_t<host_other_data_t> host_sub_container_;
  static waitable_container_t<ws_order_info_t> orders_container_;

public:
  static auto &get_host_container() { return host_sub_container_; }
  static auto &get_orders_container() { return orders_container_; }
};

} // namespace binance
