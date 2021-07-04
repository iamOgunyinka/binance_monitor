#include "database_connector.hpp"
#include "json_utils.hpp"
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
        db_config.software_client_version =
            static_cast<int>(file_content_object->at("client_version")
                                 .get<json::number_integer_t>());
        db_config.jwt_secret_key =
            file_content_object->at("jwt").get<json::string_t>();

        // then the optional ones
        if (auto const server_version_iter =
                file_content_object->find("server_version");
            server_version_iter != file_content_object->cend()) {
          db_config.software_server_version = static_cast<int>(
              server_version_iter->second.get<json::number_integer_t>());
        }
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

std::string string_or_null(std::string const &date_str) {
  if (date_str.empty()) {
    return "NULL";
  }
  return "'" + date_str + "'";
}

std::optional<login_token_info_t>
database_connector_t::get_login_token(std::string const &username,
                                      std::string const &password_hash) {
  std::string const sql_statement{
      "SELECT id, bearer_token, user_role FROM cb_user WHERE username='{}'"
      " AND password_hash='{}'"_format(username, password_hash)};

  try {
    std::lock_guard<std::mutex> lock_g{db_mutex_};
    otl_stream db_stream(2, sql_statement.c_str(), otl_connector_);
    login_token_info_t info{};
    if (!db_stream.eof()) {
      db_stream >> info.user_id >> info.bearer_token >> info.user_role;
    }
    return info;
  } catch (otl_exception const &e) {
    log_sql_error(e);
    return std::nullopt;
  }
}

bool database_connector_t::store_bearer_token(int const user_id,
                                              std::string const &bearer_token) {
  auto const sql_statement =
      "UPDATE cb_user SET bearer_token='{}' WHERE id='{}'"_format(bearer_token,
                                                                  user_id);
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

std::string database_connector_t::bearer_token_name(std::string const &token) {
  auto const sql_statement =
      "SELECT username FROM cb_user WHERE bearer_token='{}'"_format(token);
  std::lock_guard<std::mutex> lock_g{db_mutex_};
  std::string username{};
  try {
    otl_stream db_stream(1, sql_statement.c_str(), otl_connector_);
    for (auto &data_stream : db_stream) {
      data_stream >> username;
    }
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
  return username;
}

std::vector<std::pair<std::string, std::string>>
database_connector_t::get_all_bearer_tokens() {
  auto const sql_statement =
      "SELECT bearer_token, username FROM cb_user LIMIT 100";
  std::vector<std::pair<std::string, std::string>> bearer_list{};
  std::string bearer_token{};
  std::string username{};
  std::lock_guard<std::mutex> lock_g{db_mutex_};
  try {
    otl_stream db_stream(100, sql_statement, otl_connector_);
    while (db_stream >> bearer_token >> username) {
      bearer_list.push_back({bearer_token, username});
    }
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
  return bearer_list;
}

bool database_connector_t::username_exists(std::string const &username) {
  if (usernames_.find(username) != usernames_.end()) {
    return true;
  }
  auto const sql_statement =
      "SELECT id FROM cb_user WHERE username='{}'"_format(username);
  try {
    std::lock_guard<std::mutex> lock_g{db_mutex_};
    otl_stream db_stream(1, sql_statement.c_str(), otl_connector_);
    int user_id{};
    if (!db_stream.eof()) {
      db_stream >> user_id;
    }
    bool const exists = user_id != 0;
    if (exists) {
      usernames_.insert(username);
    }
    return exists;
  } catch (otl_exception const &e) {
    log_sql_error(e);
    return false;
  }
}

bool database_connector_t::create_pnl_table(std::string const &table_name) {
  auto const sql_statement = R"(CREATE TABLE IF NOT EXISTS `{}` (
	`id` INT NOT NULL AUTO_INCREMENT,
	`token_name` VARCHAR(50) NULL DEFAULT NULL,
	`side` VARCHAR(10) NULL DEFAULT NULL,
	`time` DATETIME NULL DEFAULT NULL,
	`profit` DOUBLE NOT NULL DEFAULT '0',
	`mkt_price` DOUBLE NOT NULL DEFAULT '0',
	`money` DOUBLE NOT NULL DEFAULT '0',
	`ordered_price` DOUBLE NOT NULL DEFAULT '0',
	`quantity` DOUBLE NOT NULL DEFAULT '0',
	`col_id` INT NOT NULL DEFAULT '0',
	`task_type` INT NOT NULL DEFAULT '0',
	`request_id` VARCHAR(10) NULL DEFAULT NULL,
	PRIMARY KEY (`id`))COLLATE='utf8mb4_unicode_ci')"_format(table_name);
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

bool database_connector_t::create_task_table() {
  auto const sql_statement = R"(CREATE TABLE IF NOT EXISTS `scheduled_tasks`
	(`id` INT NOT NULL AUTO_INCREMENT,
	`for_username` VARCHAR(50) NULL,
	`token_name` VARCHAR(50) NULL,
	`request_id` VARCHAR(50) NULL,
	`side` VARCHAR(50) NULL,
	`monitor_time_secs` INT NOT NULL DEFAULT '0',
	`col_id` INT NOT NULL DEFAULT '0',
	`status` INT NOT NULL DEFAULT '0',
	`task_type` INT NOT NULL DEFAULT '0',
	`order_price` DECIMAL(20,6) NOT NULL DEFAULT 0,
	`quantity` DECIMAL(20,6) NOT NULL DEFAULT 0,
    `created_time` DATETIME NULL DEFAULT NULL,
    `last_begin_time` DATETIME NULL DEFAULT NULL,
    `last_end_time` DATETIME NULL DEFAULT NULL,
	PRIMARY KEY (`id`)) COLLATE='utf8mb4_unicode_ci')";
  std::lock_guard<std::mutex> lock_g{db_mutex_};
  try {
    otl_cursor::direct_exec(otl_connector_, sql_statement,
                            otl_exception::enabled);
  } catch (otl_exception const &e) {
    log_sql_error(e);
    return false;
  }
  return true;
}

std::string intlist_to_string(std::vector<task_state_e> const &vec) {
  std::ostringstream ss{};
  if (vec.empty())
    return {};
  for (std::size_t i = 0; i != vec.size() - 1; ++i) {
    ss << static_cast<int>(vec[i]) << ", ";
  }
  ss << static_cast<int>(vec.back());
  return ss.str();
}

std::vector<scheduled_task_t::task_result_t>
database_connector_t::get_task_result(std::string const &table_name,
                                      std::string const &request_id,
                                      std::string const &begin_time,
                                      std::string const &end_time) {
  std::string sql_statement =
      "SELECT token_name, side, time, profit, mkt_price, ordered_price, "
      "money, quantity, col_id, task_type FROM {} WHERE request_id="
      "'{}'"_format(table_name, request_id);

  if (!(begin_time.empty() || end_time.empty())) {
    sql_statement +=
        " AND `time` BETWEEN '{}' AND '{}'"_format(begin_time, end_time);
  } else if (begin_time.empty() && !end_time.empty()) {
    sql_statement += " AND `time` <= '{}'"_format(end_time);
  } else if (end_time.empty() && !begin_time.empty()) {
    sql_statement += " AND `time` >= '{}'"_format(begin_time);
  }
  sql_statement += " ORDER BY `time` ASC";

  std::vector<scheduled_task_t::task_result_t> result{};
  try {
    otl_stream db_stream{10'000, sql_statement.c_str(), otl_connector_};
    otl_datetime dt{};
    std::string direction{};
    int column_id{}, task_type{};

    for (auto &data_stream : db_stream) {
      scheduled_task_t::task_result_t item{};
      data_stream >> item.token_name;
      data_stream >> direction;
      data_stream >> dt;
      data_stream >> item.pnl;
      data_stream >> item.mkt_price;
      data_stream >> item.order_price;
      data_stream >> item.money;
      data_stream >> item.quantity;
      data_stream >> column_id;
      data_stream >> task_type;

      otl_datetime_to_string(item.current_time, dt);
      item.direction = string_to_direction(direction);
      item.column_id = column_id;
      item.task_type = static_cast<task_type_e>(task_type);

      result.push_back(std::move(item));
    }
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
  return result;
}

std::vector<scheduled_task_t> database_connector_t::get_scheduled_tasks(
    std::vector<task_state_e> const &statuses, std::string const &request_id) {
  std::vector<scheduled_task_t> tasks{};
  auto const sql_statement =
      "SELECT for_username, token_name, request_id, side, monitor_time_secs, "
      "status, order_price, money, quantity, col_id, `task_type` FROM "
      "scheduled_tasks WHERE {} status IN ({})"_format(
          (request_id.empty() ? std::string{}
                              : " request_id='{}' AND "_format(request_id)),
          intlist_to_string(statuses));

  auto const current_time = std::time(nullptr);
  try {
    otl_stream db_stream{1'000, sql_statement.c_str(), otl_connector_};
    int monitor_time_secs{};
    int status{}, column_id{}, task_type{};

    for (auto &data_stream : db_stream) {
      scheduled_task_t task{};
      data_stream >> task.for_username;
      data_stream >> task.token_name;
      data_stream >> task.request_id;
      data_stream >> task.direction;

      data_stream.operator>>(monitor_time_secs);
      data_stream >> status;
      data_stream >> task.order_price;
      data_stream >> task.money;
      data_stream >> task.quantity;
      data_stream >> column_id;
      data_stream >> task_type;

      task.status = static_cast<task_state_e>(status);
      task.monitor_time_secs = monitor_time_secs;
      task.current_time = current_time;
      task.column_id = column_id;
      task.task_type = static_cast<task_type_e>(task_type);

      tasks.push_back(std::move(task));
    }
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
  return tasks;
}

std::vector<user_task_t>
database_connector_t::get_users_tasks(std::vector<task_state_e> const &statuses,
                                      std::string const &username) {
  auto const sql_statement =
      "SELECT created_time, last_begin_time, last_end_time, token_name, "
      "request_id, side, monitor_time_secs, status, money, order_price, "
      "quantity, col_id, task_type FROM scheduled_tasks WHERE for_username"
      "='{}' AND status IN ({})"_format(username, intlist_to_string(statuses));

  std::vector<user_task_t> tasks{};
  try {
    otl_stream db_stream{1'000, sql_statement.c_str(), otl_connector_};
    otl_datetime created_time{}, last_end_time{}, last_begin_time{};
    int monitor_time_secs{}, status{}, column_id{};
    int task_type{};

    for (auto &data_stream : db_stream) {
      user_task_t task{};

      // dates
      data_stream >> created_time;
      data_stream >> last_begin_time;
      data_stream >> last_end_time;

      // strings
      data_stream >> task.token_name;
      data_stream >> task.request_id;
      data_stream >> task.direction;

      // ints and doubles
      data_stream.operator>>(monitor_time_secs);
      data_stream >> status;
      data_stream >> task.money;
      data_stream >> task.order_price;
      data_stream >> task.quantity;
      data_stream >> column_id;
      data_stream >> task_type;

      task.status = static_cast<task_state_e>(status);
      task.task_type = static_cast<task_type_e>(task_type);
      task.monitor_time_secs = monitor_time_secs;
      task.column_id = column_id;
      otl_datetime_to_string(task.created_time, created_time);
      otl_datetime_to_string(task.last_begin_time, last_begin_time);

      // only stopped tasks have this column
      if (last_end_time.year != 0) {
        otl_datetime_to_string(task.last_end_time, last_end_time);
      }

      tasks.push_back(std::move(task));
    }
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
  return tasks;
}

void database_connector_t::remove_task(std::string const &request_id) {
  auto const sql_statement =
      "DELETE FROM scheduled_tasks WHERE request_id='{}'"_format(request_id);
  try {
    otl_cursor::direct_exec(otl_connector_, sql_statement.c_str(),
                            otl_exception::enabled);
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
}

void database_connector_t::change_task_status(
    task_state_e const new_status, std::string const &request_id,
    std::string const &last_begin_time, std::string const &last_end_time) {
  std::string sql_statement{};
  if (last_begin_time.empty() && last_end_time.empty()) {
    sql_statement = "UPDATE scheduled_tasks SET status={} WHERE request_id='{}'"
                    ""_format(new_status, request_id);
  } else if (last_begin_time.empty()) {
    sql_statement =
        "UPDATE scheduled_tasks SET status={}, `last_end_time`='{}' "
        " WHERE request_id='{}'"_format(new_status, last_end_time, request_id);

  } else if (last_end_time.empty()) {
    sql_statement =
        "UPDATE scheduled_tasks SET status={}, `last_begin_time`='{}' "
        "WHERE request_id='{}'"_format(new_status, last_begin_time, request_id);
  } else {
    sql_statement =
        "UPDATE scheduled_tasks SET status={}, `last_begin_time`='{}',"
        "`last_end_time`='{}' WHERE request_id = '{}'"_format(
            new_status, last_begin_time, last_end_time, request_id);
  }
  try {
    otl_cursor::direct_exec(otl_connector_, sql_statement.c_str(),
                            otl_exception::enabled);
  } catch (otl_exception const &e) {
    log_sql_error(e);
  }
}

bool database_connector_t::insert_new_task(scheduled_task_t const &task,
                                           std::string const &datetime) {
  auto const sql_statement =
      "INSERT INTO scheduled_tasks(for_username,token_name,request_id,side,"
      "monitor_time_secs, status, order_price, money, quantity, col_id, "
      "task_type, `created_time`, `last_begin_time`) VALUES('{}','{}','{}',"
      "'{}', '{}', '{}', '{}', '{}', '{}','{}', '{}', '{}', '{}')"_format(
          task.for_username, task.token_name, task.request_id, task.direction,
          task.monitor_time_secs, task.status, task.order_price, task.money,
          task.quantity, task.column_id, task.task_type, datetime, datetime);

  try {
    otl_cursor::direct_exec(otl_connector_, sql_statement.c_str(),
                            otl_exception::enabled);
  } catch (otl_exception const &e) {
    log_sql_error(e);
    return false;
  }
  return true;
}

bool database_connector_t::insert_pnl_record(
    std::string const &table_name,
    scheduled_task_t::task_result_t const &record) {
  auto const sql_statement =
      "INSERT INTO `{}`(`token_name`, `time`, `profit`, `side`, `request_id`,"
      "`mkt_price`, `ordered_price`, `money`, `quantity`, `col_id`, task_type)"
      " VALUES ('{}', '{}', '{}', '{}', '{}',  '{}', '{}', '{}', '{}', '{}',"
      "'{}')"_format(table_name, record.token_name, record.current_time,
                     record.pnl, record.direction, record.request_id,
                     record.mkt_price, record.order_price, record.money,
                     record.quantity, record.column_id, record.task_type);

  try {
    otl_cursor::direct_exec(otl_connector_, sql_statement.c_str(),
                            otl_exception::enabled);
  } catch (otl_exception const &e) {
    log_sql_error(e);
    return false;
  }
  return true;
}

bool database_connector_t::add_new_user(std::string const &username,
                                        std::string const &password_hash,
                                        int const validity) {
  auto const sql_statement =
      "INSERT INTO cb_user(username, password_hash, validity)"
      "VALUES('{}', '{}', {})"_format(username, password_hash, validity);
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

} // namespace binance
