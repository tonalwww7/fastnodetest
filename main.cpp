// main.cpp

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/json.hpp>
#include <nlohmann/json.hpp>  // для DEXTools-парсинга
#include <openssl/ssl.h>      // для SSL_set_tlsext_host_name
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <chrono>

namespace beast      = boost::beast;
namespace websocket  = beast::websocket;
namespace http       = boost::beast::http;
namespace net        = boost::asio;
namespace ssl        = boost::asio::ssl;
using     tcp        = boost::asio::ip::tcp;
using     json_dextools = nlohmann::json;
namespace json_node  = boost::json;

// --------------------
// Глобальные структуры
// --------------------

struct LogInfo {
    std::string contract;
    std::string blockHex;
    std::string txHash;
    std::string logIndexHex;
    std::string sender;
    std::string fromOrTo;
};

static std::atomic<int> nextRequestId{2};

static std::mutex                         pendingMutex;
static std::unordered_map<int, LogInfo>   pendingRequests;

struct EventInfo {
    uint64_t       blockNum            = 0;
    uint64_t       blockchainTimestamp = 0;
    std::chrono::system_clock::time_point dexSystemTime;
    std::chrono::steady_clock::time_point  dexArrival;
    std::chrono::system_clock::time_point nodeSystemTime;
    std::chrono::steady_clock::time_point  nodeArrival;
    bool           gotDex              = false;
    bool           gotNode             = false;
};

static std::mutex                                  globalMutex;
static std::unordered_map<std::string, EventInfo>  eventsMap;

// --------------------
// Утилиты
// --------------------

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

std::string toStringSys(const std::chrono::system_clock::time_point& tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    std::tm buf;
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&buf, &tt);
#else
    localtime_r(&tt, &buf);
#endif
    char str[64];
    std::strftime(str, sizeof(str), "%F %T", &buf);
    char result[80];
    std::snprintf(result, sizeof(result), "%s.%03lld", str, static_cast<long long>(ms.count()));
    return std::string(result);
}

void printComparison(const std::string& txHash, const EventInfo& info) {
    auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(info.nodeArrival - info.dexArrival).count();
    std::cout << "============================================\n";
    std::cout << "Block:            " << info.blockNum << "\n";
    std::cout << "TxHash:           " << txHash << "\n";
    std::cout << "BlockTimestamp:   " << info.blockchainTimestamp << "\n\n";
    std::cout << "DEXTools Arrival: " << toStringSys(info.dexSystemTime) << "\n";
    std::cout << "Node    Arrival:  " << toStringSys(info.nodeSystemTime) << "\n";
    std::cout << "Delta (node - dex) = " << delta << " ms\n";
    std::cout << "============================================\n\n";
}

// --------------------
// Чтение из DEXTools (SSL WebSocket)
// --------------------
void do_read_dex(
    std::shared_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_ptr,
    std::shared_ptr<beast::flat_buffer>                                 buffer_ptr)
{
    ws_ptr->async_read(
        *buffer_ptr,
        [ws_ptr, buffer_ptr](beast::error_code ec, std::size_t)
        {
            if(ec) {
                std::cerr << "DEX Read error: " << ec.message() << "\n";
                return;
            }
            std::string text = beast::buffers_to_string(buffer_ptr->data());
            buffer_ptr->consume(buffer_ptr->size());
            try {
                auto j = json_dextools::parse(text);
                if(j.contains("result") && j["result"].contains("data")) {
                    auto data = j["result"]["data"];
                    if(data.is_string()) {
                        // Статус «ready» — можно опустить
                    }
                    else if(data.is_object() && data.contains("event")) {
                        std::string event = data["event"].get<std::string>();
                        if(event == "swaps" && data.contains("swaps") && data.contains("id")) {
                            for(const auto& swap : data["swaps"]) {
                                std::string txHash    = swap.value("id", "");
                                uint64_t    blockNum  = swap.value("blockNumber", 0);
                                uint64_t    blkTs     = swap.value("timestamp", 0LL);
                                auto now_sys    = std::chrono::system_clock::now();
                                auto now_steady = std::chrono::steady_clock::now();
                                std::cout << "[DEX] Swap received: txHash=" << txHash
                                          << " block=" << blockNum
                                          << " blkTs=" << blkTs << "\n";
                                {
                                    std::lock_guard<std::mutex> lg(globalMutex);
                                    EventInfo& info = eventsMap[txHash];
                                    info.blockNum            = blockNum;
                                    info.blockchainTimestamp = blkTs;
                                    info.dexSystemTime       = now_sys;
                                    info.dexArrival          = now_steady;
                                    info.gotDex              = true;
                                    if(info.gotNode) {
                                        printComparison(txHash, info);
                                        eventsMap.erase(txHash);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            catch(const std::exception&) {
                // Не JSON — пропускаем
            }
            do_read_dex(ws_ptr, buffer_ptr);
        });
}

// --------------------
// Чтение из приватного нода (Plain WebSocket)
// --------------------
void do_read_node(
    std::shared_ptr<websocket::stream<tcp::socket>> ws_ptr,
    std::shared_ptr<beast::flat_buffer>            buffer_ptr)
{
    ws_ptr->async_read(
        *buffer_ptr,
        [ws_ptr, buffer_ptr](beast::error_code ec, std::size_t)
        {
            if(ec) {
                std::cerr << "Node Read error: " << ec.message() << "\n";
                return;
            }
            std::string text = beast::buffers_to_string(buffer_ptr->data());
            buffer_ptr->consume(buffer_ptr->size());
            json_node::value parsed;
            try {
                parsed = json_node::parse(text);
            }
            catch(const std::exception& e) {
                std::cerr << "[NODE] JSON parse error: " << e.what() << "\n";
                do_read_node(ws_ptr, buffer_ptr);
                return;
            }
            auto obj = parsed.as_object();
            if(obj.if_contains("method") && obj["method"].as_string() == "eth_subscription") {
                auto params = obj["params"].as_object();
                auto result = params["result"].as_object();
                LogInfo info;
                info.contract    = result["address"].as_string().c_str();
                info.blockHex    = result["blockNumber"].as_string().c_str();
                info.txHash      = result["transactionHash"].as_string().c_str();
                info.logIndexHex = result["logIndex"].as_string().c_str();
                auto& topics = result["topics"].as_array();
                if(topics.size() >= 3) {
                    std::string t1 = topics[1].as_string().c_str();
                    std::string t2 = topics[2].as_string().c_str();
                    if(t1.size() >= 40) info.sender   = "0x" + t1.substr(t1.size() - 40);
                    if(t2.size() >= 40) info.fromOrTo = "0x" + t2.substr(t2.size() - 40);
                }
                int reqId = nextRequestId.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lg(pendingMutex);
                    pendingRequests[reqId] = info;
                }
                json_node::object blockReq;
                blockReq["jsonrpc"] = "2.0";
                blockReq["id"]      = reqId;
                blockReq["method"]  = "eth_getBlockByNumber";
                json_node::array arr;
                arr.push_back(json_node::value(info.blockHex));
                arr.push_back(json_node::value(false));
                blockReq["params"] = arr;
                std::string reqText = json_node::serialize(blockReq);
                ws_ptr->write(net::buffer(reqText));
                std::cout << "[NODE] Sent eth_getBlockByNumber for blockHex=" << info.blockHex
                          << " (reqId=" << reqId << ", txHash=" << info.txHash << ")\n";
            }
            else if(obj.if_contains("id") && obj.if_contains("result")) {
                int respId = obj["id"].to_number<int>();
                LogInfo info_copy;
                {
                    std::lock_guard<std::mutex> lg(pendingMutex);
                    auto it = pendingRequests.find(respId);
                    if(it == pendingRequests.end()) {
                        do_read_node(ws_ptr, buffer_ptr);
                        return;
                    }
                    info_copy = it->second;
                    pendingRequests.erase(it);
                }
                auto blockObj = obj["result"].as_object();
                std::string tsHex = blockObj["timestamp"].as_string().c_str();
                uint64_t tsVal = parseHexUint64(tsHex);
                auto now_sys    = std::chrono::system_clock::now();
                auto now_steady = std::chrono::steady_clock::now();
                std::cout << "[NODE] Received block " << info_copy.blockHex
                          << " timestamp=0x" << tsHex
                          << " → " << tsVal
                          << " for txHash=" << info_copy.txHash << "\n";
                {
                    std::lock_guard<std::mutex> lg(globalMutex);
                    auto it_ev = eventsMap.find(info_copy.txHash);
                    if(it_ev == eventsMap.end()) {
                        EventInfo ev;
                        ev.blockNum            = parseHexUint64(info_copy.blockHex);
                        ev.blockchainTimestamp = tsVal;
                        ev.nodeSystemTime      = now_sys;
                        ev.nodeArrival         = now_steady;
                        ev.gotNode             = true;
                        eventsMap[info_copy.txHash] = ev;
                    }
                    else {
                        EventInfo& ev = it_ev->second;
                        ev.blockNum            = parseHexUint64(info_copy.blockHex);
                        ev.blockchainTimestamp = tsVal;
                        ev.nodeSystemTime      = now_sys;
                        ev.nodeArrival         = now_steady;
                        ev.gotNode             = true;
                        if(ev.gotDex) {
                            printComparison(info_copy.txHash, ev);
                            eventsMap.erase(it_ev);
                        }
                    }
                }
            }
            do_read_node(ws_ptr, buffer_ptr);
        });
}

// --------------------
// main()
// --------------------
int main()
{
    try
    {
        //
        // === ПОДКЛЮЧЕНИЕ К DEXTOOLS (SSL WebSocket) С ПРИНУДИТЕЛЬНЫМ IPv4 + Origin ===
        //
        const std::string dexHost = "ws.dextools.io";
        const std::string dexPort = "443";

        net::io_context   ioc;
        tcp::resolver     resolver{ioc};

        auto const dexResults = resolver.resolve(dexHost, dexPort);

        beast::tcp_stream tcpStream{ioc};
        bool connected_v4 = false;
        for (auto const& entry : dexResults) {
            if (entry.endpoint().address().is_v4()) {
                tcpStream.connect(entry.endpoint());
                connected_v4 = true;
                break;
            }
        }
        if (!connected_v4) {
            std::cerr << "Error: не удалось найти IPv4-адрес для " << dexHost << "\n";
            return EXIT_FAILURE;
        }

        ssl::context ctx{ssl::context::tlsv12_client};
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        beast::ssl_stream<beast::tcp_stream> sslStream{std::move(tcpStream), ctx};
        if(!SSL_set_tlsext_host_name(sslStream.native_handle(), dexHost.c_str()))
            std::cerr << "Warning: не удалось установить SNI hostname для DEXTools\n";
        sslStream.handshake(ssl::stream_base::client);

        auto ws_dex = std::make_shared<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            std::move(sslStream)
        );

        // Декоратор устанавливает одновременно User-Agent и Origin
        ws_dex->set_option(websocket::stream_base::decorator(
            [&](websocket::request_type& req)
            {
                req.set(http::field::user_agent,
                        std::string(BOOST_BEAST_VERSION_STRING) + " combinedScript");
                req.set(http::field::origin, "https://dextools.io");
            }));
        ws_dex->handshake(dexHost, "/");
        std::cout << "[DEX] Connected! (wss://" << dexHost << "/)\n";

        std::string subscribe_dex = R"({
  "jsonrpc": "2.0",
  "method": "subscribe",
  "params": {
    "chain": "bsc",
    "channel": "bsc:pair:0x98d6e81f14b2278455311738267d6cf93160be35"
  },
  "id": "bsc"
})";
        ws_dex->write(net::buffer(subscribe_dex));
        std::cout << "[DEX] Sent subscribe:\n" << subscribe_dex << "\n\n";

        auto buffer_dex = std::make_shared<beast::flat_buffer>();
        do_read_dex(ws_dex, buffer_dex);

        //
        // === ПОДКЛЮЧЕНИЕ К НОДУ (Plain WebSocket) ===
        //
        const std::string nodeHost = "localhost";
        const std::string nodePort = "8546";

        auto nodeEndpoints = resolver.resolve(nodeHost, nodePort);
        auto ws_node = std::make_shared<websocket::stream<tcp::socket>>(ioc);
        net::connect(ws_node->next_layer(), nodeEndpoints);

        ws_node->handshake(nodeHost + ":" + nodePort, "/");
        std::cout << "[NODE] Connected! (ws://" << nodeHost << ":" << nodePort << ")\n";

        std::string subscribe_node = R"({
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
        ws_node->write(net::buffer(subscribe_node));
        std::cout << "[NODE] Sent subscription:\n" << subscribe_node << "\n\n";

        auto buffer_node = std::make_shared<beast::flat_buffer>();
        do_read_node(ws_node, buffer_node);

        ioc.run();
    }
    catch(std::exception const& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
