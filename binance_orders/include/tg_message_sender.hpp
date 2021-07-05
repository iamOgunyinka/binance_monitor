#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <deque>
#include <memory>
#include <optional>

namespace binance {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using resolver = net::ip::tcp::resolver;
using PostOpCallback = void (*)(std::string const &);

class tg_message_sender_t
    : public std::enable_shared_from_this<tg_message_sender_t> {
  net::io_context &io_context_;
  net::ssl::context &ssl_ctx_;
  std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> ssl_web_stream_{
      nullptr};
  std::unique_ptr<resolver> resolver_;
  std::unique_ptr<http::request<http::string_body>> http_request_;
  std::unique_ptr<http::response<http::string_body>> http_response_;
  std::optional<beast::flat_buffer> buffer_{};

  std::deque<std::string> payloads_;
  PostOpCallback error_callback_;
  PostOpCallback completion_callback_;
  bool operation_completed_ = false;
  static char const *const tg_host_;

private:
  void run();
  void prepare_payload();
  void connect_to_server(resolver::results_type const &);
  void perform_ssl_handshake();
  void receive_data();
  void send_request();
  void send_next_request();

public:
  tg_message_sender_t(net::io_context &io_context, net::ssl::context &ssl_ctx,
                      std::string &&payload, PostOpCallback error_callback,
                      PostOpCallback comp_callback)
      : io_context_{io_context}, ssl_ctx_{ssl_ctx}, resolver_{nullptr},
        payloads_{std::move(payload)}, error_callback_{error_callback},
        completion_callback_{comp_callback} {}

  void start() { return run(); }
  bool completed_operation() { return operation_completed_; }
  bool available_with_less_tasks() const;
  void add_payload(std::string &&);
};
} // namespace binance
