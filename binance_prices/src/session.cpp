#include "session.hpp"
#include "crypto.hpp"
#include "database_connector.hpp"
#include "request_handler.hpp"
#include "subscription_data.hpp"
#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <jwt/jwt.hpp>
#include <spdlog/spdlog.h>
#include <zip_file.hpp>

extern std::string BEARER_TOKEN_SECRET_KEY;

namespace binance {

using namespace fmt::v7::literals;

enum constant_e { RequestBodySize = 1'024 * 1'024 * 50 };

std::string get_alphanum_tablename(std::string str) {
  static auto non_alphanum_remover = [](char const ch) {
    return !std::isalnum(ch);
  };
  str.erase(std::remove_if(str.begin(), str.end(), non_alphanum_remover),
            str.end());
  return str;
}

std::filesystem::path const download_path =
    std::filesystem::current_path() / "downloads" / "zip_files";

rule_t::rule_t(std::initializer_list<http::verb> &&verbs, callback_t callback)
    : num_verbs_{verbs.size()}, route_callback_{std::move(callback)} {
  if (verbs.size() > 3) {
    throw std::runtime_error{"maximum number of verbs is 5"};
  }
  for (int i = 0; i != verbs.size(); ++i) {
    verbs_[i] = *(verbs.begin() + i);
  }
}

void endpoint_t::add_endpoint(std::string const &route,
                              std::initializer_list<http::verb> verbs,
                              callback_t &&callback) {
  if (route.empty() || route[0] != '/') {
    throw std::runtime_error{"A valid route starts with a /"};
  }
  endpoints.emplace(route, rule_t{std::move(verbs), std::move(callback)});
}

std::optional<endpoint_t::rule_iterator>
endpoint_t::get_rules(std::string const &target) {
  auto iter = endpoints.find(target);
  if (iter == endpoints.end()) {
    return std::nullopt;
  }
  return iter;
}

std::string task_state_to_string(task_state_e const state) {
  switch (state) {
  case task_state_e::initiated:
    return "initiated";
  case task_state_e::remove:
    return "removed";
  case task_state_e::restarted:
    return "restarted";
  case task_state_e::running:
    return "running";
  case task_state_e::stopped:
    return "stopped";
  case task_state_e::unknown:
  default:
    return "unknown";
  }
}

std::optional<endpoint_t::rule_iterator>
endpoint_t::get_rules(boost::string_view const &target) {
  return get_rules(target.to_string());
}

void to_json(json &j, instrument_type_t const &instr) {
  j = json{{"inst_id", instr.instrument_id}};
}

void to_json(json &j, api_key_data_t const &data) {
  j = json{{"key", data.key}, {"alias", data.alias_for_account}};
}

void to_json(json &j, scheduled_task_t::task_result_t const &item) {
  j = json{{"token_name", item.token_name},
           {"mkt_price", item.mkt_price},
           {"order_price", item.order_price},
           {"qty", item.quantity},
           {"pnl", item.pnl},
           {"task_type", item.task_type},
           {"col_id", item.column_id}};
}

void to_json(json &j, user_task_t const &item) {
  j = json{{"token_name", item.token_name},  {"side", item.direction},
           {"time", item.monitor_time_secs}, {"money", item.money},
           {"price", item.order_price},      {"qty", item.quantity},
           {"task_type", item.task_type},    {"col_id", item.column_id}};
}

std::unordered_map<std::string, std::string> session_t::bearer_token_set_{};

session_t::session_t(net::io_context &io, net::ip::tcp::socket &&socket)
    : io_context_{io}, tcp_stream_{std::move(socket)} {
  add_endpoint_interfaces();
}

void session_t::add_endpoint_interfaces() {
  using http::verb;

  endpoint_apis_.add_endpoint(
      "/", {verb::get},
      [=](string_request_t const &request, url_query_t const &optional_query) {
        index_page_handler(request, optional_query);
      });

  endpoint_apis_.add_endpoint("/price", {verb::post},
                              [=](auto const &req, auto const &query) {
                                return get_price_handler(req, query);
                              });

  endpoint_apis_.add_endpoint("/login", {verb::post},
                              [=](auto const &req, auto const &query) {
                                return user_login_handler(req, query);
                              });

  endpoint_apis_.add_endpoint("/get_file", {verb::get},
                              [this](auto const &req, auto const &query) {
                                get_file_handler(req, query);
                              });

  endpoint_apis_.add_endpoint("/create_user", {verb::post},
                              [this](auto const &req, auto const &query) {
                                return create_user_handler(req, query);
                              });

  endpoint_apis_.add_endpoint("/trading_pairs", {verb::get},
                              [this](auto const &req, auto const &query) {
                                get_trading_pairs_handler(req, query);
                              });

  endpoint_apis_.add_endpoint("/my_tasks", {verb::get},
                              [this](auto const &req, auto const &query) {
                                get_user_jobs_handler(req, query);
                              });

  endpoint_apis_.add_endpoint("/task", {verb::post},
                              [this](auto const &request, auto const &query) {
                                scheduled_job_handler(request, query);
                              });
}

void session_t::run() { http_read_data(); }

void session_t::http_read_data() {
  buffer_.clear();
  empty_body_parser_.emplace();
  empty_body_parser_->body_limit(RequestBodySize);
  beast::get_lowest_layer(tcp_stream_).expires_after(std::chrono::minutes(5));
  http::async_read_header(tcp_stream_, buffer_, *empty_body_parser_,
                          beast::bind_front_handler(&session_t::on_header_read,
                                                    shared_from_this()));
}

void session_t::on_header_read(beast::error_code ec, std::size_t const) {
  if (ec == http::error::end_of_stream) {
    return shutdown_socket();
  }
  if (ec) {
    return error_handler(
        server_error(ec.message(), error_type_e::ServerError, {}), true);
  } else {
    content_type_ = empty_body_parser_->get()[http::field::content_type];
    client_request_ = std::make_unique<http::request_parser<http::string_body>>(
        std::move(*empty_body_parser_));
    http::async_read(tcp_stream_, buffer_, *client_request_,
                     beast::bind_front_handler(&session_t::on_data_read,
                                               shared_from_this()));
  }
}

void session_t::handle_requests(string_request_t const &request) {
  std::string const request_target{utilities::decode_url(request.target())};

  if (request_target.empty()) {
    return index_page_handler(request, {});
  }

  auto const method = request.method();
  boost::string_view request_target_view = request_target;
  auto split = utilities::split_string_view(request_target_view, "?");
  if (auto iter = endpoint_apis_.get_rules(split[0]); iter.has_value()) {
    auto const iter_end =
        iter.value()->second.verbs_.cbegin() + iter.value()->second.num_verbs_;
    auto const found_iter =
        std::find(iter.value()->second.verbs_.cbegin(), iter_end, method);
    if (found_iter == iter_end) {
      return error_handler(method_not_allowed(request));
    }
    boost::string_view const query_string = split.size() > 1 ? split[1] : "";
    auto url_query_{split_optional_queries(query_string)};
    return iter.value()->second.route_callback_(request, url_query_);
  } else {
    return error_handler(not_found(request));
  }
}

void session_t::on_data_read(beast::error_code const ec, std::size_t const) {
  if (ec == http::error::end_of_stream) { // end of connection
    return shutdown_socket();
  } else if (ec == http::error::body_limit) {
    return error_handler(server_error(ec.message(), error_type_e::ServerError,
                                      string_request_t{}),
                         true);
  } else if (ec) {
    return error_handler(server_error(ec.message(), error_type_e::ServerError,
                                      string_request_t{}),
                         true);
  } else {
    handle_requests(client_request_->get());
  }
}

bool session_t::is_closed() {
  return !beast::get_lowest_layer(tcp_stream_).socket().is_open();
}

void session_t::shutdown_socket() {
  beast::error_code ec{};
  beast::get_lowest_layer(tcp_stream_)
      .socket()
      .shutdown(net::socket_base::shutdown_send, ec);
  ec = {};
  beast::get_lowest_layer(tcp_stream_).socket().close(ec);
  beast::get_lowest_layer(tcp_stream_).close();
}

void session_t::error_handler(string_response_t &&response, bool close_socket) {
  auto resp = std::make_shared<string_response_t>(std::move(response));
  resp_ = resp;
  if (!close_socket) {
    http::async_write(tcp_stream_, *resp,
                      beast::bind_front_handler(&session_t::on_data_written,
                                                shared_from_this()));
  } else {
    http::async_write(
        tcp_stream_, *resp,
        [self = shared_from_this()](auto const err_c, std::size_t const) {
          self->shutdown_socket();
        });
  }
}

void session_t::on_data_written(beast::error_code ec,
                                std::size_t const bytes_written) {
  if (ec) {
    return spdlog::error(ec.message());
  }
  resp_ = nullptr;
  http_read_data();
}

void session_t::index_page_handler(string_request_t const &request,
                                   url_query_t const &) {
  return error_handler(
      get_error(":P", error_type_e::NoError, http::status::ok, request));
}

void session_t::get_file_handler(string_request_t const &request,
                                 url_query_t const &optional_query) {
  if (!is_validated_user(request)) {
    return error_handler(permission_denied(request));
  }

  if (content_type_ != "application/json") {
    return error_handler(bad_request("invalid content-type", request));
  }
  auto const id_iter = optional_query.find("id");
  if (id_iter == optional_query.cend()) {
    return error_handler(bad_request("key parameter missing", request));
  }
  std::string file_path{};
  try {
    file_path = utilities::base64_decode(id_iter->second.to_string());
  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }
  if (file_path.empty()) {
    return error_handler(not_found(request));
  }
  char const *const content_type =
      "application/zip, application/octet-stream, "
      "application/x-zip-compressed, multipart/x-zip";
  auto callback = [file_path] {
    std::error_code temp_ec{};
    std::filesystem::remove(file_path, temp_ec);
  };
  return send_file(file_path, content_type, request, callback);
}

void session_t::get_trading_pairs_handler(string_request_t const &request,
                                          url_query_t const &optional_query) {
  if (!is_validated_user(request)) {
    return error_handler(permission_denied(request));
  }

  auto &listed_instruments = request_handler_t::get_all_listed_instruments();
  auto filter = [](auto const &) { return true; };
  return send_response(
      json_success(listed_instruments.all_items_matching(filter), request));
}

void session_t::scheduled_job_handler(string_request_t const &request,
                                      url_query_t const &optional_query) {
  if (!is_validated_user(request)) {
    return error_handler(permission_denied(request));
  }
  if (!is_json_request()) {
    return error_handler(bad_request("invalid content-type", request));
  }
  auto const action_iter = optional_query.find("action");
  if (action_iter == optional_query.end()) {
    return error_handler(bad_request("query `action` missing", request));
  }

  auto const action = boost::to_lower_copy(action_iter->second.to_string());

  if (action == "add" || action == "new") {
    return add_new_jobs(request);
  } else if (action == "remove" || action == "delete") {
    return stop_scheduled_jobs(request, task_state_e::remove);
  } else if (action == "stop") {
    return stop_scheduled_jobs(request, task_state_e::stopped);
  } else if (action == "restart") {
    return restart_scheduled_jobs(request);
  } else if (action == "result") {
    return get_tasks_result(request);
  }

  return error_handler(bad_request("unknown `action` specified", request));
}

void session_t::add_new_jobs(string_request_t const &request) {
  std::size_t const current_time = std::time(nullptr);
  auto &scheduled_job_list = request_handler_t::get_all_scheduled_tasks();

  try {

    auto const json_root = json::parse(request.body()).get<json::object_t>();
    std::string global_request_id{};
    if (auto const request_id_iter = json_root.find("id");
        json_root.end() != request_id_iter) {
      global_request_id = request_id_iter->second.get<json::string_t>();
    }

    auto const current_time = std::time(nullptr);
    auto const json_job_list = json_root.at("contracts").get<json::array_t>();
    for (auto const &json_item : json_job_list) {
      auto const json_object = json_item.get<json::object_t>();
      scheduled_task_t new_task{};
      new_task.token_name = boost::to_upper_copy(
          json_object.at("token_name").get<json::string_t>());
      new_task.column_id =
          json_object.at("col_id").get<json::number_unsigned_t>();
      new_task.direction =
          boost::to_lower_copy(json_object.at("side").get<json::string_t>());
      new_task.monitor_time_secs = static_cast<int>(
          json_object.at("time").get<json::number_integer_t>());
      new_task.order_price =
          json_object.at("price").get<json::number_float_t>();
      new_task.quantity = json_object.at("qty").get<json::number_float_t>();
      new_task.money = json_object.at("money").get<json::number_float_t>();

      auto const task_type =
          json_object.at("task_type").get<json::number_integer_t>();
      if (task_type > 1) { // task_type == 1, price changes only
        return error_handler(bad_request("unknown `task_type` found", request));
      }

      new_task.for_username = current_username_;
      new_task.current_time = current_time;
      new_task.status = task_state_e::initiated;
      new_task.task_type = static_cast<task_type_e>(task_type);

      if (auto request_id_iter = json_object.find("id");
          request_id_iter != json_object.end()) {
        new_task.request_id = request_id_iter->second.get<json::string_t>();
      } else {
        if (global_request_id.empty()) {
          global_request_id = utilities::get_random_string(10);
        }
        new_task.request_id = global_request_id;
      }
      scheduled_job_list.append(std::move(new_task));
    }

    json::object_t result;
    result["status"] = static_cast<int>(error_type_e::NoError);
    result["message"] = "OK";
    if (!global_request_id.empty()) {
      result["id"] = global_request_id;
    }
    return send_response(json_success(result, request));

  } catch (std::exception const &e) {
    spdlog::error(e.what());
    return error_handler(bad_request("JSON object is invalid", request));
  }
}

void session_t::restart_scheduled_jobs(string_request_t const &request) {
  auto &scheduled_job_list = request_handler_t::get_all_scheduled_tasks();
  try {
    json::array_t const request_id_list =
        json::parse(request.body()).get<json::array_t>();
    for (auto const &json_item : request_id_list) {
      scheduled_task_t task{};
      task.for_username = current_username_;
      task.request_id = json_item.get<json::string_t>();
      task.status = task_state_e::restarted;
      scheduled_job_list.append(std::move(task));
    }
    json::object_t result;
    result["status"] = static_cast<int>(error_type_e::NoError);
    result["message"] = "OK";
    return send_response(json_success(result, request));
  } catch (std::exception const &e) {
    spdlog::error(e.what());
    return error_handler(bad_request("JSON object is invalid", request));
  }
}

void session_t::stop_scheduled_jobs(string_request_t const &request,
                                    task_state_e const status) {
  auto &scheduled_job_list = request_handler_t::get_all_scheduled_tasks();
  try {
    json::array_t const request_id_list =
        json::parse(request.body()).get<json::array_t>();
    for (auto const &json_item : request_id_list) {
      scheduled_task_t task{};
      task.for_username = current_username_;
      task.request_id = json_item.get<json::string_t>();
      task.status = status;
      scheduled_job_list.append(std::move(task));
    }
    json::object_t result;
    result["status"] = static_cast<int>(error_type_e::NoError);
    result["message"] = "OK";
    return send_response(json_success(result, request));

  } catch (std::exception const &e) {
    spdlog::error(e.what());
    return error_handler(bad_request("JSON object is invalid", request));
  }
}

void session_t::get_tasks_result(string_request_t const &request) {
  auto const records_table_name =
      get_alphanum_tablename(current_username_) + "_records";
  auto &database_connector = database_connector_t::s_get_db_connector();

  std::unordered_map<
      std::string,
      std::map<std::string, std::vector<scheduled_task_t::task_result_t>>>
      result_map{};

  try {

    auto const request_list = json::parse(request.body()).get<json::array_t>();
    for (auto const &json_item : request_list) {
      auto const item_object = json_item.get<json::object_t>();
      auto const request_id = item_object.at("id").get<json::string_t>();
      std::string begin_time{};
      std::string end_time{};
      if (auto const begin_time_iter = item_object.find("begin_time");
          begin_time_iter != item_object.end()) {
        begin_time = begin_time_iter->second.get<json::string_t>();
      }
      if (auto const end_time_iter = item_object.find("end_time");
          end_time_iter != item_object.end()) {
        end_time = end_time_iter->second.get<json::string_t>();
      }
      auto task_result = database_connector->get_task_result(
          records_table_name, request_id, begin_time, end_time);
      auto &request_data = result_map[request_id];
      for (auto &item : task_result) {
        request_data[item.current_time].push_back(std::move(item));
      }
    }
    return send_response(json_success(std::move(result_map), request));
  } catch (std::exception const &e) {
    spdlog::error(e.what());
    return error_handler(bad_request("JSON object is invalid", request));
  }
}

bool session_t::is_json_request() const {
  return boost::iequals(content_type_, "application/json");
}

void session_t::get_price_handler(string_request_t const &request,
                                  url_query_t const &) {
  if (!is_validated_user(request)) {
    return error_handler(permission_denied(request));
  }

  if (!is_json_request()) {
    return error_handler(bad_request("invalid content-type", request));
  }
  json::array_t result;
  auto const &tokens = request_handler_t::get_all_pushed_data();
  try {
    auto const object_root = json::parse(request.body()).get<json::object_t>();
    auto const contracts = object_root.at("contracts").get<json::array_t>();
    for (auto const &json_token : contracts) {
      auto const token_name =
          boost::to_upper_copy(json_token.get<json::string_t>());
      if (auto const find_iter = tokens.find(token_name);
          find_iter != tokens.cend()) {
        auto const &data = find_iter->second;
        auto const change =
            ((data.current_price - data.open_24h) / data.open_24h) * 100.0;
        
        json::object_t item;
        item["name"] = data.instrument_id;
        item["price"] = data.current_price;
        item["open_24h"] = data.open_24h;
        item["change"] = change;
        result.push_back(std::move(item));
      }
    }
    return send_response(json_success(result, request));
  } catch (std::exception const &e) {
    spdlog::error(e.what());
    return error_handler(bad_request("JSON object is invalid", request));
  }
}

std::optional<std::string>
session_t::extract_bearer_token(string_request_t const &request) {
  auto const authorization_str =
      request[http::field::authorization].to_string();
  if (authorization_str.empty()) {
    return std::nullopt;
  }
  auto const bearer_start_string = "Bearer ";
  auto const index = authorization_str.find(bearer_start_string);
  if (index != 0) {
    return std::nullopt;
  }
  auto const bearer_start_len = std::strlen(bearer_start_string);
  auto const token = authorization_str.substr(index + bearer_start_len);
  if (token.empty()) {
    return std::nullopt;
  }
  return token;
}

void session_t::create_user_handler(string_request_t const &request,
                                    url_query_t const &) {
  if (!is_json_request()) {
    return error_handler(bad_request("invalid content-type", request));
  }

  auto &db_connector = database_connector_t::s_get_db_connector();
  auto &body = request.body();
  try {
    auto const json_root = json::parse(body);
    auto const username = json_root.at("username").get<json::string_t>();
    auto const md5_password_hash =
        json_root.at("pwd_hash").get<json::string_t>();
    if (username.empty() || md5_password_hash.empty()) {
      return error_handler(
          bad_request("username/password hash cannot be empty", request));
    }
    if (db_connector->username_exists(username)) {
      return error_handler(bad_request("the username already exists", request));
    }
    int const is_active = 1;
    if (!db_connector->add_new_user(username, md5_password_hash, is_active)) {
      return error_handler(server_error("unable to add new user",
                                        error_type_e::ServerError, request));
    }
    return send_response(success("new user created", request));
  } catch (std::exception const &e) {
    spdlog::error(e.what());
    return error_handler(bad_request("invalid json request", request));
  }
}

bool session_t::is_validated_user(string_request_t const &request) {
  auto opt_token = extract_bearer_token(request);
  if (!opt_token.has_value()) {
    return false;
  }
  return is_validated_user(*opt_token);
}

bool session_t::is_validated_user(std::string const &token) {
  if (bearer_token_set_.empty()) {
    auto &database_connector = database_connector_t::s_get_db_connector();
    auto bearer_token_list = database_connector->get_all_bearer_tokens();
    // if the DB has no bearer tokens at all, then this bearer token is
    // invalid
    if (bearer_token_list.empty()) {
      current_username_.clear();
      return false;
    }
    for (auto const &[bearer_token, username] : bearer_token_list) {
      bearer_token_set_.insert({bearer_token, username});
    }
  }
  auto const iter = bearer_token_set_.find(token);
  if (iter == bearer_token_set_.cend()) {
    return check_database_for_token(token);
  }
  current_username_ = iter->second;
  return !current_username_.empty();
}

bool session_t::check_database_for_token(std::string const &token) {
  auto &database_connector = database_connector_t::s_get_db_connector();
  current_username_ = database_connector->bearer_token_name(token);
  bearer_token_set_.insert({token, current_username_});
  return !current_username_.empty();
}

void session_t::get_user_jobs_handler(string_request_t const &request,
                                      url_query_t const &) {
  if (!is_validated_user(request)) {
    return error_handler(permission_denied(request));
  }
  std::vector<task_state_e> const statuses{
      task_state_e::initiated, task_state_e::running, task_state_e::stopped};
  auto &database_connector = database_connector_t::s_get_db_connector();
  auto task_list =
      database_connector->get_users_tasks(statuses, current_username_);
  std::map<std::string, decltype(task_list)> task_map{};
  for (auto &task : task_list) {
    task_map[task.request_id].push_back(std::move(task));
  }

  json::array_t result_list;
  for (auto const &[task_id, contracts] : task_map) {
    if (contracts.empty()) {
      continue;
    }
    auto &first_contract = contracts[0];
    json::object_t item;
    item["task_id"] = task_id;
    item["status"] = task_state_to_string(first_contract.status);
    item["create_time"] = first_contract.created_time;
    item["last_begin_time"] = first_contract.last_begin_time;
    item["last_end_time"] = first_contract.last_end_time;
    item["contracts"] = contracts;
    result_list.push_back(std::move(item));
  }
  return send_response(json_success(std::move(result_list), request));
}

void session_t::user_login_handler(string_request_t const &request,
                                   url_query_t const &) {
  if (!is_json_request()) {
    return error_handler(bad_request("invalid content-type", request));
  }

  auto &body = request.body();
  try {
    json json_root = json::parse(std::string_view(body.data(), body.size()));
    json::object_t const login_object = json_root.get<json::object_t>();
    auto const username = login_object.at("username").get<json::string_t>();
    auto const password_hash =
        login_object.at("pwd_hash").get<json::string_t>();
    auto &database_connector = database_connector_t::s_get_db_connector();
    auto opt_bearer_token =
        database_connector->get_login_token(username, password_hash);
    if (!opt_bearer_token || opt_bearer_token->user_id == 0) {
      return error_handler(get_error("invalid username or password",
                                     error_type_e::Unauthorized,
                                     http::status::unauthorized, request));
    }
    if (opt_bearer_token->bearer_token.empty()) {
      auto const bearer_token = generate_bearer_token(
          username, opt_bearer_token->user_role, BEARER_TOKEN_SECRET_KEY);
      opt_bearer_token->bearer_token = bearer_token;
      if (database_connector->store_bearer_token(opt_bearer_token->user_id,
                                                 bearer_token)) {
        bearer_token_set_.insert({bearer_token, username});
      }
    }
    json::object_t result_obj;
    result_obj["status"] = error_type_e::NoError;
    result_obj["message"] = "success";
    result_obj["token"] = opt_bearer_token->bearer_token;

    return send_response(json_success(result_obj, request));
  } catch (std::exception const &e) {
    spdlog::error(e.what());
    return error_handler(bad_request("json object not valid", request));
  }
}

std::string session_t::generate_bearer_token(std::string const &username,
                                             int const user_role,
                                             std::string const &secret_key) {
  using jwt::params::algorithm;
  using jwt::params::algorithms;
  using jwt::params::payload;
  using jwt::params::secret;
  std::unordered_map<std::string, std::string> info_result;
  info_result["hash_used"] = "HS256";
  info_result["user_role"] = "{}"_format(user_role);
  info_result["username"] = username;

  jwt::jwt_object obj{algorithm("HS256"), payload(info_result),
                      secret(secret_key)};
  return obj.signature();
}

std::optional<json::object_t>
decode_bearer_token(std::string const &token, std::string const &secret_key) {
  using jwt::params::algorithms;
  using jwt::params::secret;

  try {
    auto dec_obj =
        jwt::decode(token, algorithms({"HS256"}), secret(secret_key));
    return dec_obj.payload().create_json_obj().get<json::object_t>();
  } catch (std::exception const &) {
    return std::nullopt;
  }
}

// =========================STATIC FUNCTIONS==============================

string_response_t session_t::not_found(string_request_t const &request) {
  return get_error("url not found", error_type_e::ResourceNotFound,
                   http::status::not_found, request);
}

string_response_t session_t::upgrade_required(string_request_t const &request) {
  return get_error("you need to upgrade your client software",
                   error_type_e::ResourceNotFound,
                   http::status::upgrade_required, request);
}

string_response_t session_t::server_error(std::string const &message,
                                          error_type_e type,
                                          string_request_t const &request) {
  return get_error(message, type, http::status::internal_server_error, request);
}

string_response_t session_t::bad_request(std::string const &message,
                                         string_request_t const &request) {
  return get_error(message, error_type_e::BadRequest, http::status::bad_request,
                   request);
}

string_response_t
session_t::permission_denied(string_request_t const &request) {
  return get_error("permission denied", error_type_e::Unauthorized,
                   http::status::unauthorized, request);
}

string_response_t session_t::method_not_allowed(string_request_t const &req) {
  return get_error("method not allowed", error_type_e::MethodNotAllowed,
                   http::status::method_not_allowed, req);
}

string_response_t session_t::get_error(std::string const &error_message,
                                       error_type_e type, http::status status,
                                       string_request_t const &req) {
  json::object_t result_obj;
  result_obj["status"] = type;
  result_obj["message"] = error_message;
  json result = result_obj;

  string_response_t response{status, req.version()};
  response.set(http::field::content_type, "application/json");
  response.keep_alive(req.keep_alive());
  response.body() = result.dump();
  response.prepare_payload();
  return response;
}

string_response_t session_t::json_success(json const &body,
                                          string_request_t const &req) {
  string_response_t response{http::status::ok, req.version()};
  response.set(http::field::content_type, "application/json");
  response.keep_alive(req.keep_alive());
  response.body() = body.dump();
  response.prepare_payload();
  return response;
}

string_response_t session_t::success(char const *message,
                                     string_request_t const &req) {
  json::object_t result_obj;
  result_obj["status"] = error_type_e::NoError;
  result_obj["message"] = message;
  json result(result_obj);

  string_response_t response{http::status::ok, req.version()};
  response.set(http::field::content_type, "application/json");
  response.keep_alive(req.keep_alive());
  response.body() = result.dump();
  response.prepare_payload();
  return response;
}

void session_t::send_response(string_response_t &&response) {
  auto resp = std::make_shared<string_response_t>(std::move(response));
  resp_ = resp;
  http::async_write(tcp_stream_, *resp,
                    beast::bind_front_handler(&session_t::on_data_written,
                                              shared_from_this()));
}

url_query_t
session_t::split_optional_queries(boost::string_view const &optional_query) {
  url_query_t result{};
  if (!optional_query.empty()) {
    auto queries = utilities::split_string_view(optional_query, "&");
    for (auto const &q : queries) {
      auto split = utilities::split_string_view(q, "=");
      if (split.size() < 2) {
        continue;
      }
      result.emplace(split[0], split[1]);
    }
  }
  return result;
}

namespace utilities {
std::string decode_url(boost::string_view const &encoded_string) {
  std::string src{};
  for (size_t i = 0; i < encoded_string.size();) {
    char const ch = encoded_string[i];
    if (ch != '%') {
      src.push_back(ch);
      ++i;
    } else {
      char c1 = encoded_string[i + 1];
      unsigned int localui1 = 0L;
      if ('0' <= c1 && c1 <= '9') {
        localui1 = c1 - '0';
      } else if ('A' <= c1 && c1 <= 'F') {
        localui1 = c1 - 'A' + 10;
      } else if ('a' <= c1 && c1 <= 'f') {
        localui1 = c1 - 'a' + 10;
      }

      char c2 = encoded_string[i + 2];
      unsigned int localui2 = 0L;
      if ('0' <= c2 && c2 <= '9') {
        localui2 = c2 - '0';
      } else if ('A' <= c2 && c2 <= 'F') {
        localui2 = c2 - 'A' + 10;
      } else if ('a' <= c2 && c2 <= 'f') {
        localui2 = c2 - 'a' + 10;
      }

      unsigned int ui = localui1 * 16 + localui2;
      src.push_back(ui);

      i += 3;
    }
  }

  return src;
}

std::vector<boost::string_view> split_string_view(boost::string_view const &str,
                                                  char const *delim) {
  std::size_t const delim_length = std::strlen(delim);
  std::size_t from_pos{};
  std::size_t index{str.find(delim, from_pos)};
  if (index == std::string::npos) {
    return {str};
  }
  std::vector<boost::string_view> result{};
  while (index != std::string::npos) {
    result.emplace_back(str.data() + from_pos, index - from_pos);
    from_pos = index + delim_length;
    index = str.find(delim, from_pos);
  }
  if (from_pos < str.length()) {
    result.emplace_back(str.data() + from_pos, str.size() - from_pos);
  }
  return result;
}

} // namespace utilities
} // namespace binance
