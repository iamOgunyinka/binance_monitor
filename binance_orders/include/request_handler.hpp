#pragma once

#include "containers.hpp"
#include "host_info.hpp"
#include "orders_info.hpp"

namespace binance {

class request_handler_t {
  static waitable_container_t<host_info_t> host_container_;
  static waitable_container_t<ws_order_info_t> orders_container_;

public:
  static auto &get_host_container() { return host_container_; }
  static auto &get_orders_container() { return orders_container_; }
};

} // namespace binance
