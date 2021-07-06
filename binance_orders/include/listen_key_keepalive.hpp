#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/ssl.hpp>
#include <optional>

namespace binance {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ip = net::ip;

/* https://binance-docs.github.io/apidocs/spot/en/#listen-key-spot
 * Keepalive a user data stream to prevent a time out. User data streams will
 * close after 60 minutes. It's recommended to send a ping about every 30
 * minutes.
 */
class listen_key_keepalive_t
    : public std::enable_shared_from_this<listen_key_keepalive_t> {

  using resolver = ip::tcp::resolver;

  static char const *const host_;

  net::io_context &io_context_;
  net::ssl::context &ssl_ctx_;
  std::optional<beast::flat_buffer> buffer_;
  std::optional<http::request<http::empty_body>> http_request_;
  std::optional<http::response<http::string_body>> http_response_;
  std::optional<net::ip::tcp::resolver> resolver_;
  std::optional<beast::ssl_stream<beast::tcp_stream>> ssl_stream_;

  std::string listen_key_;
  std::string api_key_;

private:
  void resolve_name();
  void connect_to_names(resolver::results_type const &);
  void perform_ssl_connection(resolver::results_type::endpoint_type const &);
  void renew_listen_key();
  void prepare_request();
  void send_request();
  void receive_response();
  void on_data_received();

public:
  listen_key_keepalive_t(net::io_context &, net::ssl::context &,
                         std::string listen_key, std::string api_key);
  void run();
};

} // namespace binance
