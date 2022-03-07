#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <spdlog/spdlog.h>
#include <thread>

#include "websock_launcher.hpp"

int main(int argc, char *argv[]) {
  spdlog::info(
      "binance_prices: a system for monitoring crypto prices on Binance");

  std::thread price_monitorer{binance::background_price_saver};
  price_monitorer.detach();

  net::io_context io_context{
      static_cast<int>(std::thread::hardware_concurrency())};
  net::ssl::context ssl_context(net::ssl::context::tlsv12_client);
  ssl_context.set_default_verify_paths();
  ssl_context.set_verify_mode(net::ssl::verify_none);

  std::unique_ptr<binance::market_data_stream_t> websock;
  binance::launch_price_watcher(websock, io_context, ssl_context);

  io_context.run();
  return EXIT_SUCCESS;
}
