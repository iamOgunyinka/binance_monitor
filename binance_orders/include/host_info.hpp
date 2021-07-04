#pragma once
#include <string>

namespace binance {

enum class host_changed_e : std::size_t {
  no_changes,
  host_removed,
  tg_group_changed
};

struct host_info_t {
  std::string account_alias{};
  std::string api_key{};
  std::string secret_key{};
  std::string tg_group_name{};
  host_changed_e changes{host_changed_e::no_changes};
};

bool operator==(host_info_t const &, host_info_t const &);

} // namespace binance
