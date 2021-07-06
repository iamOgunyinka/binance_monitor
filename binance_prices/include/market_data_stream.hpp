#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <optional>

#include "json_utils.hpp"

namespace binance {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websock = beast::websocket;
namespace http = beast::http;
namespace ip = net::ip;

class market_data_stream_t {
  static char const *const rest_api_host_;
  static char const *const ws_host_;
  static char const *const ws_port_number_;

  using resolver = ip::tcp::resolver;
  using results_type = resolver::results_type;

  net::io_context &io_context_;
  net::ssl::context &ssl_ctx_;
  std::optional<resolver> resolver_;
  std::optional<websock::stream<beast::ssl_stream<beast::tcp_stream>>>
      ssl_web_stream_;
  std::optional<beast::flat_buffer> buffer_;
  std::optional<http::request<http::empty_body>> http_request_;
  std::optional<http::response<http::string_body>> http_response_;

private:
  void rest_api_prepare_request();
  void rest_api_get_all_available_instruments();
  void rest_api_send_request();
  void rest_api_receive_response();
  void rest_api_on_data_received(beast::error_code const);
  void rest_api_initiate_connection();
  void rest_api_connect_to_resolved_names(results_type const &);
  void rest_api_perform_ssl_handshake(results_type::endpoint_type const &);

  void negotiate_websocket_connection();
  void initiate_websocket_connection();
  void websock_perform_ssl_handshake(results_type::endpoint_type const &);
  void websock_connect_to_resolved_names(results_type const &);
  void perform_websocket_handshake();
  void wait_for_messages();
  void interpret_generic_messages();
  void process_pushed_instruments_data(json::array_t const &);
  void process_pushed_tickers_data(json::array_t const &);

public:
  market_data_stream_t(net::io_context &, net::ssl::context &);
  void run();
};

namespace utilities {

std::optional<std::string> timet_to_string(std::size_t const t);

}
} // namespace binance
