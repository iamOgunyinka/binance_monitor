#pragma once

#include "host_info.hpp"
#include "orders_info.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#define OTL_BIG_INT long long
#define OTL_ODBC_MYSQL
#define OTL_STL
#define OTL_STREAM_WITH_STD_TUPLE_ON
#ifdef _WIN32
#define OTL_ODBC_WINDOWS
#else
#define OTL_ODBC_UNIX
#endif
#define OTL_SAFE_EXCEPTION_ON
#include <otlv4.h>

namespace binance {

void log_sql_error(otl_exception const &exception);

struct db_config_t {
  std::string db_username{};
  std::string db_password{};
  std::string db_dns{};
  std::string bot_token{};

  operator bool() {
    return !(db_username.empty() && db_password.empty() && db_dns.empty() &&
             bot_token.empty());
  }
};

class database_connector_t {
  db_config_t db_config_;
  otl_connect otl_connector_;
  std::mutex db_mutex_;
  bool is_running_ = false;

private:
  void keep_sql_server_busy();

public:
  static std::unique_ptr<database_connector_t> &s_get_db_connector();
  bool connect();
  void set_username(std::string const &username);
  void set_password(std::string const &password);
  void set_database_name(std::string const &db_name);

  [[nodiscard]] std::vector<host_info_t> get_available_hosts();
  [[nodiscard]] bool add_new_host(host_info_t const &);

  void create_order_table(std::string const &tablename);
  void create_balance_table(std::string const &tablename);
  void add_new_order(std::string const &tablename,
                     ws_order_info_t const &order);
  void add_new_balance(std::string const &table_name,
                       ws_balance_info_t const &balance);
};

[[nodiscard]] std::optional<db_config_t>
parse_config_file(std::string const &filename, std::string const &config_name);

} // namespace binance
