#include <CLI/CLI.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <thread>

#include "database_connector.hpp"
#include "server.hpp"
#include "websock_launcher.hpp"

namespace net = boost::asio;
std::string BEARER_TOKEN_SECRET_KEY;

int main(int argc, char *argv[]) {
  CLI::App cli_parser{"binance_prices: an asynchronous web server for "
                      "monitoring crypto prices"};
  binance::command_line_interface_t args{};

  cli_parser.add_option("-p", args.port, "port to bind server to", true);
  cli_parser.add_option("-a", args.ip_address, "IP address to use", true);
  cli_parser.add_option("-d", args.database_config_filename,
                        "Database config filename", true);
  cli_parser.add_option("-y", args.launch_type,
                        "Launch type(production, development)", true);
  CLI11_PARSE(cli_parser, argc, argv);

  auto const software_config = binance::parse_config_file(
      args.database_config_filename, args.launch_type);
  if (!software_config) {
    std::cerr << "Unable to get database configuration values\n";
    return EXIT_FAILURE;
  }

  auto &database_connector =
      binance::database_connector_t::s_get_db_connector();
  database_connector->set_username(software_config->db_username);
  database_connector->set_password(software_config->db_password);
  database_connector->set_database_name(software_config->db_dns);

  if (!database_connector->connect()) {
    return EXIT_FAILURE;
  }
  BEARER_TOKEN_SECRET_KEY = software_config->jwt_secret_key;

  auto const thread_count = std::thread::hardware_concurrency();
  net::io_context io_context{static_cast<int>(thread_count)};
  auto server_instance = std::make_shared<binance::server_t>(io_context, args);
  if (!(*server_instance)) {
    return EXIT_FAILURE;
  }
  server_instance->run();

  boost::asio::ssl::context ssl_context(
      boost::asio::ssl::context::tlsv12_client);
  ssl_context.set_default_verify_paths();
  ssl_context.set_verify_mode(boost::asio::ssl::verify_none);

  std::vector<std::unique_ptr<binance::market_data_stream_t>> websocks{};

  {
    std::thread price_monitorer{binance::background_price_saver};
    price_monitorer.detach();

    std::thread task_monitorer{
        [&] { binance::task_scheduler_watcher(io_context); }};
    task_monitorer.detach();

    // launch in main thread.
    binance::launch_price_watcher(websocks, io_context, ssl_context);
  }

  auto const reserved_thread_count =
      thread_count > 2 ? thread_count - 2 : thread_count;
  std::vector<std::thread> threads{};
  threads.reserve(reserved_thread_count);
  for (std::size_t counter = 0; counter < reserved_thread_count; ++counter) {
    threads.emplace_back([&] { io_context.run(); });
  }
  io_context.run();
  return EXIT_SUCCESS;
}
