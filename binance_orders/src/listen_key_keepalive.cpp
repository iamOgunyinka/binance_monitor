#include "listen_key_keepalive.hpp"

#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>
#include <spdlog/spdlog.h>

namespace binance {

using namespace fmt::v7::literals;

char const *const listen_key_keepalive_t::host_ = "api.binance.com";

listen_key_keepalive_t::listen_key_keepalive_t(net::io_context &io_context,
                                               net::ssl::context &ssl_context,
                                               std::string listen_key,
                                               std::string api_key)
    : io_context_{io_context}, ssl_ctx_{ssl_context},
      listen_key_{std::move(listen_key)}, api_key_{std::move(api_key)} {}

void listen_key_keepalive_t::run() { resolve_name(); }

void listen_key_keepalive_t::resolve_name() {
  spdlog::info("Running lkk to keep it alive...");

  resolver_.emplace(io_context_);
  resolver_->async_resolve(
      host_, "https",
      [this](auto const error_code, resolver::results_type const &results) {
        if (error_code) {
          return spdlog::error(error_code.message());
        }
        connect_to_names(results);
      });
}

void listen_key_keepalive_t::connect_to_names(
    resolver::results_type const &resolved_names) {
  resolver_.reset();
  ssl_stream_.emplace(io_context_, ssl_ctx_);
  beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(30));
  beast::get_lowest_layer(*ssl_stream_)
      .async_connect(resolved_names,
                     [this](beast::error_code const error_code,
                            resolver::results_type::endpoint_type const &ip) {
                       if (error_code) {
                         return spdlog::error(error_code.message());
                       }
                       perform_ssl_connection(ip);
                     });
}

void listen_key_keepalive_t::perform_ssl_connection(
    resolver::results_type::endpoint_type const &connected_name) {
  auto const host = "{}:{}"_format(host_, connected_name.port());

  // Set a timeout on the operation
  beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(30));

  // Set SNI Hostname (many hosts need this to handshake successfully)
  if (!SSL_set_tlsext_host_name(ssl_stream_->native_handle(), host.c_str())) {
    auto const ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                                      net::error::get_ssl_category());
    return spdlog::error(ec.message());
  }
  ssl_stream_->async_handshake(net::ssl::stream_base::client,
                               [this](beast::error_code const ec) {
                                 if (ec) {
                                   return spdlog::error(ec.message());
                                 }
                                 renew_listen_key();
                               });
}

void listen_key_keepalive_t::renew_listen_key() {
  prepare_request();
  send_request();
}

void listen_key_keepalive_t::prepare_request() {
  using http::field;
  using http::verb;

  auto &request = http_request_.emplace();
  request.method(verb::put);
  request.version(11);
  request.target("/api/v3/userDataStream?listenKey=" + listen_key_);
  request.set(field::host, host_);
  request.set(field::user_agent, "PostmanRuntime/7.28.1");
  request.set(field::accept, "*/*");
  request.set(field::accept_language, "en-US,en;q=0.5 --compressed");
  request.set("X-MBX-APIKEY", api_key_);
}

void listen_key_keepalive_t::send_request() {
  beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(20));
  http::async_write(*ssl_stream_, *http_request_,
                    [this](beast::error_code const ec, std::size_t const) {
                      if (ec) {
                        return spdlog::error(ec.message());
                      }
                      receive_response();
                    });
}

void listen_key_keepalive_t::receive_response() {
  http_request_.reset();
  buffer_.emplace();
  http_response_.emplace();

  beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(20));
  http::async_read(*ssl_stream_, *buffer_, *http_response_,
                   [this](beast::error_code ec, std::size_t const sz) {
                     if (ec) {
                       return spdlog::error(ec.message());
                     }
                     on_data_received();
                   });
}

void listen_key_keepalive_t::on_data_received() {
  spdlog::info("[LKK]received data: {}", http_response_->body());
  http_response_.reset();
  buffer_.reset();
}

} // namespace binance
