#include "websock_launcher.hpp"
#include "database_connector.hpp"
#include "request_handler.hpp"
#include "tg_message_sender.hpp"
#include <boost/algorithm/string/replace.hpp>
#include <spdlog/spdlog.h>
#include <thread>

namespace binance {

void launch_websock_listeners(
    std::vector<std::shared_ptr<private_channel_websocket_t>> &websocks,
    std::vector<host_info_t> &previous_hosts, net::io_context &io_context,
    ssl::context &ssl_context) {
  for (auto &host : previous_hosts) {
    websocks.emplace_back(new private_channel_websocket_t(
        io_context, ssl_context, std::move(host)));
    websocks.back()->run();
  }
  io_context.run();
}

void launch_previous_hosts(
    std::vector<std::shared_ptr<private_channel_websocket_t>> &websocks,
    net::io_context &io_context, ssl::context &ssl_context) {

  auto &database_connector = database_connector_t::s_get_db_connector();
  if (auto previous_hosts = database_connector->get_available_hosts();
      !previous_hosts.empty()) {
    websocks.reserve(previous_hosts.size());
    launch_websock_listeners(websocks, previous_hosts, io_context, ssl_context);
  }
}

std::string prepare_telegram_payload(ws_order_info_t const &order) {
  // %0A is defined as the newline character.
  // %20 is defined as the space character.

  std::string payload{"message="};
  payload += ("Exchange: Binance%0A");
  payload += ("OrderID: " + order.order_id + "%0A");
  payload += ("Token: " + order.instrument_id + "%0A");
  payload += ("Price: " + order.order_price + "%0A");
  payload += ("Qty: " + order.quantity_purchased + "%0A");
  payload += ("LastFilled: " + order.last_filled_quantity + "%0A");
  payload += ("Side: " + order.order_side + "%0A");
  payload += ("Type: " + order.order_type + "%0A");
  payload += ("Fee: " + order.commission_amount + " ( " +
              order.commission_asset + " )%0A");
  payload += ("ExeType: " + order.execution_type + "%0A");
  payload += ("State: " + order.order_status + "%0A");
  payload += ("CreatedTime: " + order.created_time + "%0A");
  payload += ("TransactionTime: " + order.transaction_time + "%0A");
  payload += ("&name=" + order.telegram_group);
  boost::replace_all(payload, " ", "%20");
  return payload;
}

void websock_launcher(
    std::vector<std::shared_ptr<private_channel_websocket_t>> &websocks,
    net::io_context &io_context, ssl::context &ssl_context) {

  auto error_callback = [](std::string const &error_string) {
    spdlog::error(error_string);
  };

  auto completion_callback = [](std::string const &response_string) {
    spdlog::info(response_string);
  };

  std::vector<std::shared_ptr<tg_message_sender_t>> message_senders{};
  auto &host_container = request_handler_t::get_host_container();
  while (true) {
    auto item = host_container.get();
    std::visit(
        [&websocks, &io_context, &ssl_context, &message_senders, error_callback,
         completion_callback](auto &&item_var) {
          using item_type_t = std::decay_t<decltype(item_var)>;
          if constexpr (std::is_same_v<item_type_t, host_info_t>) {
            if (item_var.changes == host_changed_e::no_changes) {
              websocks.emplace_back(
                  std::make_shared<private_channel_websocket_t>(
                      io_context, ssl_context, std::move(item_var)));
              return websocks.back()->run();
            }
            auto find_iter = std::find_if(
                websocks.begin(), websocks.end(),
                [&item_var](
                    std::shared_ptr<private_channel_websocket_t> &websock) {
                  return item_var == websock->host_info();
                });

            if (find_iter != websocks.end()) {
              auto const changes_proposed = item_var.changes;
              if (changes_proposed == host_changed_e::host_removed) {
                (*find_iter)->stop();
                websocks.erase(find_iter);
              } else if (changes_proposed == host_changed_e::tg_group_changed) {
                (*find_iter)->host_info().tg_group_name =
                    item_var.tg_group_name;
              }
            }
          } else {
            auto payload = prepare_telegram_payload(item_var);
            for (auto &sender : message_senders) {
              if (sender->available_with_less_tasks()) {
                sender->add_payload(std::move(payload));
                return;
              }
            }
            // none available?
            if (message_senders.size() > 3) {
              message_senders.erase(
                  std::remove_if(message_senders.begin(), message_senders.end(),
                                 [](auto &sender) {
                                   return sender->completed_operation();
                                 }),
                  message_senders.end());
            }
            message_senders.emplace_back(std::make_unique<tg_message_sender_t>(
                io_context, ssl_context, std::move(payload), error_callback,
                completion_callback));
            message_senders.back()->start();
          }
        },
        item);
  }
}

void background_persistent_orders_saver() {
  auto &order_container = request_handler_t::get_orders_container();
  auto &host_container = request_handler_t::get_host_container();
  auto &database_connector = database_connector_t::s_get_db_connector();
  std::map<std::string, std::string> account_table_map{};

  while (true) {
    auto item = order_container.get();
    // schedule it for announcement in the telegram group
    host_container.append(item);

    // then save it locally in the DB
    auto &tablename = account_table_map[item.for_aliased_account];
    if (tablename.empty()) {
      tablename = item.for_aliased_account + "_orders";
      database_connector->create_order_table(tablename);
    }
    database_connector->add_new_order(tablename, item);
  }
}

bool operator==(host_info_t const &first, host_info_t const &second) {
  return std::tie(first.account_alias, first.api_key, first.secret_key) ==
         std::tie(second.account_alias, second.api_key, second.secret_key);
}

bool changes_made_to_host(host_info_t const &original,
                          host_info_t const &new_host) {
  return std::tie(original.tg_group_name, original.account_alias) !=
         std::tie(new_host.tg_group_name, new_host.account_alias);
}

void monitor_host_changes() {
  auto &database_connector = database_connector_t::s_get_db_connector();
  auto previous_hosts = database_connector->get_available_hosts();
  auto &host_container = request_handler_t::get_host_container();

  do {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    auto new_hosts = database_connector->get_available_hosts();

    for (auto &new_host : new_hosts) {
      auto find_iter =
          std::find(previous_hosts.begin(), previous_hosts.end(), new_host);
      if (find_iter == previous_hosts.cend()) {
        host_container.append(new_host);
        previous_hosts.push_back(new_host);
      } else {
        if (changes_made_to_host(*find_iter, new_host)) {
          new_host.changes = host_changed_e::tg_group_changed;
          find_iter->tg_group_name = new_host.tg_group_name;
          find_iter->account_alias = new_host.account_alias;
          host_container.append(new_host);
        }
      }
    }

    for (auto iter = previous_hosts.cbegin(); iter != previous_hosts.cend();) {
      auto &old_host = *iter;
      auto find_iter =
          std::find(new_hosts.cbegin(), new_hosts.cend(), old_host);
      if (find_iter ==
          new_hosts.cend()) { // this account must have been removed
        auto new_host = old_host;
        new_host.changes = host_changed_e::host_removed;
        host_container.append(new_host);
        iter = previous_hosts.erase(iter);
      } else {
        ++iter;
      }
    }
  } while (true);
}
} // namespace binance
