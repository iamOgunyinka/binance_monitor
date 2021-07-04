#pragma once

#include "private_channel_websocket.hpp"

namespace binance {

namespace net = boost::asio;
namespace ssl = net::ssl;

void launch_previous_hosts(
    std::vector<std::shared_ptr<private_channel_websocket_t>> &,
    net::io_context &, ssl::context &ssl_context);

void websock_launcher(
    std::vector<std::shared_ptr<private_channel_websocket_t>> &,
    net::io_context &io_context, ssl::context &ssl_context);

void background_persistent_orders_saver();

void monitor_host_changes();

std::string get_alphanum_tablename(std::string);
} // namespace binance
