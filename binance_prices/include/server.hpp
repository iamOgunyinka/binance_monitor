#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/error.hpp>
#include <list>
#include <memory>

namespace binance {

namespace utilities {

struct command_line_interface_t {
  uint16_t port{3420};
  std::string ip_address{"127.0.0.1"};
  std::string launch_type{"development"};
  std::string database_config_filename{"../config/info.json"};
};

} // namespace utilities

namespace net = boost::asio;
namespace beast = boost::beast;

using utilities::command_line_interface_t;
class session_t;

class server_t : public std::enable_shared_from_this<server_t> {
  net::io_context &io_context_;
  net::ip::tcp::endpoint const endpoint_;
  net::ip::tcp::acceptor acceptor_;
  bool is_open_{false};
  command_line_interface_t const &args_;
  std::list<std::shared_ptr<session_t>> sessions_;

public:
  server_t(net::io_context &context, command_line_interface_t const &args);
  void run();
  operator bool();

private:
  void on_connection_accepted(beast::error_code const &ec,
                              net::ip::tcp::socket socket);
  void accept_connections();
};

} // namespace binance
