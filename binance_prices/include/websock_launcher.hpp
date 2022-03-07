#pragma once

#include "market_data_stream.hpp"

namespace net = boost::asio;
namespace ssl = net::ssl;

namespace binance {

void launch_price_watcher(std::unique_ptr<market_data_stream_t> &,
                          net::io_context &, ssl::context &ssl_context);
void background_price_saver();

} // namespace binance
