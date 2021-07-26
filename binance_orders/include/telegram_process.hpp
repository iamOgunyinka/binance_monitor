#pragma once

#include "orders_info.hpp"
#include "tg_message_sender.hpp"
#include <vector>

namespace binance {
namespace ssl = net::ssl;

std::string prepare_telegram_payload(ws_order_info_t const &order);
std::string prepare_telegram_payload(ws_balance_info_t const &balance);
std::string prepare_telegram_payload(ws_account_update_t const &account);
void tg_get_new_updates(tg_ccached_map_t &chat_ids, ssl::context &ssl_context);
void send_telegram_message(
    std::vector<std::shared_ptr<tg_message_sender_t>> &message_senders,
    std::string &&text, std::string const &tg_name, tg_ccached_map_t &chat_ids,
    net::io_context &io_context, ssl::context &ssl_context);

} // namespace binance
