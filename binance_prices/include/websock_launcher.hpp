#pragma once

#include "market_data_stream.hpp"

namespace binance {

namespace net = boost::asio;
namespace ssl = net::ssl;

void launch_price_watcher(std::vector<std::unique_ptr<market_data_stream_t>> &,
                          net::io_context &, ssl::context &ssl_context);

void background_price_saver();
void task_scheduler_watcher(net::io_context &io_context);

std::string get_alphanum_tablename(std::string);
} // namespace binance
