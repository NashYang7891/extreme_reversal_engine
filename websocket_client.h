#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace beast = boost::beast;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

class WebSocketClient {
public:
    using Callback = std::function<void(const json&)>;
    WebSocketClient(const std::string& symbol);
    void run();
    Callback on_book;
    Callback on_trade;
private:
    std::string symbol_;
    net::io_context ioc_;
    ws::stream<tcp::socket> ws_;
    void connect();
};