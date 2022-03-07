#include "request_handler.hpp"

namespace binance {

waitable_container_t<pushed_subscription_data_t>
    request_handler_t::tokens_container_{};

locked_set_t<instrument_type_t> request_handler_t::all_listed_instruments_{};

subscription_data_map_t request_handler_t::all_pushed_sub_data_{};

} // namespace binance
