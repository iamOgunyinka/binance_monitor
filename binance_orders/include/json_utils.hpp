#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <spdlog/spdlog.h>

namespace binance {

using nlohmann::json;

namespace detail {
template <typename T>
std::optional<T> read_json_file(std::string const &filename) {
  std::ifstream file{filename};
  if (!file) {
    return std::nullopt;
  }
  json root_object;
  try {
    file >> root_object;
    return root_object.get<T>();
  } catch (std::exception const &e) {
    spdlog::error(e.what());
    return std::nullopt;
  }
}

} // namespace detail

std::optional<json::object_t>
read_object_json_file(std::string const &filename);

std::optional<json::array_t> read_array_json_file(std::string const &filename);

} // namespace binance
