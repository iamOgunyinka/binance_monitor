#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <memory>
#include <optional>

namespace binance {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ip = net::ip;

using completion_handler_t = void (*)(std::string const &, std::string const &);

class chat_update_t : public std::enable_shared_from_this<chat_update_t> {
  using resolver = ip::tcp::resolver;
  using results_type = resolver::results_type;

  static char const *const host_;

  net::io_context &io_context_;
  net::ssl::context &ssl_context_;
  std::optional<resolver> resolver_;
  std::optional<beast::ssl_stream<beast::tcp_stream>> ssl_stream_;
  std::optional<beast::flat_buffer> buffer_;
  std::optional<http::request<http::empty_body>> http_request_;
  std::optional<http::response<http::string_body>> http_response_;
  completion_handler_t completion_cb_;

private:
  void connect_to_resolved_names(results_type const &resolved_names);
  void perform_ssl_handshake(results_type::endpoint_type const &ep);
  void get_bot_updates();
  void prepare_http_request();
  void send_http_request();
  void receive_http_response();
  void set_error(std::string const &);
  void set_result(std::string const &);

public:
  chat_update_t(net::io_context &io_context, net::ssl::context &ssl_context,
                completion_handler_t = nullptr);
  void run();
};
} // namespace binance
