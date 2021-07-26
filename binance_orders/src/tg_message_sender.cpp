#include "tg_message_sender.hpp"
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>

extern std::string BOT_TOKEN;

namespace binance {

char const *const tg_message_sender_t::tg_host_ = "api.telegram.org";

bool tg_message_sender_t::available_with_less_tasks() const {
  return !operation_completed_ && payloads_.size() < 10;
}

void tg_message_sender_t::add_payload(tg_payload_t &&payload) {
  payloads_.push_back(std::move(payload));
}

void tg_message_sender_t::run() {
  resolver_ = std::make_unique<resolver>(io_context_);
  resolver_->async_resolve(
      tg_host_, "https",
      [self = shared_from_this()](auto const &ec, auto const &resolves) {
        if (ec) {
          self->operation_completed_ = true;
          return self->error_callback_(ec.message());
        }
        self->connect_to_server(resolves);
      });
}

void tg_message_sender_t::connect_to_server(
    resolver::results_type const &resolves) {
  resolver_.reset(); // the resolver is done
  ssl_web_stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(
      io_context_, ssl_ctx_);
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(45));
  beast::get_lowest_layer(*ssl_web_stream_)
      .async_connect(resolves, [self = shared_from_this()](
                                   beast::error_code const ec, auto const &) {
        if (ec) {
          self->operation_completed_ = true;
          return self->error_callback_(ec.message());
        }
        return self->perform_ssl_handshake();
      });
}

void tg_message_sender_t::perform_ssl_handshake() {
  if (!SSL_set_tlsext_host_name(ssl_web_stream_->native_handle(), tg_host_)) {
    auto const ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                                      net::error::get_ssl_category());
    return error_callback_(ec.message());
  }
  ssl_web_stream_->async_handshake(
      net::ssl::stream_base::client,
      [self = shared_from_this()](beast::error_code const ec) {
        if (ec) {
          self->operation_completed_ = true;
          return self->error_callback_(ec.message());
        }
        self->send_request();
      });
}

void tg_message_sender_t::prepare_payload() {
  using http::field;
  using http::verb;
  auto payload = payloads_.front();
  payloads_.pop_front();
  auto const target = "/bot" + BOT_TOKEN +
                      "/sendMessage?chat_id=" + payload.chat_id +
                      "&text=" + payload.text;

  http_request_ = std::make_unique<http::request<http::string_body>>();
  http_request_->method(verb::post);
  http_request_->target(target);
  http_request_->version(11);
  http_request_->set(field::host, tg_host_);
  http_request_->set(field::content_type, "application/x-www-form-urlencoded");
  http_request_->body() = {};
  http_request_->prepare_payload();
}

void tg_message_sender_t::send_request() {
  prepare_payload();
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(30));

  http::async_write(
      *ssl_web_stream_, *http_request_,
      [self = shared_from_this()](beast::error_code ec, std::size_t const) {
        self->http_request_.reset();
        if (ec) {
          self->operation_completed_ = true;
          return self->error_callback_(ec.message());
        }
        return self->receive_data();
      });
}

void tg_message_sender_t::receive_data() {
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(45));
  http_response_ = std::make_unique<http::response<http::string_body>>();
  buffer_.emplace();

  http::async_read(
      *ssl_web_stream_, *buffer_, *http_response_,
      [self = shared_from_this()](beast::error_code ec, std::size_t const) {
        if (ec) {
          self->operation_completed_ = true;
          return self->error_callback_(ec.message());
        }
        self->completion_callback_(self->http_response_->body());
        self->http_response_.reset();
        return self->send_next_request();
      });
}

void tg_message_sender_t::send_next_request() {
  operation_completed_ = payloads_.empty();
  if (!operation_completed_) {
    send_request();
  }
}

} // namespace binance
