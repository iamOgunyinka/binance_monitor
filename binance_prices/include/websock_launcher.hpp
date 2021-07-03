#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>

namespace binance {

namespace net = boost::asio;
namespace ssl = net::ssl;

class orders_websock_t;

void launch_price_watcher(std::vector<std::unique_ptr<orders_websock_t>> &,
                          net::io_context &, ssl::context &ssl_context);

void background_price_saver();
void task_scheduler_watcher(net::io_context &io_context);

std::string get_alphanum_tablename(std::string);
} // namespace binance
