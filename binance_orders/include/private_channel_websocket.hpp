#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <memory>
#include <optional>

#include "host_info.hpp"
#include "json_utils.hpp"

namespace binance {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace websock = beast::websocket;
namespace ip = net::ip;

class private_channel_websocket_t
    : public std::enable_shared_from_this<private_channel_websocket_t> {
  using resolver = ip::tcp::resolver;

  static char const *const ws_host_;
  static char const *const ws_port_number_;

  net::io_context &io_context_;
  net::ssl::context &ssl_ctx_;
  std::unique_ptr<net::ip::tcp::resolver> resolver_;
  std::optional<websock::stream<beast::ssl_stream<beast::tcp_stream>>>
      ssl_web_stream_;
  std::optional<host_info_t> host_info_;
  std::optional<beast::flat_buffer> buffer_;
  std::optional<http::request<http::empty_body>> http_request_;
  std::optional<http::response<http::string_body>> http_response_;
  std::optional<std::string> listen_key_;

  bool stopped_ = false;

private:
  void rest_api_initiate_connection();
  void rest_api_connect_to(resolver::results_type const &);
  void rest_api_perform_ssl_connection(
      resolver::results_type::endpoint_type const &);
  void rest_api_get_listen_key();
  void rest_api_prepare_request();
  void rest_api_send_request();
  void rest_api_receive_response();
  void rest_api_on_data_received(beast::error_code const ec);

  void initiate_websocket_connection();
  void connect_to_resolved_names(resolver::results_type const &);
  void perform_ssl_handshake(resolver::results_type::endpoint_type const &);
  void perform_websocket_handshake();
  void wait_for_messages();
  void interpret_generic_messages();

  void process_orders_execution_report(json::object_t const &);
  //void process_balance_update_report(json::object_t const &);
  //void process_outbound_account_position(json::object_t const &);

public:
  private_channel_websocket_t(net::io_context &, net::ssl::context &,
                              host_info_t &&);
  void run();
  void stop();
  host_info_t &host_info() { return *host_info_; }
};

namespace utilities {
std::string base64_encode(std::basic_string<unsigned char> const &binary_data);
std::string base64_encode(std::string const &binary_data);

std::optional<std::string> timet_to_okex_timezone();
std::optional<std::string> timet_to_string(std::size_t const t);
std::optional<std::string> timet_to_string(std::string const &);
} // namespace utilities

} // namespace binance
