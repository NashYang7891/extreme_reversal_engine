#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>

int main() {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_client);
    ctx.set_default_verify_paths();
    boost::beast::websocket::stream<boost::beast::ssl_stream<boost::asio::ip::tcp::socket>> ws(ioc, ctx);
    boost::asio::ip::tcp::resolver resolver{ioc};
    auto const results = resolver.resolve("fstream.binance.com", "443");
    boost::asio::connect(ws.next_layer().next_layer(), results.begin(), results.end());
    ws.next_layer().handshake(boost::asio::ssl::stream_base::client);
    // 直接连接单流，无需订阅
    ws.handshake("fstream.binance.com", "/ws/btcusdt@aggTrade");

    boost::beast::flat_buffer buffer;
    for (int i = 0; i < 10; ++i) {
        ws.read(buffer);
        std::cout << boost::beast::buffers_to_string(buffer.data()) << std::endl;
        buffer.clear();
    }
    return 0;
}