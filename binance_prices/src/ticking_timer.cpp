#include "ticking_timer.hpp"
#include "request_handler.hpp"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/asio/post.hpp>

namespace binance {
task_scheduler_t::task_scheduler_t(net::io_context &io)
    : io_context_{io}, timers_{} {}

void task_scheduler_t::monitor_new_task(scheduled_task_t &&task) {
  timers_.emplace_back(
      std::make_unique<pnl_ticking_timer_t>(io_context_, std::move(task)));
  timers_.back()->run();
}

std::vector<pnl_ticking_timer_t *>
task_scheduler_t::get_tickers(std::string const &request_id) {
  std::vector<pnl_ticking_timer_t *> result_list{};
  for (auto &ticker : timers_) {
    if (ticker->request_id() == request_id) {
      result_list.push_back(ticker.get());
    }
  }
  return result_list;
}

void task_scheduler_t::remove_tickers(std::string const &request_id) {
  for (auto iter = timers_.begin(); iter != timers_.end();) {
    if ((*iter)->request_id() == request_id) {
      iter = timers_.erase(iter);
    } else {
      ++iter;
    }
  }
}

pnl_ticking_timer_t::pnl_ticking_timer_t(net::io_context &io,
                                         scheduled_task_t &&task)
    : io_context_{io}, timer_{}, task_{std::move(task)},
      token_iter_{request_handler_t::get_all_pushed_data().cend()},
      direction_{string_to_direction(task_.direction)} {}

void pnl_ticking_timer_t::run() {
  auto &token_list = request_handler_t::get_all_pushed_data();
  token_iter_ = token_list.find(task_.token_name);
  if (token_iter_ != token_list.cend()) {
    price_obtained_ = true;
    send_price();
  }
  timer_.emplace(io_context_);
  on_periodic_time_timeout();
}

void pnl_ticking_timer_t::stop() {
  if (timer_.has_value()) {
    timer_->cancel();
    timer_.reset();
  }
}

void pnl_ticking_timer_t::send_price() {
  scheduled_task_t::task_result_t result{};
  result.for_username = task_.for_username;
  result.token_name = task_.token_name;
  result.request_id = task_.request_id;
  result.order_price = task_.order_price;
  result.direction = direction_;
  result.column_id = task_.column_id;
  result.task_type = task_.task_type;
  result.money = task_.money;
  result.quantity = task_.quantity;

  double open_24h = 0.0;
  if (!price_obtained_) {
    auto &token_list = request_handler_t::get_all_pushed_data();
    token_iter_ = token_list.find(task_.token_name);
    if (token_iter_ != token_list.cend()) {
      price_obtained_ = true;
      open_24h = token_iter_->second.open_24h;
      result.mkt_price = token_iter_->second.current_price;
    }
  } else {
    open_24h = token_iter_->second.open_24h;
    result.mkt_price = token_iter_->second.current_price;
  }

  if (price_obtained_) {
    if (result.order_price == 0.0) {
      result.order_price = result.mkt_price;
    }
    if ((result.quantity == 0.0) && (result.money > 0.0)) {
      result.quantity = result.money / result.order_price;
    }

    if (task_.task_type == task_type_e::profit_and_loss) {
      if (result.direction == trade_direction_e::buy) {
        result.pnl = (result.mkt_price - result.order_price) * result.quantity;
      } else {
        result.pnl = (result.order_price - result.mkt_price) * result.quantity;
      }
    } else if (task_.task_type == task_type_e::price_changes) {
      result.pnl = ((result.mkt_price - open_24h) / open_24h) * 100.0;
    }
  }

  if (auto const opt_time = utilities::timet_to_string(task_.current_time);
      opt_time.has_value()) {
    result.current_time = *opt_time;
  }
  task_.current_time += task_.monitor_time_secs;
  request_handler_t::get_all_scheduled_tasks().append(std::move(result));
}

void pnl_ticking_timer_t::on_periodic_time_timeout() {
  timer_->expires_after(std::chrono::seconds(task_.monitor_time_secs));
  timer_->async_wait([this](boost::system::error_code const &ec) {
    if (ec) {
      return;
    }
    send_price();
    timer_->cancel();
    net::post(io_context_, [this] { on_periodic_time_timeout(); });
  });
}

std::string direction_to_string(trade_direction_e const &direction) {
  switch (direction) {
  case trade_direction_e::buy:
    return "buy";
  case trade_direction_e::sell:
    return "sell";
  default:
    return "none";
  }
}

trade_direction_e string_to_direction(std::string const &str) {
  if (str == "buy") {
    return trade_direction_e::buy;
  } else if (str == "sell") {
    return trade_direction_e::sell;
  }
  return trade_direction_e::none;
}

} // namespace binance
