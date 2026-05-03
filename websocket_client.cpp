#include "websocket_client.h"
#include <iostream>

WebSocketClient::WebSocketClient(const std::string& symbol)
    : symbol_(symbol), ws_(ioc_) {}

void WebSocketClient::connect() {
    tcp::resolver resolver(ioc_);
    auto const results = resolver.resolve("ws.okx.com", "8443");
    net::connect(ws_.next_layer(), results.begin(), results.end());
    ws_.handshake("ws.okx.com", "/ws/v5/public");
    // 订阅 books5 和 trades
    json sub = {
        {"op", "subscribe"},
        {"args", {
            {{"channel", "books5"}, {"instId", symbol_}},
            {{"channel", "trades"}, {"instId", symbol_}}
        }}
    };
    ws_.write(net::buffer(sub.dump()));
}

void WebSocketClient::run() {
    while (true) {
        try {
            connect();
            beast::flat_buffer buffer;
            while (true) {
                ws_.read(buffer);
                auto msg = json::parse(beast::buffers_to_string(buffer.data()));
                buffer.clear();
                if (msg.contains("arg")) {
                    auto ch = msg["arg"]["channel"];
                    if (ch == "books5" && on_book) on_book(msg);
                    else if (ch == "trades" && on_trade) on_trade(msg);
                }
            }
        } catch (std::exception const& e) {
            std::cerr << "WebSocket error: " << e.what() << std::endl;
            ws_ = ws::stream<tcp::socket>(ioc_);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}