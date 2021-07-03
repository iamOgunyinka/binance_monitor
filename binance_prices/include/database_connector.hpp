#pragma once

#include "host_info.hpp"
#include "orders_info.hpp"
#include "subscription_data.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
  std::string jwt_secret_key{};
  int software_client_version{};
  int software_server_version{};

  operator bool() {
    return !(db_username.empty() && db_password.empty() && db_dns.empty() &&
             software_client_version == 0);
  }
};

struct api_key_data_t {
  std::string key{};
  std::string alias_for_account{};
};

struct login_token_info_t {
  int user_id{};
  int user_role{-1};
  std::string bearer_token{};
};

class database_connector_t {
  std::set<std::string> usernames_{};
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

  [[nodiscard]] std::optional<login_token_info_t>
  get_login_token(std::string const &username,
                  std::string const &password_hash);
  [[nodiscard]] std::string bearer_token_name(std::string const &token);
  [[nodiscard]] std::vector<std::pair<std::string, std::string>>
  get_all_bearer_tokens();
  [[nodiscard]] bool store_bearer_token(int const user_id,
                                        std::string const &bearer_token);
  [[nodiscard]] bool username_exists(std::string const &username);
  [[nodiscard]] bool add_new_user(std::string const &username,
                                  std::string const &password,
                                  int const validity);
  [[nodiscard]] bool create_pnl_table(std::string const &table_name);
  [[nodiscard]] bool insert_pnl_record(std::string const &table_name,
                                       scheduled_task_t::task_result_t const &);
  [[nodiscard]] bool create_task_table();
  [[nodiscard]] bool insert_new_task(scheduled_task_t const &,
                                     std::string const &datetime);
  void change_task_status(task_state_e const new_status,
                          std::string const &request_id,
                          std::string const &last_begin_time,
                          std::string const &last_end_time);
  void remove_task(std::string const &request_id);
  [[nodiscard]] std::vector<scheduled_task_t>
  get_scheduled_tasks(std::vector<task_state_e> const &statuses,
                      std::string const &request_id = {});
  [[nodiscard]] std::vector<user_task_t>
  get_users_tasks(std::vector<task_state_e> const &statuses,
                  std::string const &username);

  [[nodiscard]] std::vector<scheduled_task_t::task_result_t>
  get_task_result(std::string const &table_name, std::string const &request_id,
                  std::string const &begin_time, std::string const &end_time);
};

[[nodiscard]] std::optional<db_config_t>
parse_config_file(std::string const &filename, std::string const &config_name);

} // namespace okex
