#include "websock_launcher.hpp"
#include "chat_update.hpp"
#include "database_connector.hpp"
#include "request_handler.hpp"
#include "tg_message_sender.hpp"
#include <boost/algorithm/string/replace.hpp>
#include <spdlog/spdlog.h>
#include <thread>

namespace binance {

std::map<tg_handler_t::chat_name_t, tg_handler_t::chat_id_t>
    tg_handler_t::chat_map_{};

void on_tg_update_completion(std::string const &response,
                             std::string const &error_msg) {
  if (!error_msg.empty()) {
    return spdlog::error(error_msg);
  }
  try {
    auto const json_object = json::parse(response).get<json::object_t>();
    if (auto const is_ok = json_object.at("ok").get<json::boolean_t>();
        !is_ok) {
      return spdlog::error("There was an error from the bot server");
    }
    auto const result_list = json_object.at("result").get<json::array_t>();
    auto &chat_ids = tg_handler_t::chat_map_;

    for (auto const &json_message : result_list) {
      auto const message_item = json_message.get<json::object_t>();
      auto const message_item_iter = message_item.find("message");
      if (message_item_iter == message_item.cend()) {
        continue;
      }
      auto const message_object =
          message_item_iter->second.get<json::object_t>();
      auto const chat_item_iter = message_object.find("chat");
      if (chat_item_iter == message_object.cend()) {
        continue;
      }
      auto const chat_object = chat_item_iter->second.get<json::object_t>();
      auto const chat_id =
          std::to_string(chat_object.at("id").get<json::number_integer_t>());
      std::string chat_title{};
      auto const chat_type = chat_object.at("type").get<json::string_t>();
      if (chat_type == "group") {
        chat_title = chat_object.at("title").get<json::string_t>();
      } else if (chat_type == "private") {
        chat_title = chat_object.at("username").get<json::string_t>();
      }
      chat_ids[chat_title] = chat_id;
    }

  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }
}

void tg_get_new_updates(net::ssl::context &ssl_context) {
  net::io_context io_context{};
  auto sock = std::make_shared<chat_update_t>(io_context, ssl_context,
                                              on_tg_update_completion);
  sock->run();
  io_context.run();
}

void launch_websock_listeners(
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
    launch_websock_listeners(websocks, previous_hosts, io_context, ssl_context);
  }
}

std::string prepare_telegram_payload(ws_order_info_t const &order) {
  // %0A is defined as the newline character.
  // %20 is defined as the space character.

  std::string payload = "Exchange: Binance%0A";
  payload += ("OrderID: " + order.order_id + "%0A");
  payload += ("Token: " + order.instrument_id + "%0A");
  payload += ("Price: " + order.order_price + "%0A");
  payload += ("Qty: " + order.quantity_purchased + "%0A");
  payload += ("LastFilled: " + order.last_filled_quantity + "%0A");
  payload += ("Side: " + order.order_side + "%0A");
  payload += ("Type: " + order.order_type + "%0A");
  if (!order.commission_asset.empty()) {
    payload += ("Fee: " + order.commission_amount + " ( " +
                order.commission_asset + " )%0A");
  }
  payload += ("ExeType: " + order.execution_type + "%0A");
  payload += ("State: " + order.order_status + "%0A");
  payload += ("CreatedTime: " + order.created_time + "%0A");
  payload += ("TransactionTime: " + order.transaction_time + "%0A");

  boost::replace_all(payload, " ", "%20");
  return payload;
}

std::string prepare_telegram_payload(ws_balance_info_t const &balance) {
  std::string payload = "Exchange: Binance%0A";
  payload += ("Type: BalanceUpdate%0A");
  payload += ("Token: " + balance.instrument_id + "%0A");
  payload += ("Time: " + balance.clear_time + "%0A");
  payload += ("Balance: " + balance.balance + "%0A");
  boost::replace_all(payload, " ", "%20");

  return payload;
}

std::string prepare_telegram_payload(ws_account_update_t const &account) {
  std::string payload = "Exchange: Binance%0A";
  payload += ("Type: AccountUpdate%0A");
  payload += ("Token: " + account.instrument_id + "%0A");
  payload += ("Free: " + account.free_amount + "%0A");
  payload += ("Locked: " + account.locked_amount + "%0A");
  payload += ("EventTime: " + account.event_time + "%0A");
  payload += ("LastUpdateTime: " + account.last_account_update + "%0A");

  boost::replace_all(payload, " ", "%20");
  return payload;
}

void telegram_delivery_failed(std::string const &error_message) {
  spdlog::error(error_message);
}

void telegram_delivery_successful(std::string const &message_status) {
  // spdlog::info(message_status);
}

void send_telegram_message(
    std::vector<std::shared_ptr<tg_message_sender_t>> &message_senders,
    std::string &&text, std::string const &tg_name, net::io_context &io_context,
    net::ssl::context &ssl_context) {
  auto &chat_ids = tg_handler_t::chat_map_;
  auto iter = chat_ids.find(tg_name);
  if (iter == chat_ids.end()) {
    tg_get_new_updates(ssl_context);
    iter = chat_ids.find(tg_name);
    if (iter == chat_ids.end()) {
      return;
    }
  }

  tg_payload_t payload{std::move(text), iter->second};
  for (auto &sender : message_senders) {
    if (sender->available_with_less_tasks()) {
      return sender->add_payload(std::move(payload));
    }
  }

  // none available? Make attempt to remove all unused.
  if (message_senders.size() > 3) {
    auto remove_iter = std::remove_if(
        message_senders.begin(), message_senders.end(),
        [](auto &sender) { return sender->completed_operation(); });
    message_senders.erase(remove_iter, message_senders.end());
  }

  message_senders.emplace_back(std::make_unique<tg_message_sender_t>(
      io_context, ssl_context, std::move(payload), telegram_delivery_failed,
      telegram_delivery_successful));
  message_senders.back()->start();
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

void background_persistent_orders_saver(net::io_context &io_context,
                                        ssl::context &ssl_context) {
  auto &stream_container = request_handler_t::get_stream_container();
  auto &database_connector = database_connector_t::s_get_db_connector();
  std::map<std::string, std::string> account_table_map{};
  std::vector<std::shared_ptr<tg_message_sender_t>> message_senders{};

  while (true) {
    auto item_var = stream_container.get();

    std::visit(
        [&](auto &&item) {
          // first send the telegram message
          auto payload = prepare_telegram_payload(item);
          send_telegram_message(message_senders, std::move(payload),
                                item.telegram_group, io_context, ssl_context);
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
