#include "session.hpp"
#include "database_connector.hpp"
#include "request_handler.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>
#include <spdlog/spdlog.h>

namespace binance {

using namespace fmt::v7::literals;

enum constant_e { RequestBodySize = 1'024 * 1'024 * 50 };

std::string get_alphanum_tablename(std::string str) {
  static auto non_alphanum_remover = [](char const ch) {
    return !std::isalnum(ch);
  };
  str.erase(std::remove_if(str.begin(), str.end(), non_alphanum_remover),
            str.end());
  boost::to_lower(str);
  return str;
}

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

std::optional<endpoint_t::rule_iterator>
endpoint_t::get_rules(boost::string_view const &target) {
  return get_rules(target.to_string());
}

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

  endpoint_apis_.add_endpoint("/upload", {verb::post},
                              [=](auto const &request, auto const &query) {
                                upload_handler(request, query);
                              });
}

void session_t::run() { http_read_data(); }

void session_t::http_read_data() {
  buffer_.clear();
  empty_body_parser_.emplace();
  empty_body_parser_->body_limit(RequestBodySize);
  beast::get_lowest_layer(tcp_stream_).expires_after(std::chrono::minutes(5));
  http::async_read_header(
      tcp_stream_, buffer_, *empty_body_parser_,
      [this](auto const ec, std::size_t const sz) { on_header_read(ec, sz); });
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
                     [this](beast::error_code const ec, std::size_t const sz) {
                       on_data_read(ec, sz);
                     });
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
    auto iter_end =
        iter.value()->second.verbs_.cbegin() + iter.value()->second.num_verbs_;
    auto found_iter =
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

void session_t::on_data_read(beast::error_code ec, std::size_t const) {
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
                      [this](beast::error_code const ec, std::size_t const sz) {
                        on_data_written(ec, sz);
                      });
  } else {
    http::async_write(tcp_stream_, *resp,
                      [this](auto const, auto const) { shutdown_socket(); });
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
      get_error("login", error_type_e::NoError, http::status::ok, request));
}

void session_t::upload_handler(string_request_t const &request,
                               url_query_t const &) {
  auto &body = request.body();
  auto &database_connector = database_connector_t::s_get_db_connector();
  std::vector<std::string> error_list{};
  try {
    json::array_t const info_list = json::parse(body).get<json::array_t>();
    for (auto const &json_item : info_list) {
      json::object_t const info = json_item.get<json::object_t>();
      host_info_t host_info{};
      host_info.api_key = info.at("api_key").get<json::string_t>();
      host_info.secret_key = info.at("secret_key").get<json::string_t>();
      host_info.account_alias = info.at("alias").get<json::string_t>();
      host_info.tg_group_name = info.at("tg_group").get<json::string_t>();

      if (database_connector->add_new_host(host_info)) {
        request_handler_t::get_host_container().append(std::move(host_info));
      } else {
        error_list.push_back(host_info.api_key);
      }
    }
    return send_response(json_success(error_list, request));
  } catch (std::exception const &except) {
    spdlog::error(except.what());
    return error_handler(bad_request("JSON object is invalid", request));
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
                    [this](beast::error_code const ec, std::size_t const sz) {
                      on_data_written(ec, sz);
                    });
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
