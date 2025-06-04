
// main.cpp

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/json.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <optional>
#include <chrono>
#include <unordered_map>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using     tcp       = net::ip::tcp;
namespace json      = boost::json;

// Возвращает текущее время в миллисекундах от эпохи
static uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

// Глобальные переменные для состояния
static std::optional<std::string> sentTxHash; // сюда запомним txHash из eth_sendTransaction
static uint64_t                     txSendMs = 0; // время (ms) отправки транзакции

// Если до получения ответа на sendTransaction нода уже выдала уведомление с нашим hash,
// запомним время получения в этот мап
static std::unordered_map<std::string, uint64_t> prebuffer;

// Асинхронное чтение из WebSocket
void do_read(
    std::shared_ptr<websocket::stream<tcp::socket>> ws_ptr,
    std::shared_ptr<beast::flat_buffer>            buffer_ptr)
{
    ws_ptr->async_read(
        *buffer_ptr,
        [ws_ptr, buffer_ptr](beast::error_code ec, std::size_t)
        {
            if (ec) {
                std::cerr << "[ERROR] Read error: " << ec.message() << "\n";
                return;
            }

            std::string text = beast::buffers_to_string(buffer_ptr->data());
            buffer_ptr->consume(buffer_ptr->size());

            json::value parsed;
            try {
                parsed = json::parse(text);
            }
            catch (const std::exception& e) {
                std::cerr << "[ERROR] JSON parse error: " << e.what() << "\n";
                do_read(ws_ptr, buffer_ptr);
                return;
            }

            if (!parsed.is_object()) {
                do_read(ws_ptr, buffer_ptr);
                return;
            }
            auto obj = parsed.as_object();

            // 1) Если это ответ на eth_sendTransaction (id == 2), запоминаем sentTxHash
            if (obj.if_contains("id") && obj["id"].is_int64()) {
                int id = static_cast<int>(obj["id"].as_int64());
                if (id == 2) {
                    if (obj.if_contains("result") && obj["result"].is_string()) {
                        auto txh = obj["result"].as_string().c_str();
                        sentTxHash = std::string(txh);
                        std::cout << "[INFO] eth_sendTransaction вернул txHash: " << txh << "\n";

                        // Как только узнали sentTxHash, проверяем: 
                        // возможно, нода уже присылала его до этого – есть в prebuffer?
                        auto it = prebuffer.find(txh);
                        if (it != prebuffer.end()) {
                            uint64_t receiveMs = it->second;
                            int64_t diff = static_cast<int64_t>(receiveMs) - static_cast<int64_t>(txSendMs);
                            std::cout << "\n[RESULT] (пропущенный в prebuffer)\n";
                            std::cout << "  txHash       = " << txh << "\n";
                            std::cout << "  txSendMs     = " << txSendMs << " ms\n";
                            std::cout << "  nowReceiveMs = " << receiveMs    << " ms\n";
                            std::cout << "  Задержка    = " << diff           << " ms\n\n";
                            std::cout << "[INFO] Завершаем работу.\n";
                            std::exit(EXIT_SUCCESS);
                        }
                    }
                    else if (obj.if_contains("error")) {
                        std::cerr << "[ERROR] eth_sendTransaction error: "
                                  << boost::json::serialize(obj["error"]) << "\n";
                    }
                }
            }

            // 2) Если это уведомление о newPendingTransactions
            if (obj.if_contains("method")
                && obj["method"].as_string() == "eth_subscription"
                && obj.if_contains("params"))
            {
                auto params = obj["params"].as_object();
                if (params.if_contains("result") && params["result"].is_string()) {
                    auto incomingTxHash = params["result"].as_string().c_str();
                    uint64_t receiveMs = nowMs();

                    // Сразу выводим любую новую транзакцию
                    std::cout << "[SUB] incoming txHash: " << incomingTxHash;

                    // Если sentTxHash уже известно и совпало – вычисляем задержку
                    if (sentTxHash.has_value() && incomingTxHash == sentTxHash.value()) {
                        int64_t diff = static_cast<int64_t>(receiveMs) - static_cast<int64_t>(txSendMs);
                        std::cout << "  <-- match! Δ = " << diff << " ms\n";
                        std::cout << "\n[RESULT] Нода получила нашу транзакцию в mempool:\n";
                        std::cout << "  txHash       = " << incomingTxHash << "\n";
                        std::cout << "  txSendMs     = " << txSendMs       << " ms\n";
                        std::cout << "  nowReceiveMs = " << receiveMs     << " ms\n";
                        std::cout << "  Задержка    = " << diff           << " ms\n\n";
                        std::cout << "[INFO] Завершаем работу.\n";
                        std::exit(EXIT_SUCCESS);
                    }
                    else {
                        // Если sentTxHash ещё не пришёл – буферизуем для возможного match позже
                        if (!sentTxHash.has_value()) {
                            prebuffer.emplace(std::string(incomingTxHash), receiveMs);
                        }
                        std::cout << "\n";
                    }
                }
            }

            do_read(ws_ptr, buffer_ptr);
        });
}

int main()
{
    try {
        const std::string host = "127.0.0.1";
        const std::string port = "8546";
        std::cout << "[INFO] Подключаемся к Geth-BSC по WS ws://"
                  << host << ":" << port << "\n";

        net::io_context ioc;
        tcp::resolver   resolver{ioc};
        auto            endpoints = resolver.resolve(host, port);

        auto ws_ptr = std::make_shared<websocket::stream<tcp::socket>>(ioc.get_executor());
        net::connect(ws_ptr->next_layer(), endpoints);

        // Измеряем WebSocket-RTT (опционально)
        std::chrono::steady_clock::time_point pingSentTime;
        ws_ptr->control_callback(
            [&](websocket::frame_type kind, beast::string_view) {
                if (kind == websocket::frame_type::pong) {
                    auto now = std::chrono::steady_clock::now();
                    auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(now - pingSentTime).count();
                    std::cout << "[RTT] WebSocket ping↔pong RTT: " << rtt << " ms\n";
                }
            }
        );

        ws_ptr->handshake(host + ":" + port, "/");
        std::cout << "[INFO] Connected to ws://127.0.0.1:8546\n";

        // Сразу после handshake шлём ping, чтобы измерить RTT
        pingSentTime = std::chrono::steady_clock::now();
        ws_ptr->ping({});

        // 1) Подписываемся на newPendingTransactions
        {
            json::object subObj;
            subObj["jsonrpc"] = "2.0";
            subObj["id"]      = 1;
            subObj["method"]  = "eth_subscribe";
            json::array arr; arr.push_back("newPendingTransactions");
            subObj["params"] = arr;

            ws_ptr->write(net::buffer(boost::json::serialize(subObj)));
            std::cout << "[INFO] Отправили подписку: eth_subscribe(\"newPendingTransactions\")\n";
        }

        // 2) Отправляем свою транзакцию (eth_sendTransaction)
        {
            // ← Замените адрес на ваш (в нижнем регистре):
            const std::string myAddress = "0x6f8dd885384f8d3671f0e7991c1937ded12d29c0";

            json::object tx;
            tx["from"]     = json::value(myAddress);
            tx["to"]       = json::value(myAddress);
            tx["value"]    = json::value("0x0");
            tx["gasPrice"] = json::value("0x1");    // 1 wei
            tx["gas"]      = json::value("0x5208"); // 21000

            json::object sendObj;
            sendObj["jsonrpc"] = "2.0";
            sendObj["id"]      = 2;
            sendObj["method"]  = "eth_sendTransaction";
            json::array paramsArr; paramsArr.push_back(tx);
            sendObj["params"] = paramsArr;

            txSendMs = nowMs();
            ws_ptr->write(net::buffer(boost::json::serialize(sendObj)));
            std::cout << "[INFO] Отправили eth_sendTransaction, txSendMs = "
                      << txSendMs << " ms\n";
        }

        // Запускаем асинхронное чтение
        auto buffer_ptr = std::make_shared<beast::flat_buffer>();
        do_read(ws_ptr, buffer_ptr);
        ioc.run();
    }
    catch (std::exception const& e) {
        std::cerr << "[FATAL] Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
