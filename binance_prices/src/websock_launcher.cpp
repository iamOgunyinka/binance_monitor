#include "websock_launcher.hpp"
#include "database_connector.hpp"
#include "orders_websock.hpp"
#include "request_handler.hpp"
#include "ticking_timer.hpp"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <thread>

#include <spdlog/spdlog.h>

namespace binance {

void launch_price_watcher(
    std::vector<std::unique_ptr<orders_websock_t>> &websocks,
    net::io_context &io_context, ssl::context &ssl_context) {

  websocks.emplace_back(new public_channels_t(io_context, ssl_context));
  websocks.back()->run();
}

void background_price_saver() {
  auto &token_container = request_handler_t::get_tokens_container();
  auto &pushed_subs = request_handler_t::get_all_pushed_data();

  while (true) {
    auto item = token_container.get();
    boost::to_upper(item.instrument_id);
    pushed_subs[item.instrument_id] = item;
  }
}

void stop_ticker_list(std::shared_ptr<task_scheduler_t> &scheduler,
                      std::string const &request_id) {
  auto running_tickers = scheduler->get_tickers(request_id);
  for (auto &ticker : running_tickers) {
    ticker->stop();
  }
}

void stop_task_ticker(std::shared_ptr<task_scheduler_t> &scheduler,
                      std::unique_ptr<database_connector_t> &database_connector,
                      std::string const &request_id) {
  stop_ticker_list(scheduler, request_id);
  auto const last_end_time = utilities::timet_to_string(std::time(nullptr));
  auto const last_begin_time = "";
  database_connector->change_task_status(task_state_e::stopped, request_id,
                                         last_begin_time, *last_end_time);
}

void process_scheduled_tasks(
    net::io_context &io_context,
    std::unique_ptr<database_connector_t> &database_connector,
    waitable_container_t<scheduled_task_result_t> &tasks,
    std::map<std::string, std::shared_ptr<task_scheduler_t>>
        &task_scheduler_map,
    scheduled_task_t &item) {

  std::shared_ptr<task_scheduler_t> &scheduler =
      task_scheduler_map[item.for_username];

  if (!scheduler) {
    scheduler = std::make_shared<task_scheduler_t>(io_context);
  }
  switch (item.status) {
  case task_state_e::initiated: {
    item.status = task_state_e::running;
    auto const created_time = utilities::timet_to_string(item.current_time);
    if (created_time.has_value() &&
        database_connector->insert_new_task(item, *created_time)) {
      scheduler->monitor_new_task(std::move(item));
    }
  } break;
  case task_state_e::running:
    return scheduler->monitor_new_task(std::move(item));
  case task_state_e::stopped:
    return stop_task_ticker(scheduler, database_connector, item.request_id);
  case task_state_e::remove:
    stop_ticker_list(scheduler, item.request_id);
    database_connector->remove_task(item.request_id);
    return scheduler->remove_tickers(item.request_id);
  case task_state_e::restarted:
    // stop and remove those tasks if it has not happened already
    stop_task_ticker(scheduler, database_connector, item.request_id);
    scheduler->remove_tickers(item.request_id);
    if (auto stopped_tasks = database_connector->get_scheduled_tasks(
            {task_state_e::stopped}, item.request_id);
        !stopped_tasks.empty()) {
      auto const status = task_state_e::running;
      auto const last_end_time = "";
      auto const last_begin_time =
          utilities::timet_to_string(std::time(nullptr));
      for (auto &task : stopped_tasks) {
        task.status = status;
        database_connector->change_task_status(status, task.request_id,
                                               *last_begin_time, last_end_time);
        tasks.append(std::move(task));
      }
    }
    break;
  case task_state_e::unknown:
  default:
    break;
  }
}

void task_scheduler_watcher(net::io_context &io_context) {

  auto &database_connector = database_connector_t::s_get_db_connector();
  if (!database_connector->create_task_table()) {
    return;
  }
  auto &tasks = request_handler_t::get_all_scheduled_tasks();
  std::vector<task_state_e> const running_tasks{task_state_e::initiated,
                                                task_state_e::running};
  if (auto old_tasks = database_connector->get_scheduled_tasks(running_tasks);
      !old_tasks.empty()) {
    for (auto &task : old_tasks) {
      tasks.append(std::move(task));
    }
  }

  // wait a few seconds until all token prices would have been obtained
  std::this_thread::sleep_for(std::chrono::seconds(15));

  std::map<std::string, std::shared_ptr<task_scheduler_t>> task_scheduler_map{};
  std::map<std::string, std::string> username_tablename_map{};

  while (true) {
    auto item_var = tasks.get();
    std::visit(
        [&](auto &&item) {
          using item_type_t = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<item_type_t, scheduled_task_t>) {
            process_scheduled_tasks(io_context, database_connector, tasks,
                                    task_scheduler_map, item);
          } else {
            std::string &table_name = username_tablename_map[item.for_username];
            // seeing the username for the first time creates the table
            if (table_name.empty()) {
              table_name =
                  get_alphanum_tablename(item.for_username) + "_records";
              database_connector->create_pnl_table(table_name);
            }
            database_connector->insert_pnl_record(table_name, item);
          }
        },
        item_var);
  }
}

} // namespace binance
