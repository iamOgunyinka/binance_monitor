#include "request_handler.hpp"

namespace binance {

waitable_container_t<host_other_data_t> request_handler_t::host_sub_container_{};

waitable_container_t<ws_order_info_t> request_handler_t::orders_container_{};

} // namespace okex
