#include "websock_launcher.hpp"
#include "market_data_stream.hpp"
#include "request_handler.hpp"
#include <boost/algorithm/string/case_conv.hpp>

namespace binance {

void launch_price_watcher(std::unique_ptr<market_data_stream_t> &websock,
                          net::io_context &io_context,
                          ssl::context &ssl_context) {
  websock.reset(new market_data_stream_t(io_context, ssl_context));
  websock->run();
}

void background_price_saver() {
  auto &token_container = request_handler_t::get_tokens_container();
  auto &pushed_subs = request_handler_t::get_all_pushed_data();

  while (true) {
    auto item = token_container.get();
    boost::to_upper(item.instrument_id);
    auto &data = pushed_subs[item.instrument_id];
    data.current_price = item.current_price;
    data.open_24h = item.open_24h;
    spdlog::info("{}: ${} (24h: ${})", item.instrument_id, item.current_price,
                 item.open_24h);
  }
}

} // namespace binance
