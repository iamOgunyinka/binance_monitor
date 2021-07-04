#include "orders_websock.hpp"
#include "public_channel_websocket.hpp"

namespace binance {

public_channels_t::public_channels_t(net::io_context &io_context,
                                     net::ssl::context &ssl_ctx)
    : channel_connector_{
          std::make_unique<public_channel_websocket_t>(io_context, ssl_ctx)} {}

public_channels_t::~public_channels_t() { channel_connector_.reset(); }

void public_channels_t::run() { channel_connector_->run(); }

} // namespace binance
