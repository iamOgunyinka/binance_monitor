#include "database_connector.hpp"
#include "common/json_utils.hpp"
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>

namespace binance {

using namespace fmt::v7::literals;

void log_sql_error(otl_exception const &exception) {
  spdlog::error("SQLError code: {}", exception.code);
  spdlog::error("SQLError stmt: {}", exception.stm_text);
  spdlog::error("SQLError state: {}", exception.sqlstate);
  spdlog::error("SQLError msg: {}", exception.msg);
}

void otl_datetime_to_string(std::string &result, otl_datetime const &date) {
  result = "{}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}"_format(
      date.year, date.month, date.day, date.hour, date.minute, date.second);
}

std::optional<db_config_t> parse_config_file(std::string const &filename,
                                             std::string const &config_name) {
  auto const file_content_object = read_object_json_file(filename);
  if (!file_content_object) {
    return std::nullopt;
  }
  auto const database_list_iter = file_content_object->find("database");
  if (database_list_iter == file_content_object->cend()) {
    return std::nullopt;
  }
  try {
    auto const &database_list = database_list_iter->second.get<json::array_t>();

    for (auto const &config_data : database_list) {
      auto const temp_object = config_data.get<json::object_t>();
      auto const temp_object_iter = temp_object.find("type");
      if (temp_object_iter != temp_object.cend() &&
          temp_object_iter->second == config_name) {
        auto const db_data = temp_object.at("data").get<json::object_t>();
        db_config_t db_config{};

        // let's get out the compulsory field first
        db_config.db_username = db_data.at("username").get<json::string_t>();
        db_config.db_password = db_data.at("password").get<json::string_t>();
        db_config.db_dns = db_data.at("db_dns").get<json::string_t>();
        return db_config;
      }
    }
  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }
  return std::nullopt;
}

std::unique_ptr<database_connector_t> &
database_connector_t::s_get_db_connector() {
  static std::unique_ptr<database_connector_t> db_connector{};
  if (!db_connector) {
    otl_connect::otl_initialize(1);
    db_connector = std::make_unique<database_connector_t>();
  }
  return db_connector;
}

void database_connector_t::set_username(std::string const &username) {
  db_config_.db_username = username;
}

void database_connector_t::set_password(std::string const &password) {
  db_config_.db_password = password;
}

void database_connector_t::set_database_name(std::string const &db_name) {
  db_config_.db_dns = db_name;
}

void database_connector_t::keep_sql_server_busy() {
  spdlog::info("keeping DB server busy");
  std::thread sql_thread{[this] {
    while (true) {
      try {
        otl_cursor::direct_exec(otl_connector_, "select 1", true);
      } catch (otl_exception const &exception) {
        log_sql_error(exception);
        otl_connector_.logoff();
        otl_connector_.rlogon("{}/{}@{}"_format(db_config_.db_username,
                                                db_config_.db_password,
                                                db_config_.db_dns)
                                  .c_str());
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
      std::this_thread::sleep_for(std::chrono::minutes(15));
    }
  }};
  sql_thread.detach();
}

bool database_connector_t::connect() {
  if (!db_config_) {
    throw std::runtime_error{"configuration incomplete"};
  }
  if (is_running_) {
    return is_running_;
  }

  std::string const login_str{"{}/{}@{}"_format(
      db_config_.db_username, db_config_.db_password, db_config_.db_dns)};
  try {
    this->otl_connector_.rlogon(login_str.c_str());
    keep_sql_server_busy();
    is_running_ = true;
    return is_running_;
  } catch (otl_exception const &exception) {
    log_sql_error(exception);
    return is_running_;
  }
}

std::vector<host_info_t> database_connector_t::get_available_hosts() {
  auto const sql_statement =
      "SELECT alias, api_key, secret_key, tg_group FROM hosts";
  std::vector<host_info_t> hosts{};
  std::lock_guard<std::mutex> lock_g{db_mutex_};

  try {
    otl_stream db_stream{10'000, sql_statement, otl_connector_};
    for (auto &item_stream : db_stream) {
      host_info_t host_info{};
      item_stream >> host_info.account_alias;
      item_stream >> host_info.api_key;
      item_stream >> host_info.secret_key;
      item_stream >> host_info.tg_group_name;

      hosts.push_back(std::move(host_info));
    }
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
  return hosts;
}

bool database_connector_t::add_new_host(host_info_t const &host_info) {
  auto const sql_statement =
      "INSERT INTO hosts(api_key, secret_key, alias, tg_group) VALUES"
      "('{}', '{}','{}', '{}')"_format(host_info.api_key, host_info.secret_key,
                                       host_info.account_alias,
                                       host_info.tg_group_name);
  std::lock_guard<std::mutex> lock_g{db_mutex_};
  try {
    otl_cursor::direct_exec(otl_connector_, sql_statement.c_str(),
                            otl_exception::enabled);
  } catch (otl_exception const &e) {
    log_sql_error(e);
    return false;
  }
  return true;
}

void database_connector_t::create_balance_table(std::string const &table_name) {
  auto const sql_statement = R"(CREATE TABLE if not exists `{}` (
	`id` INT(10) NOT NULL AUTO_INCREMENT,
	`instrument_id` VARCHAR(50) NULL DEFAULT NULL COLLATE 'utf8mb4_unicode_ci',
	`balance` VARCHAR(50) NULL DEFAULT NULL COLLATE 'utf8mb4_unicode_ci',
    `event_time` DATETIME NULL DEFAULT NULL,
	`clear_time` DATETIME NULL DEFAULT NULL,
    PRIMARY KEY (`id`) USING BTREE) COLLATE='utf8mb4_unicode_ci' ENGINE=InnoDB)"_format(
      table_name);

  std::lock_guard<std::mutex> lock_g{db_mutex_};
  try {
    otl_cursor::direct_exec(otl_connector_, sql_statement.c_str(),
                            otl_exception::enabled);
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
}

void database_connector_t::create_order_table(std::string const &tablename) {
  auto const sql_statement = R"(CREATE TABLE if not exists `{}` (
	`id` INT(10) NOT NULL AUTO_INCREMENT,
	`instrument_id` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`order_side` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`order_type` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`time_in_force` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`quantity_purchased` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`order_price` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`stop_price` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`execution_type` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`order_status` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`reject_reason` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`order_id` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`last_filled_quantity` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`cummulative_filled_quantity` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`last_executed_price` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`commission_amount` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`commission_asset` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`trade_id` VARCHAR(50) NULL DEFAULT '' COLLATE 'utf8mb4_unicode_ci',
	`transaction_time` DATETIME NULL DEFAULT NULL,
	`event_time` DATETIME NULL DEFAULT NULL,
	`created_time` DATETIME NULL DEFAULT NULL,
    PRIMARY KEY (`id`) USING BTREE) COLLATE='utf8mb4_unicode_ci' ENGINE=InnoDB)"_format(
      tablename);
  std::lock_guard<std::mutex> lock_g{db_mutex_};

  try {
    otl_cursor::direct_exec(otl_connector_, sql_statement.c_str(),
                            otl_exception::enabled);
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
}

std::string string_or_null(std::string const &str) {
  if (str.empty()) {
    return "NULL";
  }
  return "'" + str + "'";
}

void database_connector_t::add_new_balance(std::string const &table_name,
                                           ws_balance_info_t const &balance) {
  auto const sql_statement =
      "INSERT INTO {} (instrument_id, balance, event_time, clear_time)"
      "VALUES ('{}', '{}', {}, {})"_format(table_name, balance.instrument_id,
                                           balance.balance,
                                           string_or_null(balance.event_time),
                                           string_or_null(balance.clear_time));
  std::lock_guard<std::mutex> lock_g{db_mutex_};
  try {
    otl_cursor::direct_exec(otl_connector_, sql_statement.c_str(),
                            otl_exception::enabled);
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
}

void database_connector_t::add_new_order(std::string const &tablename,
                                         ws_order_info_t const &order) {
  auto const sql_statement =
      "INSERT INTO {} (instrument_id, order_side, order_type,"
      "time_in_force, quantity_purchased, order_price, stop_price, "
      "execution_type, order_status, reject_reason, order_id, "
      "last_filled_quantity, cummulative_filled_quantity, last_executed_price,"
      "commission_amount, commission_asset, trade_id, event_time, "
      "transaction_time, created_time) VALUES"
      "('{}', '{}', '{}', '{}', '{}', '{}', '{}', '{}', '{}', '{}', "
      "'{}', '{}', '{}', '{}', '{}', {}, '{}', {}, {}, {})"_format(
          tablename, order.instrument_id, order.order_side, order.order_type,
          order.time_in_force, order.quantity_purchased, order.order_price,
          order.stop_price, order.execution_type, order.order_status,
          order.reject_reason, order.order_id, order.last_filled_quantity,
          order.cummulative_filled_quantity, order.last_executed_price,
          order.commission_amount, string_or_null(order.commission_asset),
          order.trade_id, string_or_null(order.event_time),
          string_or_null(order.transaction_time),
          string_or_null(order.created_time));
  std::lock_guard<std::mutex> lock_g{db_mutex_};

  try {
    otl_cursor::direct_exec(otl_connector_, sql_statement.c_str(),
                            otl_exception::enabled);
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
}

} // namespace binance
