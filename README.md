# binance_monitor
The aim of this project is to make a programmable system where you can schedule trades on Binance. It's most likely gonna be in form of an embedded DSL where you would be able to schedule series of trade commands that looks like so:
  ```
  watch out for the price of BTC
  when( the price is $35'000 ){
    place a LIMIT BUY order for 0.1 BTC
    when the order is filled {
      make a LIMIT SELL order for it at $39'000
      make a STOP LIMIT SELL order for $35'100 at $35'099
    }
    notify me via Telegram with chatid=-10000001
  }
```

It's a work in progress and the actual grammar for the _language_ has not been ironed out at the moment.

## Installation
Running the `command.sh` file on any Ubuntu linux distro will download the necessary libraries and install the services. Please check the commands to see what and what do not need to be adjusted before running it.
