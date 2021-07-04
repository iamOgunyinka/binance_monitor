#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>

#include "fields_alloc.hpp"

#define BN_REQUEST_PARAM                                                       \
  (string_request_t const &request, url_query_t const &optional_query)

namespace binance {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using string_response_t = http::response<http::string_body>;
using string_request_t = http::request<http::string_body>;
using dynamic_request = http::request_parser<http::string_body>;
using url_query_t = std::map<boost::string_view, boost::string_view>;
using string_body_ptr =
    std::unique_ptr<http::request_parser<http::string_body>>;
using alloc_t = fields_alloc<char>;
using nlohmann::json;

using callback_t =
    std::function<void(string_request_t const &, url_query_t const &)>;

struct rule_t {
  std::size_t num_verbs_{};
  std::array<http::verb, 3> verbs_{};
  callback_t route_callback_;

  rule_t(std::initializer_list<http::verb> &&verbs, callback_t callback);
};

class endpoint_t {
  std::map<std::string, rule_t> endpoints;
  using rule_iterator = std::map<std::string, rule_t>::iterator;

public:
  void add_endpoint(std::string const &, std::initializer_list<http::verb>,
                    callback_t &&);
  std::optional<rule_iterator> get_rules(std::string const &target);
  std::optional<rule_iterator> get_rules(boost::string_view const &target);
};

enum class error_type_e {
  NoError,
  ResourceNotFound,
  RequiresUpdate,
  BadRequest,
  ServerError,
  MethodNotAllowed,
  Unauthorized
};

// defined in subscription_data.hpp
enum class task_state_e : std::size_t;

class session_t {
  net::io_context &io_context_;
  beast::tcp_stream tcp_stream_;
  beast::flat_buffer buffer_{};
  std::optional<http::request_parser<http::empty_body>> empty_body_parser_{};
  string_body_ptr client_request_{};
  boost::string_view content_type_{};
  std::string current_username_{};
  std::shared_ptr<void> resp_;
  endpoint_t endpoint_apis_;
  std::optional<http::response<http::file_body, http::basic_fields<alloc_t>>>
      file_response_;
  alloc_t alloc_{8 * 1'024};
  // The file-based response serializer.
  std::optional<
      http::response_serializer<http::file_body, http::basic_fields<alloc_t>>>
      file_serializer_;
  static std::unordered_map<std::string, std::string> bearer_token_set_;

private:
  session_t *shared_from_this() { return this; }
  void add_endpoint_interfaces();
  void http_read_data();
  void on_header_read(beast::error_code, std::size_t const);
  void on_data_read(beast::error_code ec, std::size_t const);
  void shutdown_socket();
  void send_response(string_response_t &&response);
  void error_handler(string_response_t &&response, bool close_socket = false);
  void on_data_written(beast::error_code ec, std::size_t const bytes_written);
  void handle_requests(string_request_t const &request);
  void index_page_handler BN_REQUEST_PARAM;
  void get_file_handler BN_REQUEST_PARAM;
  void get_trading_pairs_handler BN_REQUEST_PARAM;
  void get_price_handler BN_REQUEST_PARAM;
  void user_login_handler BN_REQUEST_PARAM;
  void create_user_handler BN_REQUEST_PARAM;
  void scheduled_job_handler BN_REQUEST_PARAM;
  void get_user_jobs_handler BN_REQUEST_PARAM;

  void add_new_jobs(string_request_t const &);
  void stop_scheduled_jobs(string_request_t const &, task_state_e const);
  void restart_scheduled_jobs(string_request_t const &);
  void get_tasks_result(string_request_t const &);
  bool is_validated_user(string_request_t const &);
  bool is_validated_user(std::string const &);
  bool is_json_request() const;
  std::optional<std::string> extract_bearer_token(string_request_t const &);
  std::string generate_bearer_token(std::string const &username,
                                    int const user_role,
                                    std::string const &secret_key);
  bool check_database_for_token(std::string const &token);

private:
  static string_response_t json_success(json const &body,
                                        string_request_t const &req);
  static string_response_t success(char const *message,
                                   string_request_t const &);
  static string_response_t bad_request(std::string const &message,
                                       string_request_t const &);
  static string_response_t permission_denied(string_request_t const &);
  static string_response_t not_found(string_request_t const &);
  static string_response_t upgrade_required(string_request_t const &);
  static string_response_t method_not_allowed(string_request_t const &request);
  static string_response_t server_error(std::string const &, error_type_e,
                                        string_request_t const &);
  static string_response_t get_error(std::string const &, error_type_e,
                                     http::status, string_request_t const &);
  static url_query_t split_optional_queries(boost::string_view const &args);
  template <typename Func>
  void send_file(std::filesystem::path const &, boost::string_view,
                 string_request_t const &, Func &&func);

public:
  session_t(net::io_context &io, net::ip::tcp::socket &&socket);
  bool is_closed();
  void run();
};

template <typename Func>
void session_t::send_file(std::filesystem::path const &file_path,
                          boost::string_view const content_type,
                          string_request_t const &request, Func &&func) {
  std::error_code ec_{};
  if (!std::filesystem::exists(file_path, ec_)) {
    return error_handler(bad_request("file does not exist", request));
  }
  http::file_body::value_type file;
  beast::error_code ec{};
  file.open(file_path.string().c_str(), beast::file_mode::read, ec);
  if (ec) {
    return error_handler(server_error("unable to open file specified",
                                      error_type_e::ServerError, request));
  }
  file_response_.emplace(std::piecewise_construct, std::make_tuple(),
                         std::make_tuple(alloc_));
  file_response_->result(http::status::ok);
  file_response_->keep_alive(request.keep_alive());
  file_response_->set(http::field::server, "okex-feed");
  file_response_->set(http::field::content_type, content_type);
  file_response_->body() = std::move(file);
  file_response_->prepare_payload();
  file_serializer_.emplace(*file_response_);
  http::async_write(tcp_stream_, *file_serializer_,
                    [callback = std::move(func), self = shared_from_this()](
                        beast::error_code ec, std::size_t const size_written) {
                      self->file_serializer_.reset();
                      self->file_response_.reset();
                      callback();
                      self->on_data_written(ec, size_written);
                    });
}

std::optional<json::object_t>
decode_bearer_token(std::string const &token, std::string const &secret_key);
std::string get_alphanum_tablename(std::string);

} // namespace binance
