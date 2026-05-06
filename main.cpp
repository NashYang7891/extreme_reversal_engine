/*
 * main.cpp - 交易执行引擎（开仓自动挂对应止损单）
 * 依赖：nlohmann/json.hpp (同目录下单头文件)
 * 编译：g++ -std=c++11 -pthread main.cpp -o engine
 */

#include <iostream>
#include <string>
#include <thread>
#include <map>
#include <atomic>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

const int PORT = 5555;
const int BUFFER_SIZE = 4096;

std::atomic<int> next_order_id{10000};

struct OrderRecord {
    int order_id;
    int stop_order_id;          // 关联止损单 ID
    std::string symbol;
    std::string side;
    int quantity;
    std::string status;         // filled, cancelled, pending_stop ...
};

std::map<int, OrderRecord> order_book;

// 模拟下单（实际应接入券商/交易所 API）
int simulate_order(const std::string &symbol, const std::string &side, 
                   int quantity, const std::string &order_type) {
    int id = next_order_id++;
    std::cout << "[ORDER] id=" << id << " " << symbol << " " << side 
              << " qty=" << quantity << " type=" << order_type << std::endl;
    OrderRecord rec;
    rec.order_id = id;
    rec.stop_order_id = 0;
    rec.symbol = symbol;
    rec.side = side;
    rec.quantity = quantity;
    rec.status = "filled";
    order_book[id] = rec;
    return id;
}

// 模拟条件止损挂单
int simulate_stop_order(int parent_order_id, const std::string &symbol, 
                        const std::string &side, int quantity,
                        double trigger_price, double limit_price) {
    int stop_id = next_order_id++;
    std::cout << "[STOP] id=" << stop_id << " parent=" << parent_order_id
              << " " << symbol << " " << side << " qty=" << quantity
              << " trigger=" << trigger_price << " limit=" << limit_price << std::endl;
    if (order_book.count(parent_order_id)) {
        order_book[parent_order_id].stop_order_id = stop_id;
    }
    OrderRecord stop_rec;
    stop_rec.order_id = stop_id;
    stop_rec.stop_order_id = 0;
    stop_rec.symbol = symbol;
    stop_rec.side = side;
    stop_rec.quantity = quantity;
    stop_rec.status = "pending_stop";
    order_book[stop_id] = stop_rec;
    return stop_id;
}

// 处理开仓+止损信号
std::string process_order_with_stop(const json &order) {
    if (!order.contains("symbol") || !order.contains("side") ||
        !order.contains("quantity") || !order.contains("order_type")) {
        return "{\"error\": \"Missing required fields\"}";
    }
    if (!order.contains("stop_loss")) {
        return "{\"error\": \"stop_loss object required\"}";
    }
    auto sl = order["stop_loss"];
    if (!sl.contains("trigger_price") || !sl.contains("limit_price")) {
        return "{\"error\": \"stop_loss must have trigger_price and limit_price\"}";
    }

    std::string symbol = order["symbol"];
    std::string side = order["side"];
    int quantity = order["quantity"];
    std::string order_type = order.value("order_type", "market");
    double trigger_price = sl["trigger_price"];
    double limit_price = sl["limit_price"];
    int sl_qty = sl.value("quantity", quantity);

    std::string stop_side = (side == "buy") ? "sell" : "buy";

    int main_id = simulate_order(symbol, side, quantity, order_type);
    int stop_id = simulate_stop_order(main_id, symbol, stop_side, 
                                      sl_qty, trigger_price, limit_price);

    json resp;
    resp["status"] = "ok";
    resp["main_order"] = {
        {"order_id", main_id},
        {"symbol", symbol},
        {"side", side},
        {"quantity", quantity},
        {"status", "filled"}
    };
    resp["stop_order"] = {
        {"order_id", stop_id},
        {"parent_order_id", main_id},
        {"symbol", symbol},
        {"side", stop_side},
        {"quantity", sl_qty},
        {"trigger_price", trigger_price},
        {"limit_price", limit_price},
        {"status", "pending_stop"}
    };
    return resp.dump();
}

// 处理撤单（撤主单自动联动撤销止损单）
std::string process_cancel(const json &cancel) {
    if (!cancel.contains("order_id")) {
        return "{\"error\": \"Missing order_id\"}";
    }
    int order_id = cancel["order_id"];
    auto it = order_book.find(order_id);
    if (it == order_book.end()) {
        return "{\"error\": \"Order not found\"}";
    }
    it->second.status = "cancelled";
    std::cout << "[CANCEL] order_id=" << order_id << std::endl;

    if (it->second.stop_order_id != 0) {
        int stop_id = it->second.stop_order_id;
        if (order_book.count(stop_id)) {
            order_book[stop_id].status = "cancelled";
            std::cout << "[AUTO CANCEL STOP] stop_id=" << stop_id << std::endl;
        }
    }

    json resp;
    resp["status"] = "cancelled";
    resp["order_id"] = order_id;
    return resp.dump();
}

std::string handle_message(const std::string &msg) {
    try {
        json j = json::parse(msg);
        std::string action = j.value("action", "");
        if (action == "order_with_stop") {
            return process_order_with_stop(j);
        } else if (action == "cancel") {
            return process_cancel(j);
        } else {
            return "{\"error\": \"Unknown action\"}";
        }
    } catch (json::parse_error &e) {
        return "{\"error\": \"JSON parse error: " + std::string(e.what()) + "\"}";
    }
}

void client_handler(int client_sock) {
    char buffer[BUFFER_SIZE];
    std::string data;
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            std::cout << "Client disconnected\n";
            break;
        }
        data.append(buffer, bytes);
        size_t pos;
        while ((pos = data.find('\n')) != std::string::npos) {
            std::string msg = data.substr(0, pos);
            data.erase(0, pos + 1);
            if (!msg.empty()) {
                std::string resp = handle_message(msg) + "\n";
                send(client_sock, resp.c_str(), resp.size(), 0);
            }
        }
    }
    close(client_sock);
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed"); exit(EXIT_FAILURE);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt"); exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed"); exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0) {
        perror("listen"); exit(EXIT_FAILURE);
    }
    std::cout << "Engine with auto-stop on port " << PORT << std::endl;

    while (true) {
        socklen_t addrlen = sizeof(address);
        int client_sock = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (client_sock < 0) {
            perror("accept"); continue;
        }
        std::thread(client_handler, client_sock).detach();
    }
    return 0;
}