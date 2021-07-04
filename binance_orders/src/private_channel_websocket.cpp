#include "private_channel_websocket.hpp"
#include "crypto.hpp"
#include "request_handler.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>

namespace binance {

char const *const private_channel_websocket_t::ws_host_ = "stream.binance.com";
char const *const private_channel_websocket_t::ws_port_number_ = "9443";

using namespace fmt::v7;

private_channel_websocket_t::private_channel_websocket_t(
    net::io_context &io_context, net::ssl::context &ssl_ctx,
    host_info_t &&host_info)
    : io_context_{io_context}, ssl_ctx_{ssl_ctx},
      ssl_web_stream_{}, resolver_{nullptr}, host_info_{std::move(host_info)} {}

void private_channel_websocket_t::run() { rest_api_initiate_connection(); }

void private_channel_websocket_t::stop() {
  stopped_ = true;

  if (ssl_web_stream_) {
    ssl_web_stream_->async_close(
        websock::close_reason{},
        [](beast::error_code const ec) { spdlog::error(ec.message()); });
  }
}

void private_channel_websocket_t::rest_api_initiate_connection() {
  if (stopped_) {
    return;
  }

  resolver_ = std::make_unique<resolver>(io_context_);
  resolver_->async_resolve(
      "api.binance.com", "https",
      [self = shared_from_this()](auto const error_code,
                                  resolver::results_type const &results) {
        if (error_code) {
          return spdlog::error(error_code.message());
        }
        self->rest_api_connect_to(results);
      });
}

void private_channel_websocket_t::rest_api_connect_to(
    resolver::results_type const &connections) {
  resolver_.reset();
  ssl_web_stream_.emplace(io_context_, ssl_ctx_);
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(30));
  beast::get_lowest_layer(*ssl_web_stream_)
      .async_connect(connections,
                     [self = shared_from_this()](
                         beast::error_code const error_code,
                         resolver::results_type::endpoint_type const &ip) {
                       if (error_code) {
                         return spdlog::error(error_code.message());
                       }
                       self->rest_api_perform_ssl_connection(ip);
                     });
}

void private_channel_websocket_t::rest_api_perform_ssl_connection(
    resolver::results_type::endpoint_type const &ip) {
  auto const host = "api.binance.com:" + std::to_string(ip.port());
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
  ssl_web_stream_->next_layer().async_handshake(
      net::ssl::stream_base::client,
      [self = shared_from_this()](beast::error_code const ec) {
        if (ec) {
          return spdlog::error(ec.message());
        }
        self->rest_api_get_listen_key();
      });
}

void private_channel_websocket_t::rest_api_get_listen_key() {
  rest_api_prepare_request();
  rest_api_send_request();
}

void private_channel_websocket_t::rest_api_prepare_request() {
  using http::field;
  using http::verb;

  auto &request = http_request_.emplace();
  request.method(verb::post);
  request.version(11);
  request.target("/api/v3/userDataStream");
  request.set(field::host, "api.binance.com");
  request.set(field::user_agent, "PostmanRuntime/7.28.1");
  request.set(field::accept, "*/*");
  request.set(field::accept_language, "en-US,en;q=0.5 --compressed");
  request.set("X-MBX-APIKEY", host_info_->api_key);
  request.body() = {};
  request.prepare_payload();
}

void private_channel_websocket_t::rest_api_send_request() {
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(20));
  http::async_write(ssl_web_stream_->next_layer(), *http_request_,
                    [self = shared_from_this()](beast::error_code const ec,
                                                std::size_t const) {
                      if (ec) {
                        return spdlog::error(ec.message());
                      }
                      self->rest_api_receive_response();
                    });
}

void private_channel_websocket_t::rest_api_receive_response() {
  http_request_.reset();
  buffer_.emplace();
  http_response_.emplace();

  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(20));
  http::async_read(
      ssl_web_stream_->next_layer(), *buffer_, *http_response_,
      [self = shared_from_this()](beast::error_code ec, std::size_t const sz) {
        self->rest_api_on_data_received(ec);
      });
}

void private_channel_websocket_t::rest_api_on_data_received(
    beast::error_code const ec) {
  if (ec) {
    return spdlog::error(ec.message());
  }

  try {
    auto const result =
        json::parse(http_response_->body()).get<json::object_t>();
    if (auto listen_key_iter = result.find("listenKey");
        listen_key_iter != result.end()) {
      listen_key_.emplace(listen_key_iter->second.get<json::string_t>());
    } else {
      spdlog::error(http_response_->body());
      return http_response_.reset();
    }
  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }
  http_response_.reset();
  ssl_web_stream_.reset();
  return initiate_websocket_connection();
}

void private_channel_websocket_t::initiate_websocket_connection() {
  if (stopped_) {
    return;
  }
  ssl_web_stream_.emplace(io_context_, ssl_ctx_);
  resolver_ = std::make_unique<net::ip::tcp::resolver>(io_context_);

  resolver_->async_resolve(
      ws_host_, ws_port_number_,
      [self = shared_from_this()](
          auto const error_code,
          net::ip::tcp::resolver::results_type const &results) {
        if (error_code) {
          return spdlog::error(error_code.message());
        }
        self->connect_to_resolved_names(results);
      });
}

void private_channel_websocket_t::connect_to_resolved_names(
    net::ip::tcp::resolver::results_type const &resolved_names) {

  resolver_.reset();
  beast::get_lowest_layer(*ssl_web_stream_)
      .expires_after(std::chrono::seconds(30));

  beast::get_lowest_layer(*ssl_web_stream_)
      .async_connect(
          resolved_names,
          [self = shared_from_this()](
              auto const error_code,
              net::ip::tcp::resolver::results_type::endpoint_type const
                  &connected_name) {
            if (error_code) {
              return spdlog::error(error_code.message());
            }
            self->perform_ssl_handshake(connected_name);
          });
}

void private_channel_websocket_t::perform_ssl_handshake(
    net::ip::tcp::resolver::results_type::endpoint_type const &ep) {
  auto const host = ws_host_ + ':' + std::to_string(ep.port());

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
  ssl_web_stream_->next_layer().async_handshake(
      net::ssl::stream_base::client,
      [self = shared_from_this()](beast::error_code const ec) {
        if (ec) {
          return spdlog::error(ec.message());
        }
        beast::get_lowest_layer(*self->ssl_web_stream_).expires_never();
        return self->perform_websocket_handshake();
      });
}

void private_channel_websocket_t::perform_websocket_handshake() {
  auto const okex_handshake_path = "/ws/" + listen_key_.value();
  auto opt = websock::stream_base::timeout();

  opt.idle_timeout = std::chrono::seconds(30);
  opt.handshake_timeout = std::chrono::seconds(20);

  // enable the automatic keepalive pings
  opt.keep_alive_pings = true;
  ssl_web_stream_->set_option(opt);

  ssl_web_stream_->control_callback(
      [self = shared_from_this()](auto const frame_type, auto const &) {
        if (frame_type == websock::frame_type::close) {
          if (!self->stopped_) {
            self->ssl_web_stream_.reset();
            return self->initiate_websocket_connection();
          }
        } else if (frame_type == websock::frame_type::pong) {
          spdlog::info("pong...");
        }
      });

  ssl_web_stream_->async_handshake(
      ws_host_, okex_handshake_path,
      [self = shared_from_this()](beast::error_code const ec) {
        if (ec) {
          return spdlog::error(ec.message());
        }
        self->wait_for_messages();
      });
}

void private_channel_websocket_t::wait_for_messages() {
  buffer_.emplace();
  ssl_web_stream_->async_read(
      *buffer_, [self = shared_from_this()](beast::error_code const error_code,
                                            std::size_t const) {
        if (error_code) {
          spdlog::error(error_code.message());
          return self->initiate_websocket_connection();
        }
        self->interpret_generic_messages();
      });
}

void private_channel_websocket_t::interpret_generic_messages() {
  char const *buffer_cstr = static_cast<char const *>(buffer_->cdata().data());
  std::string_view const buffer(buffer_cstr, buffer_->size());

  try {
    json::object_t const root = json::parse(buffer).get<json::object_t>();
    if (auto event_iter = root.find("e"); event_iter != root.end()) {
      auto const event_type = event_iter->second.get<json::string_t>();
      
      // only three events are expected
      if (event_type == "executionReport") {
        process_orders_execution_report(root);
      } else if (event_type == "balanceUpdate") { // to-do
        // process_balance_update_report(root);
      } else if (event_type == "outboundAccountPosition") { // to-do
        // process_outbound_account_position(root);
      }
    }
  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }
  return wait_for_messages();
}

template <typename T>
T get_value(json::object_t const &data, std::string const &key) {
  if constexpr (std::is_same_v<T, json::number_integer_t>) {
    return data.at(key).get<json::number_integer_t>();
  } else if constexpr (std::is_same_v<T, json::string_t>) {
    return data.at(key).get<json::string_t>();
  } else if constexpr (std::is_same_v<T, json::number_float_t>) {
    return data.at(key).get<json::number_float_t>();
  }
  return {};
}

void process_timet(std::string &result, std::size_t const time_t_value_ms) {
  std::size_t const time_t_value = time_t_value_ms / 1'000;
  if (auto opt_event_time = utilities::timet_to_string(time_t_value);
      opt_event_time.has_value()) {
    result = std::move(*opt_event_time);
  }
}

/*** From Binance API doc
{
  "E": 1499405658658,            // Event time
  "s": "ETHBTC",                 // Symbol
  "S": "BUY",                    // Side
  "o": "LIMIT",                  // Order type
  "f": "GTC",                    // Time in force
  "q": "1.00000000",             // Order quantity
  "p": "0.10264410",             // Order price
  "P": "0.00000000",             // Stop price
  "F": "0.00000000",             // Iceberg quantity
  "x": "NEW",                    // Current execution type
  "X": "NEW",                    // Current order status
  "r": "NONE",                   // Order reject reason; will be an error code.
  "i": 4293153,                  // Order ID
  "l": "0.00000000",             // Last executed quantity
  "z": "0.00000000",             // Cumulative filled quantity
  "L": "0.00000000",             // Last executed price
  "n": "0",                      // Commission amount
  "N": null,                     // Commission asset
  "T": 1499405658657,            // Transaction time
  "t": -1,                       // Trade ID
  "O": 1499405658657,            // Order creation time
}
***/

void private_channel_websocket_t::process_orders_execution_report(
    json::object_t const &order_event) {
  using string_t = json::string_t;
  using inumber_t = json::number_integer_t;
  using fnumber_t = json::number_float_t;

  ws_order_info_t order_info{};
  order_info.instrument_id = get_value<string_t>(order_event, "s");
  order_info.order_side = get_value<string_t>(order_event, "S");
  order_info.order_type = get_value<string_t>(order_event, "o");
  order_info.time_in_force = get_value<string_t>(order_event, "f");
  order_info.quantity_purchased = get_value<string_t>(order_event, "q");
  order_info.order_price = get_value<string_t>(order_event, "p");
  order_info.stop_price = get_value<string_t>(order_event, "P");
  order_info.execution_type = get_value<string_t>(order_event, "x");
  order_info.order_status = get_value<string_t>(order_event, "X");
  order_info.reject_reason = get_value<string_t>(order_event, "r");
  order_info.last_filled_quantity = get_value<string_t>(order_event, "l");
  order_info.commission_amount = get_value<string_t>(order_event, "n");
  order_info.last_executed_price = get_value<string_t>(order_event, "L");
  order_info.cummulative_filled_quantity =
      get_value<string_t>(order_event, "z");

  order_info.order_id = std::to_string(get_value<inumber_t>(order_event, "i"));
  order_info.trade_id = std::to_string(get_value<inumber_t>(order_event, "t"));

  if (auto commission_asset_iter = order_event.find("N");
      commission_asset_iter != order_event.end()) {

    auto json_commission_asset = commission_asset_iter->second;
    // documentation doesn't specify the type of this data but
    // my best guess is that this type is most likely a string
    if (json_commission_asset.is_string()) {
      order_info.commission_asset = json_commission_asset.get<string_t>();
    } else if (json_commission_asset.is_number()) {
      order_info.commission_asset =
          std::to_string(json_commission_asset.get<fnumber_t>());
    }
  }

  process_timet(order_info.event_time, get_value<inumber_t>(order_event, "E"));
  process_timet(order_info.transaction_time,
                get_value<inumber_t>(order_event, "T"));
  process_timet(order_info.created_time,
                get_value<inumber_t>(order_event, "O"));

  order_info.for_aliased_account = host_info_->account_alias;
  order_info.telegram_group = host_info_->tg_group_name;

  auto &orders_container = request_handler_t::get_orders_container();
  orders_container.append(std::move(order_info));
}

} // namespace binance
