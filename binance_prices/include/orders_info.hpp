#pragma once

#include <string>

namespace binance {

struct instrument_type_t {
  std::string instrument_id{};

  friend bool operator==(instrument_type_t const &this_,
                         instrument_type_t const &other) {
    return this_.instrument_id == other.instrument_id;
  }
};
} // namespace binance

namespace std {

template <> struct hash<binance::instrument_type_t> {
  std::size_t
  operator()(binance::instrument_type_t const &instr) const noexcept {
    return std::hash<std::string>{}(instr.instrument_id);
  }
};

} // namespace std
