#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <mutex>
#include <chrono>
#include <functional>
#include <string>

using json = nlohmann::json;
namespace beast      = boost::beast;
namespace websocket  = beast::websocket;
namespace asio       = boost::asio;
namespace ssl        = boost::asio::ssl;
using tcp            = asio::ip::tcp;

// Глобальный мьютекс для вывода в консоль
static std::mutex cout_mutex;

/**
 * GateClient: минимальный WebSocket-клиент для Gate.io Futures
 * – авторизация (futures.login)
 * – размещение ордера (futures.order_place)
 * – логирование RTT
 */
class GateClient {
public:
    GateClient(asio::io_context& ctx,
               ssl::context& ssl_ctx,
               const std::string& host,
               const std::string& port,
               const std::string& apiKey,
               const std::string& apiSecret)
        : ioc_(ctx)
        , ssl_ctx_(ssl_ctx)
        , resolver_(asio::make_strand(ioc_))
        , ws_(asio::make_strand(ioc_), ssl_ctx_)
        , host_(host)
        , port_(port)
        , apiKey_(apiKey)
        , apiSecret_(apiSecret)
    {}

    ~GateClient() {
        close();
        if (thread_.joinable())
            thread_.join();
    }

    // Запустить WS-клиент; onLogCallback – callback для логирования в консоль
    void run(std::function<void(const std::string&)> onLogCallback) {
        onLog_ = onLogCallback;
        thread_ = std::thread([this]() {
            resolver_.async_resolve(host_, port_,
                beast::bind_front_handler(&GateClient::onResolve, this));
            ioc_.run();
        });
    }

    // Отправить futures.login
    void sendLogin() {
        auto t0 = std::chrono::high_resolution_clock::now();
        {
            std::lock_guard<std::mutex> lk(timeMutex_);
            loginStart_ = t0;
        }
        uint64_t ts = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()
                      ).count();
        std::string channel = "futures.login";
        std::string event   = "api";
        std::string reqParam = "";

        // Генерируем подпись HMAC_SHA512
        std::string sign = makeSignature(channel, event, reqParam, ts);

        json payload = {
            {"api_key",   apiKey_},
            {"signature", sign},
            {"timestamp", std::to_string(ts)},
            {"req_id",    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()
                             ).count()) + "-login"}
        };

        json req = baseRequest(channel, event);
        req["payload"] = payload;
        sendJson(req);
    }

    // Отправить futures.order_place
    void sendPlaceOrder(const std::string& contract,
                        int64_t size,
                        const std::string& price,
                        const std::string& tif,
                        const std::string& text) {
        auto t0 = std::chrono::high_resolution_clock::now();
        {
            std::lock_guard<std::mutex> lk(timeMutex_);
            orderStart_ = t0;
        }
        std::string channel = "futures.order_place";
        std::string event   = "api";

        json orderParam = {
            {"contract",    contract},
            {"size",        size},
            {"price",       price},
            {"tif",         tif},
            {"text",        text},
            {"iceberg",     0},
            {"close",       false},
            {"reduce_only", false},
            {"stp_act",     "-"}
        };

        json payload = {
            {"req_id",    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()
                             ).count()) + "-order"},
            {"req_param", orderParam}
        };

        json req = baseRequest(channel, event);
        req["payload"] = payload;
        sendJson(req);
    }

    // Закрыть соединение
    void close() {
        shouldClose_ = true;
        if (connected_) {
            beast::error_code ec;
            ws_.close(websocket::close_code::normal, ec);
            if (ec) {
                onLog_("Error closing WS: " + ec.message());
            }
            connected_ = false;
        }
    }

private:
    // Генерация HMAC_SHA512 подписи
    std::string makeSignature(const std::string& channel,
                              const std::string& event,
                              const std::string& reqParam,
                              uint64_t timestamp) {
        std::string signString = event + "\n" + channel + "\n" + reqParam + "\n" + std::to_string(timestamp);
        unsigned char* result = HMAC(
            EVP_sha512(),
            apiSecret_.c_str(), apiSecret_.length(),
            reinterpret_cast<const unsigned char*>(signString.c_str()), signString.length(),
            nullptr, nullptr
        );
        std::ostringstream oss;
        for (unsigned int i = 0; i < 64; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)result[i];
        }
        return oss.str();
    }

    // Beast-коллбеки для асинхронного соединения/чтения/записи
    void onResolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) {
            onLog_("Error resolve: " + ec.message());
            return;
        }
        beast::get_lowest_layer(ws_.next_layer()).async_connect(
            results,
            beast::bind_front_handler(&GateClient::onConnect, this));
    }

    void onConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (ec) {
            onLog_("Error connect: " + ec.message());
            return;
        }
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())) {
            beast::error_code errs{static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()};
            onLog_("Error SNI: " + errs.message());
            return;
        }
        ws_.next_layer().async_handshake(
            ssl::stream_base::client,
            beast::bind_front_handler(&GateClient::onSSLHandshake, this));
    }

    void onSSLHandshake(beast::error_code ec) {
        if (ec) {
            onLog_("Error SSL handshake: " + ec.message());
            return;
        }
        ws_.async_handshake(host_, "/v4/ws/usdt",
            beast::bind_front_handler(&GateClient::onHandshake, this));
    }

    void onHandshake(beast::error_code ec) {
        if (ec) {
            onLog_("Error WS handshake: " + ec.message());
            return;
        }
        connected_ = true;
        onLog_("WebSocket connected");
        ws_.async_read(buffer_,
            beast::bind_front_handler(&GateClient::onRead, this));
    }

    void onRead(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        if (ec) {
            if (ec == websocket::error::closed) {
                onLog_("WebSocket closed");
                return;
            }
            onLog_("Error Read: " + ec.message());
            return;
        }
        std::string msg = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());
        onLog_("[RECV] " + msg);

        // Замер RTT для login/order_place
        try {
            auto j = json::parse(msg);
            if (j.contains("header") && j["header"].contains("channel")) {
                std::string ch = j["header"]["channel"].get<std::string>();
                if (ch == "futures.login") {
                    std::lock_guard<std::mutex> lk(timeMutex_);
                    logTime("Login RTT", loginStart_);
                }
                else if (ch == "futures.order_place") {
                    bool isAck = j.value("ack", false);
                    std::lock_guard<std::mutex> lk(timeMutex_);
                    if (isAck) {
                        logTime("Order send → Ack RTT", orderStart_);
                    } else {
                        logTime("Order placed RTT", orderStart_);
                    }
                }
            }
        } catch (...) {
            // не JSON — игнорируем
        }

        ws_.async_read(buffer_,
            beast::bind_front_handler(&GateClient::onRead, this));
    }

    void onWrite(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        if (ec) {
            onLog_("Error Write: " + ec.message());
        }
    }

    // Отправка JSON-сообщения (потокобезопасно)
    void sendJson(const json& jmsg) {
        std::string s = jmsg.dump();
        std::lock_guard<std::mutex> lk(writeMutex_);
        if (connected_) {
            ws_.text(true);
            ws_.async_write(
                asio::buffer(s),
                beast::bind_front_handler(&GateClient::onWrite, this));
            onLog_("[SEND] " + s);
        } else {
            onLog_("Cannot send, not connected");
        }
    }

    // Базовый шаблон JSON: заполняем time, channel, event
    json baseRequest(const std::string& channel, const std::string& event) {
        json req;
        uint64_t ts = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()
                      ).count();
        req["time"]    = ts;
        req["channel"] = channel;
        req["event"]   = event;
        return req;
    }

    // Замер RTT и лог
    void logTime(const std::string& desc, std::chrono::high_resolution_clock::time_point t0) {
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::ostringstream oss;
        oss << "[" << desc << "] " << ms << " ms";
        onLog_(oss.str());
    }

private:
    asio::io_context&                                          ioc_;
    ssl::context&                                              ssl_ctx_;
    tcp::resolver                                              resolver_;

    using ssl_stream_t = beast::ssl_stream<beast::tcp_stream>;
    websocket::stream<ssl_stream_t>                            ws_;
    beast::flat_buffer                                         buffer_;
    std::thread                                                thread_;
    std::mutex                                                 writeMutex_;
    bool                                                       connected_   = false;
    bool                                                       shouldClose_ = false;
    std::string                                                host_;
    std::string                                                port_;
    std::string                                                apiKey_;
    std::string                                                apiSecret_;
    std::function<void(const std::string&)>                    onLog_;
    std::mutex                                                 timeMutex_;
    std::chrono::high_resolution_clock::time_point              loginStart_;
    std::chrono::high_resolution_clock::time_point              orderStart_;
};

int main() {
    // -------------------------------
    // 1) Настройка Boost.Asio + SSL
    // -------------------------------
    asio::io_context ioc;
    ssl::context    ssl_ctx(ssl::context::tlsv12_client);
    ssl_ctx.set_verify_mode(ssl::verify_none);

    // -------------------------------
    // 2) Запрос API-ключей и хоста у пользователя
    // -------------------------------
    std::string host, port, apiKey, apiSecret;
    std::cout << "Enter Gate.io WS host (e.g. fx-ws.gateio.ws): ";
    std::getline(std::cin, host);
    std::cout << "Enter port (e.g. 443): ";
    std::getline(std::cin, port);
    std::cout << "Enter API Key: ";
    std::getline(std::cin, apiKey);
    std::cout << "Enter API Secret: ";
    std::getline(std::cin, apiSecret);

    // -------------------------------
    // 3) Создаём GateClient
    // -------------------------------
    GateClient client(ioc, ssl_ctx, host, port, apiKey, apiSecret);

    // -------------------------------
    // 4) Запускаем WebSocket-клиент в фоне с логированием в консоль
    // -------------------------------
    client.run([&](const std::string& line) {
        std::lock_guard<std::mutex> lk(cout_mutex);
        std::cout << line << std::endl;
    });

    // Даем секунду на подключение
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // -------------------------------
    // 5) Консольное меню для пользователя
    // -------------------------------
    while (true) {
        std::cout << "\nMenu:\n"
                  << "1) Login\n"
                  << "2) Place Order\n"
                  << "3) Exit\n"
                  << "Choose an option: ";
        int choice;
        if (!(std::cin >> choice)) {
            std::cin.clear();
            std::cin.ignore(10000, '\n');
            continue;
        }

        if (choice == 1) {
            client.sendLogin();
        }
        else if (choice == 2) {
            std::string contract, price, tif, text;
            int64_t size;
            std::cout << "Contract (e.g. BTC_USDT): ";
            std::cin >> contract;
            std::cout << "Size (integer): ";
            std::cin >> size;
            std::cout << "Price (e.g. 30000.00): ";
            std::cin >> price;
            std::cout << "TIF (e.g. gtc): ";
            std::cin >> tif;
            std::cout << "Text field: ";
            std::cin >> text;
            client.sendPlaceOrder(contract, size, price, tif, text);
        }
        else if (choice == 3) {
            break;
        }
        else {
            std::cout << "Invalid option.\n";
        }
    }

    // -------------------------------
    // 6) Завершение работы
    // -------------------------------
    client.close();
    return 0;
}
