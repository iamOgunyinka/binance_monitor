#pragma once

#include "user_data_stream.hpp"
#include <map>

namespace binance {

namespace net = boost::asio;
namespace ssl = net::ssl;

struct tg_handler_t {
  using chat_id_t = std::string;
  using chat_name_t = std::string;
  static std::map<chat_name_t, chat_id_t> chat_map_;
};

void launch_previous_hosts(std::vector<std::shared_ptr<user_data_stream_t>> &,
                           net::io_context &, ssl::context &ssl_context);

void websock_launcher(std::vector<std::shared_ptr<user_data_stream_t>> &,
                      net::io_context &io_context, ssl::context &ssl_context);

void background_persistent_orders_saver(net::io_context &, ssl::context &);

void monitor_database_host_table_changes();

std::string get_alphanum_tablename(std::string);
} // namespace binance
