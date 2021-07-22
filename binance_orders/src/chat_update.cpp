#include "chat_update.hpp"
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>

extern std::string BOT_TOKEN;

namespace binance {

char const *const chat_update_t::host_ = "api.telegram.org";

chat_update_t::chat_update_t(net::io_context &io_context,
                             net::ssl::context &ssl_context,
                             completion_handler_t cb)
    : io_context_{io_context}, ssl_context_{ssl_context}, completion_cb_{cb} {}

void chat_update_t::run() {
  resolver_.emplace(io_context_);
  resolver_->async_resolve(
      host_, "https",
      [self = shared_from_this()](
          auto const error_code,
          net::ip::tcp::resolver::results_type const &results) {
        if (error_code) {
          return self->set_error(error_code.message());
        }
        self->connect_to_resolved_names(results);
      });
}

void chat_update_t::connect_to_resolved_names(
    results_type const &resolved_names) {
  resolver_.reset();
  ssl_stream_.emplace(io_context_, ssl_context_);
  beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(30));

  beast::get_lowest_layer(*ssl_stream_)
      .async_connect(
          resolved_names,
          [self = shared_from_this()](
              auto const error_code,
              net::ip::tcp::resolver::results_type::endpoint_type const
                  &connected_name) {
            if (error_code) {
              return self->set_error(error_code.message());
            }
            self->perform_ssl_handshake(connected_name);
          });
}

void chat_update_t::perform_ssl_handshake(
    results_type::endpoint_type const &ep) {
  beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(15));
  // Set SNI Hostname (many hosts need this to handshake successfully)
  if (!SSL_set_tlsext_host_name(ssl_stream_->native_handle(), host_)) {
    auto const ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                                      net::error::get_ssl_category());
    return set_error(ec.message());
  }

  ssl_stream_->async_handshake(
      net::ssl::stream_base::client,
      [self = shared_from_this()](beast::error_code const ec) {
        if (ec) {
          return self->set_error(ec.message());
        }
        return self->get_bot_updates();
      });
}

void chat_update_t::get_bot_updates() {
  prepare_http_request();
  send_http_request();
}

void chat_update_t::prepare_http_request() {
  using http::field;
  using http::verb;

  auto &request = http_request_.emplace();
  request.method(verb::get);
  request.version(11);
  request.target("/bot" + BOT_TOKEN + "/getUpdates");
  request.set(field::host, host_);
  request.set(field::user_agent, "BinanceAgent/1.0.0");
  request.set(field::accept, "*/*");
  request.set(field::accept_language, "en-US,en;q=0.5 --compressed");
}

void chat_update_t::send_http_request() {
  beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(20));
  http::async_write(*ssl_stream_, *http_request_,
                    [self = shared_from_this()](beast::error_code const ec,
                                                std::size_t const) {
                      if (ec) {
                        return self->set_error(ec.message());
                      }
                      self->receive_http_response();
                    });
}

void chat_update_t::receive_http_response() {
  http_request_.reset();
  buffer_.emplace();
  http_response_.emplace();

  beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(20));
  http::async_read(
      *ssl_stream_, *buffer_, *http_response_,
      [self = shared_from_this()](beast::error_code ec, std::size_t const sz) {
        if (ec) {
          return self->set_error(ec.message());
        }
        self->set_result(self->http_response_->body());
      });
}

void chat_update_t::set_error(std::string const &error_message) {
  if (completion_cb_) {
    completion_cb_({}, error_message);
  }
}

void chat_update_t::set_result(std::string const &msg) {
  if (completion_cb_) {
    completion_cb_(msg, {});
  }
}

} // namespace binance
