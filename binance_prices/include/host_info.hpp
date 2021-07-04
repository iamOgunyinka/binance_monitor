#pragma once
#include <string>

namespace binance {

struct host_info_t {
  std::string account_alias{};
  std::string passphrase{};
  std::string api_key{};
  std::string secret_key{};
  std::string ws_host{};
  std::string ws_port_number{};
  std::string tg_group_name{};
};
} // namespace binance
