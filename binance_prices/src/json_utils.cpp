#include "json_utils.hpp"
#include <random>

namespace binance {

std::optional<json::object_t>
read_object_json_file(std::string const &filename) {
  return detail::read_json_file<json::object_t>(filename);
}

std::optional<json::array_t> read_array_json_file(std::string const &filename) {
  return detail::read_json_file<json::array_t>(filename);
}

namespace utilities {

std::optional<std::string> timet_to_string(std::size_t const t) {
#if _MSC_VER && !__INTEL_COMPILER
#pragma warning(disable : 4996)
#endif

  try {
    std::time_t current_time = t;
    auto const tm_t = std::gmtime(&current_time);
    if (!tm_t) {
      return std::nullopt;
    }
    std::string output((std::size_t)32, (char)'\0');
    auto const format = "%Y-%m-%d %H:%M:%S";
    auto const string_length =
        std::strftime(output.data(), output.size(), format, tm_t);
    if (string_length) {
      output.resize(string_length);
      return output;
    }
  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }
  return std::nullopt;
}

std::optional<std::string> timet_to_string(std::string const &str) {
  if (str.empty()) {
    return std::nullopt;
  }
  try {
    auto const t = static_cast<std::size_t>(std::stoull(str)) / 1'000;
    return timet_to_string(t);
  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }
  return std::nullopt;
}

std::size_t get_random_integer() {
  static std::random_device rd{};
  static std::mt19937 gen{rd()};
  static std::uniform_int_distribution<> uid(1, 50);
  return uid(gen);
}

char get_random_char() {
  static std::random_device rd{};
  static std::mt19937 gen{rd()};
  static std::uniform_int_distribution<> uid(0, 52);
  static char const *all_alphas =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_";
  return all_alphas[uid(gen)];
}

std::string get_random_string(std::size_t const length) {
  std::string result{};
  result.reserve(length);
  for (std::size_t i = 0; i != length; ++i) {
    result.push_back(get_random_char());
  }
  return result;
}

} // namespace utilities

} // namespace binance
