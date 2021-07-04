#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>

namespace binance {

namespace net = boost::asio;

class public_channel_websocket_t;

class orders_websock_t {
public:
  virtual ~orders_websock_t(){};
  virtual void run() = 0;
};

class public_channels_t : public orders_websock_t {
  std::unique_ptr<public_channel_websocket_t> channel_connector_;

public:
  public_channels_t(net::io_context &, net::ssl::context &);
  ~public_channels_t();
  void run() override;
};

} // namespace binance
