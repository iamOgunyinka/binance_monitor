#include "market_data_stream.hpp"
#include "crypto.hpp"
#include "request_handler.hpp"

#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>

namespace binance {

char const *const market_data_stream_t::rest_api_host_ = "api.binance.com";
char const *const market_data_stream_t::ws_host_ = "stream.binance.com";
char const *const market_data_stream_t::ws_port_number_ = "9443";

using namespace fmt::v7::literals;

market_data_stream_t::market_data_stream_t(net::io_context &io_context,
                                           net::ssl::context &ssl_ctx)
    : io_context_{io_context}, ssl_ctx_{ssl_ctx},
      ssl_web_stream_{}, resolver_{} {}

void market_data_stream_t::run() { rest_api_initiate_connection(); }

void market_data_stream_t::rest_api_initiate_connection() {
  resolver_.emplace(io_context_);

  resolver_->async_resolve(
      rest_api_host_, "https",
      [this](auto const error_code,
             net::ip::tcp::resolver::results_type const &results) {
        if (error_code) {
          return spdlog::error(error_code.message());
        }
        rest_api_connect_to_resolved_names(results);
      });
}

void market_data_stream_t::rest_api_connect_to_resolved_names(
    results_type const &resolved_names) {

  resolver_.reset();
  ssl_web_stream_.emplace(io_context_, ssl_ctx_);
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(30));

  beast::get_lowest_layer(*ssl_web_stream_)
      .async_connect(
          resolved_names,
          [this](auto const error_code,
                 net::ip::tcp::resolver::results_type::endpoint_type const
                     &connected_name) {
            if (error_code) {
              return spdlog::error(error_code.message());
            }
            rest_api_perform_ssl_handshake(connected_name);
          });
}

void market_data_stream_t::rest_api_perform_ssl_handshake(
    results_type::endpoint_type const &ep) {
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(15));
  // Set SNI Hostname (many hosts need this to handshake successfully)
  if (!SSL_set_tlsext_host_name(ssl_web_stream_->next_layer().native_handle(),
                                rest_api_host_)) {
    auto const ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                                      net::error::get_ssl_category());
    return spdlog::error(ec.message());
  }

  ssl_web_stream_->next_layer().async_handshake(
      net::ssl::stream_base::client, [this](beast::error_code const ec) {
        if (ec) {
          return spdlog::error(ec.message());
        }
        return rest_api_get_all_available_instruments();
      });
}

void market_data_stream_t::rest_api_get_all_available_instruments() {
  rest_api_prepare_request();
  return rest_api_send_request();
}

void market_data_stream_t::rest_api_prepare_request() {
  using http::field;
  using http::verb;

  auto &request = http_request_.emplace();
  request.method(verb::get);
  request.version(11);
  request.target("/api/v3/ticker/price");
  request.set(field::host, rest_api_host_);
  request.set(field::user_agent, "PostmanRuntime/7.28.1");
  request.set(field::accept, "*/*");
  request.set(field::accept_language, "en-US,en;q=0.5 --compressed");
}

void market_data_stream_t::rest_api_send_request() {
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(20));
  http::async_write(ssl_web_stream_->next_layer(), *http_request_,
                    [this](beast::error_code const ec, std::size_t const) {
                      if (ec) {
                        return spdlog::error(ec.message());
                      }
                      rest_api_receive_response();
                    });
}

void market_data_stream_t::rest_api_receive_response() {
  http_request_.reset();
  buffer_.emplace();
  http_response_.emplace();

  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(20));
  http::async_read(ssl_web_stream_->next_layer(), *buffer_, *http_response_,
                   [this](beast::error_code ec, std::size_t const sz) {
                     rest_api_on_data_received(ec);
                   });
}

void market_data_stream_t::rest_api_on_data_received(
    beast::error_code const ec) {
  if (ec) {
    return spdlog::error(ec.message());
  }

  try {
    auto const token_list =
        json::parse(http_response_->body()).get<json::array_t>();
    process_pushed_instruments_data(token_list);
    return initiate_websocket_connection();
  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }
}

void market_data_stream_t::initiate_websocket_connection() {
  resolver_.emplace(io_context_);

  resolver_->async_resolve(
      ws_host_, ws_port_number_,
      [this](auto const error_code,
             net::ip::tcp::resolver::results_type const &results) {
        if (error_code) {
          return spdlog::error(error_code.message());
        }
        websock_connect_to_resolved_names(results);
      });
}

void market_data_stream_t::websock_connect_to_resolved_names(
    results_type const &resolved_names) {
  resolver_.reset();
  ssl_web_stream_.emplace(io_context_, ssl_ctx_);
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(30));

  beast::get_lowest_layer(*ssl_web_stream_)
      .async_connect(
          resolved_names,
          [this](auto const error_code,
                 net::ip::tcp::resolver::results_type::endpoint_type const
                     &connected_name) {
            if (error_code) {
              return spdlog::error(error_code.message());
            }
            websock_perform_ssl_handshake(connected_name);
          });
}

void market_data_stream_t::websock_perform_ssl_handshake(
    results_type::endpoint_type const &ep) {
  auto const host = "{}:{}"_format(ws_host_, ep.port());

  // Set a timeout on the operation
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(30));

  // Set SNI Hostname (many hosts need this to handshake successfully)
  if (!SSL_set_tlsext_host_name(ssl_web_stream_->next_layer().native_handle(),
                                host.c_str())) {
    auto const ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                                      net::error::get_ssl_category());
    return spdlog::error(ec.message());
  }
  negotiate_websocket_connection();
}

void market_data_stream_t::negotiate_websocket_connection() {
  http_request_.reset();
  http_response_.reset();

  ssl_web_stream_->next_layer().async_handshake(
      net::ssl::stream_base::client, [this](beast::error_code const ec) {
        if (ec) {
          return spdlog::error(ec.message());
        }
        beast::get_lowest_layer(*ssl_web_stream_).expires_never();
        return perform_websocket_handshake();
      });
}

void market_data_stream_t::perform_websocket_handshake() {
  static auto const binance_handshake_path = "/ws/!miniTicker@arr";

  auto opt = websock::stream_base::timeout();
  opt.idle_timeout = std::chrono::seconds(20);
  opt.handshake_timeout = std::chrono::seconds(5);
  opt.keep_alive_pings = true;
  ssl_web_stream_->set_option(opt);

  ssl_web_stream_->control_callback(
      [this](auto const frame_type, auto const &) {
        if (frame_type == websock::frame_type::close) {
          ssl_web_stream_.reset();
          return initiate_websocket_connection();
        }
      });

  ssl_web_stream_->async_handshake(ws_host_, binance_handshake_path,
                                   [this](beast::error_code const ec) {
                                     if (ec) {
                                       return spdlog::error(ec.message());
                                     }

                                     wait_for_messages();
                                   });
}

void market_data_stream_t::wait_for_messages() {
  buffer_.emplace();
  ssl_web_stream_->async_read(
      *buffer_, [this](beast::error_code const error_code, std::size_t const) {
        if (error_code == net::error::operation_aborted) {
          return spdlog::error(error_code.message());
        } else if (error_code) {
          spdlog::error(error_code.message());
          ssl_web_stream_.reset();
          return initiate_websocket_connection();
        }
        interpret_generic_messages();
      });
}

void market_data_stream_t::interpret_generic_messages() {
  char const *buffer_cstr = static_cast<char const *>(buffer_->cdata().data());
  std::string_view const buffer(buffer_cstr, buffer_->size());

  try {
    auto object_list = json::parse(buffer).get<json::array_t>();
    process_pushed_tickers_data(std::move(object_list));
  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }

  return wait_for_messages();
}

void market_data_stream_t::process_pushed_instruments_data(
    json::array_t const &data_list) {

  std::vector<instrument_type_t> instruments{};
  instruments.reserve(data_list.size());
  for (auto const &data_json : data_list) {
    auto const data_object = data_json.get<json::object_t>();
    std::string instrument_id = data_object.at("symbol").get<json::string_t>();

    instruments.push_back({instrument_id});
  }
  request_handler_t::get_all_listed_instruments().insert_list(
      std::move(instruments));
}

void market_data_stream_t::process_pushed_tickers_data(
    json::array_t const &data_list) {

  std::vector<pushed_subscription_data_t> pushed_list{};
  pushed_list.reserve(data_list.size());

  for (auto const &data_json : data_list) {
    pushed_subscription_data_t data{};
    auto const data_object = data_json.get<json::object_t>();
    // symbol => BTCDOGE, DOGEUSDT etc
    data.instrument_id = data_object.at("s").get<json::string_t>();
    data.current_price = std::stod(data_object.at("c").get<json::string_t>());
    data.open_24h = std::stod(data_object.at("o").get<json::string_t>());
    pushed_list.push_back(std::move(data));
  }

  auto &market_stream = request_handler_t::get_tokens_container();
  market_stream.append_list(std::move(pushed_list));
}

} // namespace binance
