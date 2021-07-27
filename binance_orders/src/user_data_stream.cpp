#include "user_data_stream.hpp"
#include "common/crypto.hpp"
#include "listen_key_keepalive.hpp"
#include "request_handler.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>

namespace binance {

using namespace fmt::v7::literals;

char const *const user_data_stream_t::rest_api_host_ = "api.binance.com";
char const *const user_data_stream_t::ws_host_ = "stream.binance.com";
char const *const user_data_stream_t::ws_port_number_ = "9443";

using namespace fmt::v7::literals;

user_data_stream_t::user_data_stream_t(net::io_context &io_context,
                                       net::ssl::context &ssl_ctx,
                                       host_info_t &&host_info)
    : io_context_{io_context}, ssl_ctx_{ssl_ctx},
      ssl_web_stream_{}, resolver_{}, host_info_{std::move(host_info)} {}

user_data_stream_t::~user_data_stream_t() {
  if (ssl_web_stream_.has_value()) {
    ssl_web_stream_->close({});
  }
  buffer_.reset();
  on_error_timer_.reset();
  listen_key_timer_.reset();
  ssl_web_stream_.reset();
  listen_key_keepalive_.reset();
}

void user_data_stream_t::run() { rest_api_initiate_connection(); }

void user_data_stream_t::stop() {
  stopped_ = true;

  if (ssl_web_stream_) {
    ssl_web_stream_->async_close(
        websock::close_reason{},
        [](beast::error_code const ec) { spdlog::error(ec.message()); });
  }
}

void user_data_stream_t::rest_api_initiate_connection() {
  if (stopped_) {
    return;
  }

  listen_key_timer_.reset();
  on_error_timer_.reset();
  resolver_.emplace(io_context_);

  resolver_->async_resolve(
      rest_api_host_, "https",
      [self = shared_from_this()](auto const error_code,
                                  resolver::results_type const &results) {
        if (error_code) {
          return spdlog::error(error_code.message());
        }
        self->rest_api_connect_to(results);
      });
}

void user_data_stream_t::rest_api_connect_to(
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

void user_data_stream_t::rest_api_perform_ssl_connection(
    resolver::results_type::endpoint_type const &ip) {
  auto const host = "{}:{}"_format(rest_api_host_, ip.port());

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

void user_data_stream_t::rest_api_get_listen_key() {
  rest_api_prepare_request();
  rest_api_send_request();
}

void user_data_stream_t::rest_api_prepare_request() {
  using http::field;
  using http::verb;

  auto &request = http_request_.emplace();
  request.method(verb::post);
  request.version(11);
  request.target("/api/v3/userDataStream");
  request.set(field::host, rest_api_host_);
  request.set(field::user_agent, "PostmanRuntime/7.28.1");
  request.set(field::accept, "*/*");
  request.set(field::accept_language, "en-US,en;q=0.5 --compressed");
  request.set("X-MBX-APIKEY", host_info_->api_key);
  request.body() = {};
  request.prepare_payload();
}

void user_data_stream_t::rest_api_send_request() {
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

void user_data_stream_t::rest_api_receive_response() {
  http_request_.reset();
  listen_key_.reset();

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

void user_data_stream_t::rest_api_on_data_received(beast::error_code const ec) {
  if (ec) {
    return spdlog::error(ec.message());
  }

  try {
    auto const result =
        json::parse(http_response_->body()).get<json::object_t>();
    if (auto const listen_key_iter = result.find("listenKey");
        listen_key_iter != result.cend()) {
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
  return ws_initiate_connection();
}

void user_data_stream_t::ws_initiate_connection() {
  if (stopped_ || !listen_key_.has_value()) {
    return;
  }
  ssl_web_stream_.emplace(io_context_, ssl_ctx_);
  resolver_.emplace(io_context_);

  resolver_->async_resolve(
      ws_host_, ws_port_number_,
      [self = shared_from_this()](
          auto const error_code,
          net::ip::tcp::resolver::results_type const &results) {
        if (error_code) {
          return spdlog::error(error_code.message());
        }
        self->ws_connect_to_names(results);
      });
}

void user_data_stream_t::ws_connect_to_names(
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
            self->ws_perform_ssl_handshake(connected_name);
          });
}

void user_data_stream_t::ws_perform_ssl_handshake(
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
        return self->ws_upgrade_to_websocket();
      });
}

// this sends an upgrade from HTTPS to ws protocol and thus ws handshake begins
// https://binance-docs.github.io/apidocs/spot/en/#user-data-streams

void user_data_stream_t::ws_upgrade_to_websocket() {
  auto const binance_handshake_path = "/ws/" + listen_key_.value();
  auto opt = websock::stream_base::timeout();

  opt.idle_timeout = std::chrono::minutes(5);
  opt.handshake_timeout = std::chrono::seconds(20);

  // enable the automatic keepalive pings
  opt.keep_alive_pings = true;
  ssl_web_stream_->set_option(opt);

  ssl_web_stream_->control_callback(
      [self = shared_from_this()](auto const frame_type, auto const &) {
        if (frame_type == websock::frame_type::close) {
          if (!self->stopped_) {
            return self->on_ws_connection_severed();
          }
        } else if (frame_type == websock::frame_type::pong) {
          spdlog::info("pong...");
        }
      });

  ssl_web_stream_->async_handshake(
      ws_host_, binance_handshake_path,
      [self = shared_from_this()](beast::error_code const ec) {
        if (ec) {
          return spdlog::error(ec.message());
        }

        if (!self->listen_key_timer_) {
          self->activate_listen_key_keepalive();
        }
        self->ws_wait_for_messages();
      });
}

void user_data_stream_t::ws_wait_for_messages() {
  buffer_.emplace();
  ssl_web_stream_->async_read(
      *buffer_, [self = shared_from_this()](beast::error_code const error_code,
                                            std::size_t const) {
        if (error_code) {
          spdlog::error(error_code.message());
          return self->on_ws_connection_severed();
        }
        self->ws_interpret_generic_messages();
      });
}

void user_data_stream_t::ws_interpret_generic_messages() {
  char const *buffer_cstr = static_cast<char const *>(buffer_->cdata().data());
  std::string_view const buffer(buffer_cstr, buffer_->size());

  try {
    json::object_t const root = json::parse(buffer).get<json::object_t>();
    if (auto const event_iter = root.find("e"); event_iter != root.cend()) {
      auto const event_type = event_iter->second.get<json::string_t>();

      // only three events are expected
      if (event_type == "executionReport") {
        ws_process_orders_execution_report(root);
      } else if (event_type == "balanceUpdate") {
        ws_process_balance_update(root);
      } else if (event_type == "outboundAccountPosition") {
        ws_process_account_position(root);
      }
    }
  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }

  return ws_wait_for_messages();
}

void user_data_stream_t::on_periodic_time_timeout() {
  listen_key_timer_->expires_after(std::chrono::minutes(30));
  listen_key_timer_->async_wait([self = shared_from_this()](
                                    boost::system::error_code const &ec) {
    if (ec || !self->ssl_web_stream_.has_value()) {
      return;
    }
    self->listen_key_keepalive_ = std::make_unique<listen_key_keepalive_t>(
        self->io_context_, self->ssl_ctx_, *self->listen_key_,
        self->host_info_->api_key);
    self->listen_key_keepalive_->run();
    self->listen_key_timer_->cancel();
    net::post(self->io_context_, [self] { self->on_periodic_time_timeout(); });
  });
}

void user_data_stream_t::on_ws_connection_severed() {
  if (ssl_web_stream_.has_value()) {
    ssl_web_stream_->close({});
    ssl_web_stream_.reset();
  }
  listen_key_.reset();
  listen_key_timer_.reset();
  listen_key_keepalive_.reset();
  buffer_.reset();

  auto &timer = on_error_timer_.emplace(io_context_);
  timer.expires_after(std::chrono::seconds(10));
  timer.async_wait([self = shared_from_this()](auto const &ec) {
    if (ec) {
      return spdlog::error(ec.message());
    }
    self->rest_api_initiate_connection();
  });
}

void user_data_stream_t::activate_listen_key_keepalive() {
  listen_key_timer_.emplace(io_context_);
  on_periodic_time_timeout();
}

void process_timet(std::string &result, std::size_t const time_t_value_ms) {
  std::size_t const time_t_value = time_t_value_ms / 1'000;
  if (auto opt_event_time = utilities::timet_to_string(time_t_value);
      opt_event_time.has_value()) {
    result = std::move(*opt_event_time);
  }
}

// https://binance-docs.github.io/apidocs/spot/en/#payload-order-update
void user_data_stream_t::ws_process_orders_execution_report(
    json::object_t const &order_object) {
  using utilities::get_value;

  ws_order_info_t order_info{};
  order_info.instrument_id = get_value<string_t>(order_object, "s");
  order_info.order_side = get_value<string_t>(order_object, "S");
  order_info.order_type = get_value<string_t>(order_object, "o");
  order_info.time_in_force = get_value<string_t>(order_object, "f");
  order_info.quantity_purchased = get_value<string_t>(order_object, "q");
  order_info.order_price = get_value<string_t>(order_object, "p");
  order_info.stop_price = get_value<string_t>(order_object, "P");
  order_info.execution_type = get_value<string_t>(order_object, "x");
  order_info.order_status = get_value<string_t>(order_object, "X");
  order_info.reject_reason = get_value<string_t>(order_object, "r");
  order_info.last_filled_quantity = get_value<string_t>(order_object, "l");
  order_info.commission_amount = get_value<string_t>(order_object, "n");
  order_info.last_executed_price = get_value<string_t>(order_object, "L");
  order_info.cummulative_filled_quantity =
      get_value<string_t>(order_object, "z");

  order_info.order_id = std::to_string(get_value<inumber_t>(order_object, "i"));
  order_info.trade_id = std::to_string(get_value<inumber_t>(order_object, "t"));

  if (auto const commission_asset_iter = order_object.find("N");
      commission_asset_iter != order_object.cend()) {

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

  process_timet(order_info.event_time, get_value<inumber_t>(order_object, "E"));
  process_timet(order_info.transaction_time,
                get_value<inumber_t>(order_object, "T"));
  process_timet(order_info.created_time,
                get_value<inumber_t>(order_object, "O"));

  order_info.for_aliased_account = host_info_->account_alias;
  order_info.telegram_group = host_info_->tg_group_name;

  auto &streams_container = request_handler_t::get_stream_container();
  streams_container.append(std::move(order_info));
}

// https://binance-docs.github.io/apidocs/spot/en/#payload-balance-update
void user_data_stream_t::ws_process_balance_update(
    json::object_t const &balance_object) {
  using utilities::get_value;

  ws_balance_info_t balance_data{};
  balance_data.balance = get_value<string_t>(balance_object, "d");
  balance_data.instrument_id = get_value<string_t>(balance_object, "a");

  process_timet(balance_data.event_time,
                get_value<inumber_t>(balance_object, "E"));
  process_timet(balance_data.clear_time,
                get_value<inumber_t>(balance_object, "T"));
  balance_data.for_aliased_account = host_info_->account_alias;
  balance_data.telegram_group = host_info_->tg_group_name;

  auto &streams_container = request_handler_t::get_stream_container();
  streams_container.append(std::move(balance_data));
}

// https://binance-docs.github.io/apidocs/spot/en/#payload-account-update
void user_data_stream_t::ws_process_account_position(
    json::object_t const &account_object) {
  using utilities::get_value;

  std::string event_time{}, last_account_update{};

  process_timet(event_time, get_value<inumber_t>(account_object, "E"));
  process_timet(last_account_update, get_value<inumber_t>(account_object, "u"));

  ws_account_update_t data{};
  data.event_time = event_time;
  data.last_account_update = last_account_update;
  data.for_aliased_account = host_info_->account_alias;
  data.telegram_group = host_info_->tg_group_name;

  auto const balances_array = account_object.at("B").get<json::array_t>();
  std::vector<ws_account_update_t> updates{};
  updates.reserve(balances_array.size());

  for (auto const &json_item : balances_array) {
    auto const asset_item = json_item.get<json::object_t>();
    data.instrument_id = get_value<string_t>(asset_item, "a");
    data.free_amount = get_value<string_t>(asset_item, "f");
    data.locked_amount = get_value<string_t>(asset_item, "l");
    updates.push_back(data);
  }
  auto &streams_container = request_handler_t::get_stream_container();
  streams_container.append_list(std::move(updates));
}

} // namespace binance
