#include "background_threads.hpp"
#include "database_connector.hpp"
#include "request_handler.hpp"
#include "telegram_process.hpp"
#include <thread>

namespace binance {

void launch_websocket_listeners(
    std::vector<std::shared_ptr<user_data_stream_t>> &websocks,
    std::vector<host_info_t> &previous_hosts, net::io_context &io_context,
    ssl::context &ssl_context) {
  for (auto &host : previous_hosts) {
    websocks.emplace_back(
        new user_data_stream_t(io_context, ssl_context, std::move(host)));
    websocks.back()->run();
  }
  io_context.run();
}

void launch_previous_hosts(
    std::vector<std::shared_ptr<user_data_stream_t>> &websocks,
    net::io_context &io_context, ssl::context &ssl_context) {

  auto &database_connector = database_connector_t::s_get_db_connector();
  if (auto previous_hosts = database_connector->get_available_hosts();
      !previous_hosts.empty()) {
    websocks.reserve(previous_hosts.size());
    launch_websocket_listeners(websocks, previous_hosts, io_context,
                               ssl_context);
  }
}

void process_host_changes(
    host_info_t &&host,
    std::vector<std::shared_ptr<user_data_stream_t>> &websocks,
    net::io_context &io_context, ssl::context &ssl_context) {
  if (host.changes == host_changed_e::no_changes) {
    websocks.emplace_back(std::make_shared<user_data_stream_t>(
        io_context, ssl_context, std::move(host)));
    return websocks.back()->run();
  }
  auto find_iter =
      std::find_if(websocks.begin(), websocks.end(),
                   [&host](std::shared_ptr<user_data_stream_t> &websock) {
                     return host == websock->host_info();
                   });

  if (find_iter != websocks.end()) {
    auto const changes_proposed = host.changes;
    if (changes_proposed == host_changed_e::host_removed) {
      (*find_iter)->stop();
      websocks.erase(find_iter);
    } else if (changes_proposed == host_changed_e::tg_group_changed) {
      (*find_iter)->host_info().tg_group_name = host.tg_group_name;
    }
  }
}

void websock_launcher(
    std::vector<std::shared_ptr<user_data_stream_t>> &websocks,
    net::io_context &io_context, ssl::context &ssl_context) {

  auto &host_container = request_handler_t::get_host_container();

  while (true) {
    auto item = host_container.get();
    process_host_changes(std::move(item), websocks, io_context, ssl_context);
  }
}

void persistent_orders_saver(net::io_context &io_context,
                             ssl::context &ssl_context) {
  auto &stream_container = request_handler_t::get_stream_container();
  auto &database_connector = database_connector_t::s_get_db_connector();
  std::map<std::string, std::string> account_table_map{};
  auto chats_id_map{database_connector->get_tg_cached_ids()};

  std::vector<std::shared_ptr<tg_message_sender_t>> message_senders{};
  tg_get_new_updates(chats_id_map, ssl_context);

  while (true) {
    auto item_var = stream_container.get();

    std::visit(
        [&](auto &&item) {
          // first send the telegram message
          auto payload = prepare_telegram_payload(item);
          send_telegram_message(message_senders, std::move(payload),
                                item.telegram_group, chats_id_map, io_context,
                                ssl_context);
          // then save it locally in the DB
          auto &table_alias = account_table_map[item.for_aliased_account];
          if (table_alias.empty()) {
            table_alias = get_alphanum_tablename(item.for_aliased_account);
            auto const orders_tablename = table_alias + "_orders";
            auto const balance_tablename = table_alias + "_balance";
            // auto const acct_update_tablename = table_alias + "_account";
            database_connector->create_order_table(orders_tablename);
            database_connector->create_balance_table(balance_tablename);
          }

          using item_type = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<item_type, ws_order_info_t>) {
            auto const table_name = table_alias + "_orders";
            database_connector->add_new_order(table_name, item);
          } else if constexpr (std::is_same_v<item_type, ws_balance_info_t>) {
            auto const table_name = table_alias + "_balance";
            database_connector->add_new_balance(table_name, item);
          } else {
          }
        },
        item_var);
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

void monitor_database_host_table_changes() {
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
      auto const find_iter =
          std::find(new_hosts.cbegin(), new_hosts.cend(), old_host);
      // this account must have been removed
      if (find_iter == new_hosts.cend()) {
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
