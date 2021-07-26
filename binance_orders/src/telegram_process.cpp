#include "telegram_process.hpp"
#include "chat_update.hpp"
#include "common/json_utils.hpp"
#include "database_connector.hpp"
#include <boost/algorithm/string/replace.hpp>
#include <spdlog/spdlog.h>

namespace binance {
void on_tg_update_completion(tg_ccached_map_t &chat_ids,
                             std::string const &response,
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
      chat_ids[chat_title].telegram_chat_id = chat_id;
    }

  } catch (std::exception const &e) {
    spdlog::error(e.what());
  }
}

void tg_get_new_updates(tg_ccached_map_t &chat_ids, ssl::context &ssl_context) {
  net::io_context io_context{};
  auto sock = std::make_shared<chat_update_t>(
      io_context, ssl_context,
      [&chat_ids](std::string const &err_msg, std::string const &response_str) {
        on_tg_update_completion(chat_ids, err_msg, response_str);
        auto &database_connector = database_connector_t::s_get_db_connector();
        database_connector->insert_update_cached_ids(chat_ids);
      });
  sock->run();
  io_context.run();
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

void telegram_delivery_successful(std::string const &) {}

void send_telegram_message(
    std::vector<std::shared_ptr<tg_message_sender_t>> &message_senders,
    std::string &&text, std::string const &tg_name, tg_ccached_map_t &chat_ids,
    net::io_context &io_context, ssl::context &ssl_context) {
  auto iter = chat_ids.find(tg_name);
  if (iter == chat_ids.end()) {
    tg_get_new_updates(chat_ids, ssl_context);
    iter = chat_ids.find(tg_name);
    if (iter == chat_ids.end()) {
      return spdlog::error("Chat '{}' not found", tg_name);
    }
  }

  tg_payload_t payload{std::move(text), iter->second.telegram_chat_id};
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

} // namespace binance
