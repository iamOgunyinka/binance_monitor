#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>

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
  std::shared_ptr<void> resp_;
  endpoint_t endpoint_apis_;

private:
  void add_endpoint_interfaces();
  void http_read_data();
  void on_header_read(beast::error_code, std::size_t const);
  void on_data_read(beast::error_code ec, std::size_t const);
  void shutdown_socket();
  void send_response(string_response_t &&response);
  void error_handler(string_response_t &&response, bool close_socket = false);
  void on_data_written(beast::error_code ec, std::size_t const bytes_written);
  void handle_requests(string_request_t const &request);
  void upload_handler(string_request_t const &request,
                      url_query_t const &optional_query);
  void index_page_handler(string_request_t const &request,
                          url_query_t const &optional_query);

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

public:
  session_t(net::io_context &io, net::ip::tcp::socket &&socket);
  bool is_closed();
  void run();
};

std::string get_alphanum_tablename(std::string);

namespace utilities {

std::string decode_url(boost::string_view const &encoded_string);
std::vector<boost::string_view> split_string_view(boost::string_view const &str,
                                                  char const *delim);

} // namespace utilities

} // namespace binance
