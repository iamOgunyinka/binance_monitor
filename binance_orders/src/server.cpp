#include "server.hpp"
#include "session.hpp"
#include <boost/asio/strand.hpp>
#include <spdlog/spdlog.h>

namespace binance {

server_t::operator bool() { return is_open_; }

server_t::server_t(net::io_context &context,
                   command_line_interface_t const &args)
    : io_context_{context}, endpoint_{net::ip::make_address(args.ip_address),
                                      args.port},
      acceptor_{net::make_strand(io_context_)}, args_{args} {
  beast::error_code ec{}; // used when we don't need to throw all around
  acceptor_.open(endpoint_.protocol(), ec);
  if (ec) {
    spdlog::error("Could not open socket: {}", ec.message());
    return;
  }
  acceptor_.set_option(net::socket_base::reuse_address(true), ec);
  if (ec) {
    spdlog::error("set_option failed: {}", ec.message());
    return;
  }
  acceptor_.bind(endpoint_, ec);
  if (ec) {
    spdlog::error("binding failed: {}", ec.message());
    return;
  }
  acceptor_.listen(net::socket_base::max_listen_connections, ec);
  if (ec) {
    spdlog::error("not able to listen: {}", ec.message());
    return;
  }
  is_open_ = true;
}

void server_t::run() {
  if (is_open_) {
    return accept_connections();
  }
}

void server_t::on_connection_accepted(beast::error_code const &ec,
                                      net::ip::tcp::socket socket) {
  if (ec) {
    spdlog::error("error on connection: {}", ec.message());
  } else {
    if (sessions_.size() > 20) {
      auto beg = std::remove_if(sessions_.begin(), sessions_.end(),
                                [](std::shared_ptr<session_t> &session_t) {
                                  return session_t->is_closed();
                                });
      sessions_.erase(beg, sessions_.end());
    }
    sessions_.push_back(
        std::make_shared<session_t>(io_context_, std::move(socket)));
    sessions_.back()->run();
  }
  accept_connections();
}

void server_t::accept_connections() {
  acceptor_.async_accept(
      net::make_strand(io_context_),
      beast::bind_front_handler(&server_t::on_connection_accepted,
                                shared_from_this()));
}

} // namespace binance
