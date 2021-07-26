#pragma once

#include "user_data_stream.hpp"

namespace binance {

namespace net = boost::asio;
namespace ssl = net::ssl;

void launch_previous_hosts(std::vector<std::shared_ptr<user_data_stream_t>> &,
                           net::io_context &, ssl::context &ssl_context);

void websock_launcher(std::vector<std::shared_ptr<user_data_stream_t>> &,
                      net::io_context &io_context, ssl::context &ssl_context);

void persistent_orders_saver(net::io_context &, ssl::context &);

void monitor_database_host_table_changes();

std::string get_alphanum_tablename(std::string);
} // namespace binance
