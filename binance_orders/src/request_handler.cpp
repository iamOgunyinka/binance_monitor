#include "request_handler.hpp"

namespace binance {

waitable_container_t<host_info_t> request_handler_t::host_container_{};

waitable_container_t<user_stream_result_t>
    request_handler_t::user_stream_container_{};

} // namespace binance
