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
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <chrono>

namespace beast     = boost::beast;            // из <boost/beast.hpp>
namespace websocket = beast::websocket;        // из <boost/beast/websocket.hpp>
namespace net       = boost::asio;             // из <boost/asio.hpp>
using     tcp       = net::ip::tcp;            // из <boost/asio/ip/tcp.hpp>
namespace json      = boost::json;

// Генератор requestId для JSON-RPC (1 уже занят «eth_subscribe»)
static std::atomic<int> nextRequestId{2};

// Структура для временного хранения деталей лога, пока не придёт timestamp
struct LogInfo {
    std::string contract;
    std::string blockHex;
    std::string txHash;
    std::string logIndexHex;
    std::string sender;
    std::string fromOrTo;
};

// Здесь храним все незавершённые запросы «eth_getBlockByNumber», чтобы потом сопоставить ответ с нужным логом
static std::mutex                          pendingMutex;
static std::unordered_map<int, LogInfo>    pendingRequests;

// Утилита: читаем из «0x...»-строки беззнаковое 64-битное число
uint64_t parseHexUint64(const std::string& hexStr) {
    uint64_t value = 0;
    std::size_t start = 0;
    if (hexStr.rfind("0x", 0) == 0) start = 2;
    for (std::size_t i = start; i < hexStr.size(); ++i) {
        value <<= 4;
        char c = hexStr[i];
        if (c >= '0' && c <= '9')       value |= (c - '0');
        else if (c >= 'a' && c <= 'f')  value |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  value |= (c - 'A' + 10);
    }
    return value;
}

// Когда у нас есть LogInfo + timestamp, печатаем итог в человеко-читаемом виде:
void printLog(const LogInfo& info, uint64_t timestamp) {
    uint64_t blockNum = parseHexUint64(info.blockHex);
    uint64_t logIndex = parseHexUint64(info.logIndexHex);

    // Текущее системное время в миллисекундах с эпохи
    auto now = std::chrono::system_clock::now();
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    // Блок-таймстемп в миллисекундах
    uint64_t blockMs = timestamp * 1000;

    // Разница (пинг) в миллисекундах
    int64_t ping = static_cast<int64_t>(nowMs) - static_cast<int64_t>(blockMs);

    std::cout << "Событие Swap:\n"
              << "  — Контракт:     " << info.contract   << "\n"
              << "  — Блок:         " << blockNum        << "\n"
              << "  — TxHash:       " << info.txHash     << "\n"
              << "  — logIndex:     " << logIndex        << "\n"
              << "  — sender:       " << info.sender     << "\n"
              << "  — fromOrTo:     " << info.fromOrTo   << "\n"
              << "  — timestamp:    " << timestamp       << "\n"
              << "  — ping (ms):    " << ping            << "\n\n";
}

// Асинхронное чтение из WebSocket, тут парсим JSON-RPC ответы и уведомления
void do_read(
    std::shared_ptr<websocket::stream<tcp::socket>> ws_ptr,
    std::shared_ptr<beast::flat_buffer> buffer_ptr)
{
    ws_ptr->async_read(
        *buffer_ptr,
        [ws_ptr, buffer_ptr](beast::error_code ec, std::size_t)
        {
            if (ec) {
                std::cerr << "Read error: " << ec.message() << "\n";
                return;
            }

            // Считываем весь полученный текст
            std::string text = beast::buffers_to_string(buffer_ptr->data());
            buffer_ptr->consume(buffer_ptr->size());

            // Парсим JSON
            json::value parsed;
            try {
                parsed = json::parse(text);
            }
            catch (const std::exception& e) {
                std::cerr << "JSON parse error: " << e.what() << "\n";
                // Запускаем следующий цикл чтения и выходим
                do_read(ws_ptr, buffer_ptr);
                return;
            }

            auto obj = parsed.as_object();

            // 1) Если это уведомление от «eth_subscription» (новый лог)
            if (obj.if_contains("method") && obj["method"].as_string() == "eth_subscription") {
                auto params = obj["params"].as_object();
                auto result = params["result"].as_object();

                // Собираем детали лога в LogInfo
                LogInfo info;
                info.contract    = result["address"].as_string().c_str();
                info.blockHex    = result["blockNumber"].as_string().c_str();
                info.txHash      = result["transactionHash"].as_string().c_str();
                info.logIndexHex = result["logIndex"].as_string().c_str();

                // «topics» — это массив из 3 элементов:
                // [0] = сигнатура события,
                // [1] = indexed sender,
                // [2] = indexed fromOrTo.
                auto topics = result["topics"].as_array();
                if (topics.size() >= 3) {
                    std::string t1 = topics[1].as_string().c_str();
                    std::string t2 = topics[2].as_string().c_str();
                    // Берём последние 40 символов после «0x»
                    if (t1.size() >= 40) info.sender   = "0x" + t1.substr(t1.size() - 40);
                    if (t2.size() >= 40) info.fromOrTo = "0x" + t2.substr(t2.size() - 40);
                }

                // Генерируем новый requestId для запроса блока
                int     reqId = nextRequestId.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lg(pendingMutex);
                    pendingRequests[reqId] = info;
                }

                // Собираем JSON-RPC-запрос eth_getBlockByNumber
                json::object blockReq;
                blockReq["jsonrpc"] = "2.0";
                blockReq["id"]      = reqId;
                blockReq["method"]  = "eth_getBlockByNumber";
                json::array arr;
                arr.push_back(json::value(info.blockHex));
                arr.push_back(json::value(false));
                blockReq["params"] = arr;

                std::string reqText = json::serialize(blockReq);
                ws_ptr->write(net::buffer(reqText));
            }
            // 2) Если это ответ на «eth_getBlockByNumber» (есть поле «result» с блоком)
            else if (obj.if_contains("id") && obj.if_contains("result")) {
                int respId = obj["id"].to_number<int>();

                LogInfo info;
                {
                    std::lock_guard<std::mutex> lg(pendingMutex);
                    auto it = pendingRequests.find(respId);
                    if (it == pendingRequests.end()) {
                        // Неожиданный id — просто игнорируем
                        do_read(ws_ptr, buffer_ptr);
                        return;
                    }
                    info = it->second;
                    pendingRequests.erase(it);
                }

                // Внутри «result» — объект блока, есть «timestamp» в hex
                auto blockObj = obj["result"].as_object();
                std::string tsHex = blockObj["timestamp"].as_string().c_str();
                uint64_t tsVal = parseHexUint64(tsHex);

                // Выводим полный человеко-читаемый лог
                printLog(info, tsVal);
            }
            // Иначе — игнорируем

            // Запускаем следующий асинхронный read
            do_read(ws_ptr, buffer_ptr);
        });
}

int main() {
    try {
        // Подставьте сюда IP/порт вашей ноды
        std::string const host = "127.0.0.1";
        std::string const port = "8546";

        net::io_context   ioc;
        tcp::resolver     resolver{ioc};
        auto const        endpoints = resolver.resolve(host, port);

        // Открываем WebSocket поверх TCP
        auto ws_ptr = std::make_shared<websocket::stream<tcp::socket>>(ioc.get_executor());
        net::connect(ws_ptr->next_layer(), endpoints);

        // Делаем WebSocket handshake
        ws_ptr->handshake(host + ":" + port, "/");
        std::cout << "Connected to ws://" << host << ":" << port << "\n\n";

        // Шаблон JSON для подписки на логи контракта
        std::string subscribe_msg = R"({
  "jsonrpc": "2.0",
  "id": 1,
  "method": "eth_subscribe",
  "params": [
    "logs",
    {
      "address": "0x98d6E81F14B2278455311738267D6Cf93160be35"
    }
  ]
})";

        // Отправляем запрос на подписку
        ws_ptr->write(net::buffer(subscribe_msg));
        std::cout << "Sent subscription:\n" << subscribe_msg << "\n\n";

        // Запускаем асинхронное чтение
        auto buffer_ptr = std::make_shared<beast::flat_buffer>();
        do_read(ws_ptr, buffer_ptr);

        // Выход из run() только при error или когда процесс завершается
        ioc.run();
    }
    catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
