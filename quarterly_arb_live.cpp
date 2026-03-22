#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <queue>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <conio.h>
void enable_vt_mode() {
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE)
    return;
  DWORD dwMode = 0;
  if (!GetConsoleMode(hOut, &dwMode))
    return;
  dwMode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
  SetConsoleMode(hOut, dwMode);
}
#else
void enable_vt_mode() {}
#endif

// Dependencies (must be installed via vcpkg/MSYS2/etc)
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/sha.h>

using json = nlohmann::json;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

// =========================================================================
// CONFIGURATION
// =========================================================================
struct Config {
  std::string api_key = "";    // must be set in config.json
  std::string api_secret = ""; // must be set in config.json
  std::string symbol = "BTCUSDT";
  double quantity = 0.0001;
  int use_contract = 0;

  double spot_fee = 0.00075;
  double futures_fee = 0.00045;

  double min_apr_pct = 8.0;
  bool spread_entry_enabled = true;
  double spread_entry_min_usdt = 110.0;
  double spread_entry_min_dte = 20.0;

  double fee_safety_mult = 1.3;
  double exit_pct_of_entry = 0.20;
  double exit_spread_usdt = 8.0;

  int min_hold_bars = 0;
  int close_before_expiry_mins = 120;
  double min_dte = 1.0;

  int leverage = 20;
  int spot_leverage = 10;
  int max_open_positions = 1;

  double print_interval_sec = 0.01;
  bool dry_run = true;
};

// =========================================================================
// GLOBALS & CONSTANTS
// =========================================================================
const std::string SPOT_URL = "https://api.binance.com/api/v3";
const std::string FUT_URL = "https://fapi.binance.com/fapi/v1";
const std::string SPOT_ORDER_URL = SPOT_URL + "/order";
const std::string MARGIN_ORDER_URL =
    "https://api.binance.com/sapi/v1/margin/order";
const std::string FUT_ORDER_URL = FUT_URL + "/order";

Config cfg;


#include <mutex>
#include <atomic>

std::atomic<bool> force_close_trades{false};
std::atomic<bool> force_quit{false};
std::deque<std::string> g_recent_trade_logs;
std::mutex g_log_mtx;
std::atomic<bool> force_ui_update{false};

void add_trade_log_msg(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_log_mtx);
    g_recent_trade_logs.push_back(msg);
    if(g_recent_trade_logs.size() > 6) g_recent_trade_logs.pop_front();
    force_ui_update = true;
}

void keyboard_listener_thread() {
  while (true) {
#ifdef _WIN32
    if (_kbhit()) {
      int ch = _getch();
      if (ch == 'c' || ch == 'C') {
        force_close_trades = true;
      } else if (ch == 'q' || ch == 'Q') {
        force_quit = true;
      }
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

#ifdef _WIN32
BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        force_quit = true;
        std::cout << "\n[!] Shutdown signal detected! Initiating safe closing of open trades...\n";
        // Give the main thread up to 10 seconds to close trades and exit
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return TRUE;
    }
    return FALSE;
}
#endif

// ── Two dedicated cURL handles: one for spot, one for futures REST calls ────
// This lets parallel std::async entry/exit orders genuinely run in parallel.
enum class CurlChan { SPOT, FUT, UTIL }; // UTIL = balance/admin queries
static CURL *g_curl_spot = nullptr;
static CURL *g_curl_fut = nullptr;
static CURL *g_curl_util = nullptr;
static curl_slist *g_headers = nullptr;
static std::mutex g_mtx_spot;
static std::mutex g_mtx_fut;
static std::mutex g_mtx_util;

// ── Boot discovery variables ───────────────────────────────────────────────
std::string fut_sym_boot;
long long global_expiry_ms_boot = 0;

// ── Trade audit log ────────────────────────────────────────────────────────
static std::ofstream g_trade_log;

static size_t WriteCallback(void *contents, size_t size, size_t nmemb,
                            void *userp) {
  ((std::string *)userp)->append((char *)contents, size * nmemb);
  return size * nmemb;
}

// =========================================================================
// HMAC SHA256 & CURL REQUESTS
// =========================================================================
std::string hmac_sha256(const std::string &key, const std::string &data) {
  unsigned int len = EVP_MAX_MD_SIZE;
  unsigned char hash[EVP_MAX_MD_SIZE];
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  thread_local EVP_MAC *mac = nullptr;
  thread_local EVP_MAC_CTX *ctx = nullptr;
  if(!mac) mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
  if(!ctx) ctx = EVP_MAC_CTX_new(mac);
  
  OSSL_PARAM params[2] = {OSSL_PARAM_construct_utf8_string("digest", (char *)"SHA256", 0), OSSL_PARAM_construct_end()};
  EVP_MAC_init(ctx, (const unsigned char *)key.c_str(), key.length(), params);
  EVP_MAC_update(ctx, (const unsigned char *)data.c_str(), data.length());
  EVP_MAC_final(ctx, hash, (size_t *)&len, sizeof(hash));
#else
  thread_local HMAC_CTX *ctx = nullptr;
  if (!ctx) ctx = HMAC_CTX_new();
  HMAC_Init_ex(ctx, key.c_str(), key.length(), EVP_sha256(), NULL);
  HMAC_Update(ctx, (unsigned char *)data.c_str(), data.length());
  HMAC_Final(ctx, hash, &len);
#endif

  std::stringstream ss;
  for (unsigned int i = 0; i < len; i++) {
    ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
  }
  return ss.str();
}

long long get_current_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ── Init one cURL handle with shared options ──────────────────────────────
static CURL *make_curl_handle(const std::string &api_key, const char *ca_path) {
  CURL *h = curl_easy_init();
  if (!h) {
    std::cerr << "[FATAL] curl_easy_init() failed\n";
    std::exit(1);
  }
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPIDLE, 30L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPINTVL, 15L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, g_headers);
  curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
  curl_easy_setopt(h, CURLOPT_PIPEWAIT, 1L);
  curl_easy_setopt(h, CURLOPT_FORBID_REUSE, 0L);
  curl_easy_setopt(h, CURLOPT_FRESH_CONNECT, 0L);
  curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, 300L);
  if (ca_path) {
    curl_easy_setopt(h, CURLOPT_CAINFO, ca_path);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
  } else {
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  return h;
}

void init_curl(const std::string &api_key) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::string hdr = "X-MBX-APIKEY: " + api_key;
  g_headers = curl_slist_append(nullptr, hdr.c_str());

  // Resolve CA bundle
  const char *ca_path = std::getenv("CURL_CA_BUNDLE");
  const char *msys2_bundle = "C:/msys64/usr/ssl/certs/ca-bundle.crt";
  const char *gitfw_bundle =
      "C:/Program Files/Git/mingw64/ssl/certs/ca-bundle.crt";
  auto file_exists = [](const char *p) -> bool {
    std::ifstream f(p);
    return f.good();
  };

  if (ca_path && file_exists(ca_path)) {
    // explicit env var — keep as-is
  } else if (file_exists(msys2_bundle)) {
    ca_path = msys2_bundle;
  } else if (file_exists(gitfw_bundle)) {
    ca_path = gitfw_bundle;
  } else {
    ca_path = nullptr;
  }

  if (ca_path) {
    std::cout << "[+] SSL verification ENABLED  (CA bundle: " << ca_path
              << ")\n";
  } else {
    std::cerr
        << "[FATAL] No CA bundle found. Cannot trade securely. Exiting.\n";
    std::exit(1);
  }

  // Create three independent handles so parallel calls never block each other
  g_curl_spot = make_curl_handle(api_key, ca_path);
  g_curl_fut = make_curl_handle(api_key, ca_path);
  g_curl_util = make_curl_handle(api_key, ca_path);
}

// Route a signed REST call through the correct handle+mutex
json signed_request(const std::string &method, const std::string &url_base,
                    std::string query_string, CurlChan chan = CurlChan::UTIL) {
  long long ts = get_current_ms();
  if (!query_string.empty())
    query_string += "&";
  query_string += "timestamp=" + std::to_string(ts);

  std::string signature = hmac_sha256(cfg.api_secret, query_string);
  std::string full_url =
      url_base + "?" + query_string + "&signature=" + signature;

  CURL *h = (chan == CurlChan::SPOT)  ? g_curl_spot
            : (chan == CurlChan::FUT) ? g_curl_fut
                                      : g_curl_util;
  std::mutex &mtx = (chan == CurlChan::SPOT)  ? g_mtx_spot
                    : (chan == CurlChan::FUT) ? g_mtx_fut
                                              : g_mtx_util;

  std::string readBuffer;
  std::lock_guard<std::mutex> lk(mtx); // only locks its own handle

  curl_easy_setopt(h, CURLOPT_URL, full_url.c_str());
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &readBuffer);

  if (method == "POST") {
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, 0L);
  } else {
    curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
  }

  CURLcode res = curl_easy_perform(h);
  if (res != CURLE_OK && readBuffer.empty()) {
    readBuffer = R"({"error": "cURL failure: )" +
                 std::string(curl_easy_strerror(res)) + R"("})";
  }

  try {
    return json::parse(readBuffer);
  } catch (...) {
    return json::parse(
        R"({"parse_error": "failed to parse json", "raw_buffer": ")" +
        readBuffer + R"("})");
  }
}

void warmup_connections() {
    signed_request("GET", "https://api.binance.com/api/v3/time", "", CurlChan::SPOT);
    signed_request("GET", "https://fapi.binance.com/fapi/v1/time", "", CurlChan::FUT);
    signed_request("GET", "https://api.binance.com/api/v3/time", "", CurlChan::UTIL);
    std::cout << "[+] Connections warmed up.\n";
}

// =========================================================================
// ACCOUNT BALANCES POLLER
// =========================================================================
std::atomic<double> bal_spot_usdt{0};
std::atomic<double> bal_spot_btc{0};
std::atomic<double> bal_fut_usdt{0};
std::atomic<double> max_borrow_usdt{0};
std::atomic<double> max_borrow_btc{0};

void balance_updater_thread() {
  while (true) {
    if (!cfg.api_key.empty() && cfg.api_key != "YOUR_API_KEY") {
      json m_res = signed_request(
          "GET", "https://api.binance.com/sapi/v1/margin/isolated/account", "");
      if (m_res.contains("assets")) {
        for (auto &p : m_res["assets"]) {
          if (p.contains("symbol") && p["symbol"] == cfg.symbol) {
            bal_spot_btc = std::stod(p["baseAsset"]["free"].get<std::string>());
            bal_spot_usdt = std::stod(p["quoteAsset"]["free"].get<std::string>());
          }
        }
      }
      
      std::string sym_base = cfg.symbol.length() > 3 ? cfg.symbol.substr(0, cfg.symbol.length() - 4) : "BTC";
      json b_usdt = signed_request("GET", "https://api.binance.com/sapi/v1/margin/maxBorrowable", "asset=USDT&isolatedSymbol=" + cfg.symbol);
      if(b_usdt.contains("amount")) max_borrow_usdt = std::stod(b_usdt["amount"].get<std::string>());
      
      json b_btc = signed_request("GET", "https://api.binance.com/sapi/v1/margin/maxBorrowable", "asset=" + sym_base + "&isolatedSymbol=" + cfg.symbol);
      if(b_btc.contains("amount")) max_borrow_btc = std::stod(b_btc["amount"].get<std::string>());

      json f_res = signed_request("GET", "https://fapi.binance.com/fapi/v2/account", "");
      if (f_res.contains("assets")) {
        for (auto &a : f_res["assets"]) {
          std::string ast = a["asset"];
          if (ast == "USDT")
            bal_fut_usdt = std::stod(a["availableBalance"].get<std::string>());
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(15));
  }
}

// =========================================================================
// WEBSOCKET MANAGER
// =========================================================================
struct Quote {
  double bid = 0.0, ask = 0.0;
  long long event_time = 0, recv_time = 0;
};

class BinanceWebSocketManager {
public:
  std::mutex spot_mtx, fut_mtx;
  Quote spot_quote{}, fut_quote{};
  std::atomic<long long> last_spot_time{0};
  std::atomic<long long> last_fut_time{0};

  BinanceWebSocketManager(const std::string &s_sym, const std::string &f_sym) {
    spot_symbol = s_sym;
    fut_symbol = f_sym;
    for (char &c : spot_symbol)
      c = std::tolower(c);
    for (char &c : fut_symbol)
      c = std::tolower(c);
  }

  void start() {
    std::thread([this]() {
      run_stream("stream.binance.com", "9443",
                 "/ws/" + spot_symbol + "@bookTicker", true);
    }).detach();
    std::thread([this]() {
      run_stream("fstream.binance.com", "443",
                 "/ws/" + fut_symbol + "@bookTicker", false);
    }).detach();
  }

  Quote get_spot_quote() {
    std::lock_guard<std::mutex> lk(spot_mtx);
    return spot_quote;
  }
  Quote get_fut_quote() {
    std::lock_guard<std::mutex> lk(fut_mtx);
    return fut_quote;
  }

private:
  std::string spot_symbol, fut_symbol;

  void run_stream(const std::string &host, const std::string &port,
                  const std::string &target, bool is_spot) {
    while (true) {
      try {
        asio::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};

        tcp::resolver resolver{ioc};
        websocket::stream<ssl::stream<tcp::socket>> ws{ioc, ctx};

        auto const results = resolver.resolve(host, port);
        auto ep = asio::connect(ws.next_layer().next_layer(), results);

        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(),
                                      host.c_str())) {
          continue;
        }

        ws.next_layer().handshake(ssl::stream_base::client);
        ws.set_option(
            websocket::stream_base::decorator([](websocket::request_type &req) {
              req.set(beast::http::field::user_agent, "C++ Binance Bot");
            }));

        ws.handshake(host + ":" + std::to_string(ep.port()), target);
        beast::flat_buffer buffer;
        while (true) {
          ws.read(buffer);
          std::string msg = beast::buffers_to_string(buffer.data());
          buffer.consume(buffer.size());

          auto j = json::parse(msg, nullptr, false);
          if (!j.is_discarded() && j.contains("b") && j.contains("a")) {
            double bid = std::stod(j["b"].get<std::string>());
            double ask = std::stod(j["a"].get<std::string>());
            long long evt_time = 0;
            if (j.contains("E"))
              evt_time = j["E"].get<long long>();
            else if (j.contains("T"))
              evt_time = j["T"].get<long long>();

            if (is_spot) {
              if (evt_time > 0)
                last_spot_time = evt_time;
              std::lock_guard<std::mutex> lk(spot_mtx);
              spot_quote = {bid, ask, evt_time, get_current_ms()};
            } else {
              if (evt_time > 0)
                last_fut_time = evt_time;
              std::lock_guard<std::mutex> lk(fut_mtx);
              fut_quote = {bid, ask, evt_time, get_current_ms()};
            }
          }
        }
      } catch (const std::exception &e) {
        std::cerr << "  [WS] " << (is_spot ? "SPOT" : "FUT")
                  << " dropped: " << e.what() << " — reconnecting NOW\n";
      } catch (...) {
        std::cerr << "  [WS] " << (is_spot ? "SPOT" : "FUT")
                  << " dropped — reconnecting NOW\n";
      }
    }
  }
};

// ── Forward declarations needed by WsOrderSession & WS wrappers ──────────
std::string fmt_qty(double q);
double get_isolated_margin_base_free();
std::pair<bool,double> place_spot_order(const std::string&, const std::string&, double);
std::pair<bool,double> place_fut_order(const std::string&, const std::string&, bool, double);
// ─────────────────────────────────────────────────────────────────────────

extern std::atomic<bool> force_quit;

// =========================================================================
// WS ORDER SESSION  (Binance WebSocket Order API — 2023)
// Spot/Margin endpoint : wss://ws-api.binance.com:443/ws-api/v3
// Futures endpoint     : wss://ws-fapi.binance.com:443/ws-fapi/v1
// =========================================================================
#include <condition_variable>
class WsOrderSession {
public:
  WsOrderSession(const std::string& host, const std::string& port,
                 const std::string& target, bool is_futures)
      : host_(host), port_(port), target_(target), is_futures_(is_futures) {}

  // Connect (blocking) in a background thread that also handles reconnects
  void start() {
    std::thread([this]() { run_loop(); }).detach();
  }

  bool ready() const { return connected_.load(); }

  // Send a signed order.place request and wait up to 2 s for the reply.
  // Returns the parsed JSON response or {"error":"timeout"}.
  json send_order(json params) {
    if (!connected_.load()) return {{"error","not connected"}};

    // Generate a unique request id
    static std::atomic<uint64_t> seq{1};
    std::string id = "o" + std::to_string(seq++);

    // Build and sign
    long long ts = get_current_ms();
    params["timestamp"] = ts;
    params["apiKey"]    = cfg.api_key;

    // Build canonical query string for signing
    std::string qs;
    for (auto& [k, v] : params.items()) {
      if (!qs.empty()) qs += '&';
      qs += k + '=';
      if (v.is_string())        qs += v.get<std::string>();
      else if (v.is_number_integer()) qs += std::to_string(v.get<long long>());
      else if (v.is_number_float())   qs += fmt_qty(v.get<double>());
      else qs += v.dump();
    }
    params["signature"] = hmac_sha256(cfg.api_secret, qs);

    json frame;
    frame["id"]     = id;
    frame["method"] = is_futures_ ? "order.place" : "order.place";
    frame["params"] = params;
    std::string txt = frame.dump();

    // Register pending slot
    {
      std::lock_guard<std::mutex> lk(pending_mtx_);
      pending_[id] = json{};
    }

    // Write — guarded by send mutex so parallel orders don't interleave frames
    {
      std::lock_guard<std::mutex> lk(send_mtx_);
      try {
        ws_->write(asio::buffer(txt));
      } catch (...) {
        connected_ = false;
        std::lock_guard<std::mutex> lk2(pending_mtx_);
        pending_.erase(id);
        return {{"error","write failed"}};
      }
    }

    // Wait for response
    std::unique_lock<std::mutex> lk(pending_mtx_);
    bool ok = pending_cv_.wait_for(lk, std::chrono::seconds(2),
                                   [&]{ return !pending_[id].empty(); });
    if (!ok) {
      pending_.erase(id);
      return {{"error","timeout"}};
    }
    json result = std::move(pending_[id]);
    pending_.erase(id);
    return result;
  }

private:
  std::string host_, port_, target_;
  bool is_futures_;
  std::atomic<bool> connected_{false};
  std::mutex send_mtx_;

  // pending-reply map
  std::mutex pending_mtx_;
  std::condition_variable pending_cv_;
  std::map<std::string, json> pending_;

  // live WS handle (replaced on reconnect)
  using WsStream = websocket::stream<ssl::stream<tcp::socket>>;
  std::unique_ptr<asio::io_context>  ioc_;
  std::unique_ptr<ssl::context>      tls_;
  std::unique_ptr<WsStream>          ws_;

  void run_loop() {
    while (!force_quit.load()) {
      try {
        connect_and_serve();
      } catch (const std::exception& e) {
        if (force_quit.load()) break;
        std::cerr << "  [WS-ORD] " << host_ << " dropped: "
                  << e.what() << " — reconnecting in 2 s\n";
      } catch (...) {
        if (force_quit.load()) break;
        std::cerr << "  [WS-ORD] " << host_ << " dropped — reconnecting in 2 s\n";
      }
      if (force_quit.load()) break;
      connected_ = false;
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }

  void connect_and_serve() {
    ioc_ = std::make_unique<asio::io_context>();
    tls_ = std::make_unique<ssl::context>(ssl::context::tlsv12_client);
    ws_  = std::make_unique<WsStream>(*ioc_, *tls_);

    tcp::resolver resolver{*ioc_};
    auto ep = asio::connect(ws_->next_layer().next_layer(),
                            resolver.resolve(host_, port_));

    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(),
                                  host_.c_str()))
      throw std::runtime_error("SNI failed");

    ws_->next_layer().handshake(ssl::stream_base::client);
    ws_->set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) {
          req.set(beast::http::field::user_agent, "C++ Binance WS Order Bot");
        }));
    ws_->handshake(host_ + ":" + std::to_string(ep.port()), target_);
    connected_ = true;
    std::cout << "  [+] WS Order session connected: " << host_ << target_ << "\n";

    beast::flat_buffer buf;
    while (true) {
      buf.consume(buf.size());
      ws_->read(buf);
      std::string msg = beast::buffers_to_string(buf.data());
      auto j = json::parse(msg, nullptr, false);
      if (j.is_discarded() || !j.contains("id")) continue;

      std::string rid = j["id"].get<std::string>();
      // Extract the meaningful payload
      json result = j.contains("result") ? j["result"]
                  : j.contains("error")  ? j["error"]
                  : j;

      std::lock_guard<std::mutex> lk(pending_mtx_);
      if (pending_.count(rid)) {
        pending_[rid] = std::move(result);
        pending_cv_.notify_all();
      }
    }
  }
};

// Global WS Order session for Futures — connected during startup
std::unique_ptr<WsOrderSession> g_ws_fut;

void init_ws_order_sessions() {
  g_ws_fut  = std::make_unique<WsOrderSession>(
      "ws-fapi.binance.com", "443", "/ws-fapi/v1", true);
  g_ws_fut->start();
  // Give connection ~1 s to complete handshake
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

// =========================================================================
// WS ORDER WRAPPERS  (call WS first, fall back to REST silently)
// =========================================================================

// Helper: parse avgPrice / fills from a WS response (same shape as REST)
static double ws_fill_price(const json& resp, double qty) {
  if (resp.contains("avgPrice")) {
    double p = std::stod(resp["avgPrice"].get<std::string>());
    if (p > 0) return p;
  }
  if (resp.contains("fills") && resp["fills"].is_array()) {
    double total = 0, vol = 0;
    for (auto& f : resp["fills"]) {
      double p = std::stod(f["price"].get<std::string>());
      double q = std::stod(f["qty"].get<std::string>());
      total += p * q; vol += q;
    }
    if (vol > 0) return total / vol;
  }
  return 0.0;
}



std::pair<bool,double> place_fut_order_ws(const std::string& fut_symbol,
                                          const std::string& side,
                                          bool reduce_only,
                                          double price_hint) {
  if (!cfg.dry_run && g_ws_fut && g_ws_fut->ready()) {
    json params;
    params["symbol"]     = fut_symbol;
    params["side"]       = side;
    params["type"]       = "MARKET";
    params["quantity"]   = fmt_qty(cfg.quantity);
    params["reduceOnly"] = reduce_only ? "true" : "false";

    json resp = g_ws_fut->send_order(params);

    if (resp.contains("error") || resp.contains("code")) {
      std::cout << "  \033[1;33m[WS FUT fallback]\033[0m " << resp.dump() << "\n";
      // Fall through to REST
    } else {
      double fill = ws_fill_price(resp, cfg.quantity);
      return {true, fill > 0 ? fill : price_hint};
    }
  }
  // REST fallback
  return place_fut_order(fut_symbol, side, reduce_only, price_hint);
}

// =========================================================================
// TRADING EXECUTION ENGINE
// =========================================================================
struct SpreadMetrics {
  double spread = 0.0, spread_rate = 0.0, apr = 0.0, dte = 0.0;
};

struct Trade {
  std::string direction;
  std::string entry_mode;
  double entry_spot = 0.0;
  double entry_fut = 0.0;
  double entry_spread = 0.0;
  double entry_fees = 0.0;
  int held_bars = 0;
};

SpreadMetrics compute_spread_metrics(double spot_p, double fut_p,
                                     long long expiry_ms) {
  SpreadMetrics m;
  m.spread = fut_p - spot_p;
  m.spread_rate = (m.spread / spot_p) * 100.0;
  double now_sec = get_current_ms() / 1000.0;
  double exp_sec = expiry_ms / 1000.0;
  m.dte = (exp_sec - now_sec) / 86400.0;
  m.apr = (m.dte > 0) ? (m.spread_rate * (365.0 / m.dte)) : 0.0;
  return m;
}

double get_fill_price(const json &resp, double request_qty) {
  if (resp.contains("fills") && resp["fills"].is_array() &&
      !resp["fills"].empty()) {
    double total_qty = 0, total_vol = 0;
    for (auto &f : resp["fills"]) {
      double f_qty = std::stod(f["qty"].get<std::string>());
      double f_px = std::stod(f["price"].get<std::string>());
      total_qty += f_qty;
      total_vol += f_qty * f_px;
    }
    return total_qty > 0 ? (total_vol / total_qty) : 0.0;
  } else if (resp.contains("avgPrice")) {
    return std::stod(resp["avgPrice"].get<std::string>());
  } else if (resp.contains("price")) {
    return std::stod(resp["price"].get<std::string>());
  }
  return 0.0;
}

std::string fmt_qty(double q) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(6) << q;
  std::string s = ss.str();
  s.erase(s.find_last_not_of('0') + 1, std::string::npos);
  if (s.back() == '.') s.pop_back();
  return s;
}

double get_isolated_margin_base_free() {
  json margin_data = signed_request(
      "GET", "https://api.binance.com/sapi/v1/margin/isolated/account", "", CurlChan::UTIL);
  if (margin_data.contains("assets")) {
    for (auto &p : margin_data["assets"]) {
      if (p.contains("symbol") && p["symbol"] == cfg.symbol) {
        return std::stod(p["baseAsset"]["free"].get<std::string>());
      }
    }
  }
  return 0.0;
}

std::pair<bool, double> place_spot_order(const std::string &side,
                                         const std::string &side_effect,
                                         double price_hint) {
  if (cfg.dry_run)
    return {true, price_hint};
    
  double qty = cfg.quantity;
  if (side == "SELL" && side_effect == "AUTO_REPAY") {
    double free_base = get_isolated_margin_base_free();
    if (free_base > 0 && free_base < qty) {
      double adjusted = std::floor(free_base * 100000.0) / 100000.0;
      std::cout << "\n  \033[1;33m[i] Adjusting SELL qty from " << qty
                << " to " << adjusted << " (fee deducted).\033[0m\n";
      qty = adjusted;
    }
  }

  std::string qs = "symbol=" + cfg.symbol + "&side=" + side +
                   "&type=MARKET&quantity=" + fmt_qty(qty) +
                   "&isIsolated=TRUE&sideEffectType=" + side_effect;
  json resp =
      signed_request("POST", MARGIN_ORDER_URL, qs, CurlChan::SPOT);
  if (resp.contains("code")) {
    std::cout << "\n  \033[1;31m[SPOT API ERROR]\033[0m " << resp.dump() << "\n";
    return {false, 0.0};
  }
  return {true, get_fill_price(resp, qty)};
}

std::pair<bool, double> place_fut_order(const std::string &fut_symbol,
                                        const std::string &side,
                                        bool reduce_only, double price_hint) {
  if (cfg.dry_run)
    return {true, price_hint};
    
  std::string qs = "symbol=" + fut_symbol + "&side=" + side +
                   "&type=MARKET&quantity=" + fmt_qty(cfg.quantity) +
                   "&reduceOnly=" + (reduce_only ? "true" : "false");
  json resp = signed_request("POST", FUT_ORDER_URL, qs, CurlChan::FUT);
  if (resp.contains("code")) {
    std::cout << "\n  \033[1;31m[FUT API ERROR]\033[0m " << resp.dump() << "\n";
    return {false, 0.0};
  }
  double fill = get_fill_price(resp, cfg.quantity);
  if (fill == 0.0 && resp.contains("orderId")) {
    for (int i = 0; i < 5 && fill == 0.0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      json q_resp = signed_request("GET", FUT_ORDER_URL,
                                   "symbol=" + fut_symbol +
                                       "&orderId=" + resp["orderId"].dump(),
                                   CurlChan::FUT);
      fill = get_fill_price(q_resp, cfg.quantity);
    }
  }
  return {true, fill};
}

// ── Write one line to the audit log (thread-safe) ──────────────────────────
void log_trade(const std::string &line) {
  if (g_trade_log.is_open()) {
    g_trade_log << line << "\n";
    g_trade_log.flush();
  }
}

std::string get_time_fmt() {
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;
  auto timer = std::chrono::system_clock::to_time_t(now);
  std::tm bt = *std::localtime(&timer);
  std::ostringstream oss;
  oss << std::put_time(&bt, "%H:%M:%S") << '.' << std::setfill('0')
      << std::setw(3) << ms.count();
  return oss.str();
}

void load_config() {
  std::ifstream f("config.json");
  if (!f.is_open())
    f.open("../config.json");
  if (f.is_open()) {
    try {
      json j = json::parse(f);
      if (j.contains("api_key"))
        cfg.api_key = j["api_key"];
      if (j.contains("api_secret"))
        cfg.api_secret = j["api_secret"];
      if (j.contains("symbol"))
        cfg.symbol = j["symbol"];
      if (j.contains("quantity"))
        cfg.quantity = j["quantity"];
      if (j.contains("use_contract"))
        cfg.use_contract = j["use_contract"];
      if (j.contains("spot_fee"))
        cfg.spot_fee = j["spot_fee"];
      if (j.contains("futures_fee"))
        cfg.futures_fee = j["futures_fee"];
      if (j.contains("min_apr_pct"))
        cfg.min_apr_pct = j["min_apr_pct"];
      if (j.contains("spread_entry_enabled"))
        cfg.spread_entry_enabled = j["spread_entry_enabled"];
      if (j.contains("spread_entry_min_usdt"))
        cfg.spread_entry_min_usdt = j["spread_entry_min_usdt"];
      if (j.contains("spread_entry_min_dte"))
        cfg.spread_entry_min_dte = j["spread_entry_min_dte"];
      if (j.contains("fee_safety_mult"))
        cfg.fee_safety_mult = j["fee_safety_mult"];
      if (j.contains("exit_pct_of_entry"))
        cfg.exit_pct_of_entry = j["exit_pct_of_entry"];
      if (j.contains("exit_spread_usdt"))
        cfg.exit_spread_usdt = j["exit_spread_usdt"];
      if (j.contains("min_hold_bars"))
        cfg.min_hold_bars = j["min_hold_bars"];
      if (j.contains("close_before_expiry_mins"))
        cfg.close_before_expiry_mins = j["close_before_expiry_mins"];
      if (j.contains("min_dte"))
        cfg.min_dte = j["min_dte"];
      if (j.contains("leverage"))
        cfg.leverage = j["leverage"];
      if (j.contains("spot_leverage"))
        cfg.spot_leverage = j["spot_leverage"];
      if (j.contains("max_open_positions"))
        cfg.max_open_positions = j["max_open_positions"];
      if (j.contains("print_interval_sec"))
        cfg.print_interval_sec = j["print_interval_sec"];
      if (j.contains("dry_run"))
        cfg.dry_run = j["dry_run"];
    } catch (const std::exception &e) {
      std::cerr << "[WARN] config.json parse error: " << e.what() << "\n";
    }
  }
}

void draw_dashboard(double spot_p, double fut_p, const SpreadMetrics &m,
                    long long ping, int trades, double pnl, bool active,
                    const Trade &t, const std::string &fut_symbol) {
  std::ostringstream out;
  out << "\033[s"; // Save cursor
  out << "\033[H"; // Move to top-left

  out << "\033[1;36m==========================================================="
         "=====================================\033[0m\n";
  out << "  \033[1;37m[ QUANTITATIVE ARBITRAGE ENGINE ]\033[0m        MODE: "
      << (cfg.dry_run ? "\033[1;33mPAPER (DRY RUN)\033[0m "
                      : "\033[1;31mLIVE (DANGER!)\033[0m ")
      << "  STATUS: \033[1;32mLIVE\033[0m\033[K\n";
  out << "\033[1;36m==========================================================="
         "=====================================\033[0m\n";

  std::string base_coin = cfg.symbol.length() > 3
                              ? cfg.symbol.substr(0, cfg.symbol.length() - 4)
                              : "BTC";

  out << "  PAIR        : " << std::left << std::setw(32)
      << (cfg.symbol + " | " + fut_symbol) << "  PING  : " << ping
      << "ms\033[K\n";
  out << "  VOLUME      : " << cfg.quantity << " " << std::left << std::setw(25)
      << base_coin << "  DTE   : " << std::fixed << std::setprecision(1)
      << m.dte << " Days\033[K\n";
  out << "\033[1;30m-----------------------------------------------------------"
         "-------------------------------------\033[0m\n";

  out << "  \033[1;33m[ LIVE MARKET MATRIX ]\033[0m                            "
         "\033[1;36m[ OPEN POSITION ]\033[0m\033[K\n";
  out << "  SPOT        : $" << std::left << std::setw(32) << std::fixed
      << std::setprecision(2) << spot_p << "DIRECTION   : ";
  if (active)
    out << t.direction << "\033[K\n";
  else
    out << "\033[1;30mNONE\033[0m\033[K\n";

  out << "  FUTURES     : $" << std::left << std::setw(32) << fut_p
      << "ENTRY SVD   : ";
  if (active)
    out << std::showpos << t.entry_spread << std::noshowpos << "\033[K\n";
  else
    out << "$0.00\033[K\n";

  double live_net = 0;
  if (active) {
    double gross = (t.direction == "long_spot_short_fut")
                       ? ((spot_p - t.entry_spot) * cfg.quantity +
                          (t.entry_fut - fut_p) * cfg.quantity)
                       : ((t.entry_spot - spot_p) * cfg.quantity +
                          (fut_p - t.entry_fut) * cfg.quantity);
    double fee_now = spot_p * cfg.quantity * cfg.spot_fee +
                     fut_p * cfg.quantity * cfg.futures_fee;
    live_net = gross - t.entry_fees - fee_now;
  }

  out << "  SPREAD      : " << (m.spread > 0 ? "\033[1;32m+" : "\033[1;31m")
      << "$" << std::setprecision(2) << m.spread << "\033[0m";
  out << std::string(34 - std::to_string(m.spread).length(), ' ')
      << "ACTUAL PNL  : ";
  if (active)
    out << (live_net > 0 ? "\033[1;32m+" : "\033[1;31m") << "$" << live_net
        << "\033[0m\033[K\n";
  else
    out << "$0.00\033[K\n";

  out << "  APR YIELD   : "
      << (m.apr >= cfg.min_apr_pct ? "\033[1;32m" : "\033[1;37m") << m.apr
      << "%\033[0m";
  out << std::string(34 - std::to_string(m.apr).length(), ' ')
      << "HOLD BARS   : " << (active ? t.held_bars : 0) << "\033[K\n";

  out << "\033[1;30m-----------------------------------------------------------"
         "-------------------------------------\033[0m\n";
  out << "  \033[1;35m[ ACCOUNT & RISK METRICS ]\033[0m                        "
         "\033[1;35m[ SESSION TOTALS ]\033[0m\033[K\n";

  out << "  USDT        : S: " << std::fixed << std::setprecision(2)
      << bal_spot_usdt.load() << " | F: " << std::left << std::setw(12)
      << bal_fut_usdt.load() << "      TOTAL PNL   : "
      << (pnl > 0 ? "\033[1;32m+" : (pnl < 0 ? "\033[1;31m" : "\033[1;37m"))
      << "$" << pnl << "\033[0m\033[K\n";

  out << "  " << std::left << std::setw(12) << base_coin
      << ": S: " << std::fixed << std::setprecision(5) << std::left
      << std::setw(21) << bal_spot_btc.load()
      << "       TRADES      : " << trades << " Completed\033[K\n";

  double cap_s = (spot_p * cfg.quantity) / cfg.spot_leverage;
  double cap_f = (fut_p * cfg.quantity) / cfg.leverage;
  double rnd_fee = 2.0 * (spot_p * cfg.quantity * cfg.spot_fee +
                          fut_p * cfg.quantity * cfg.futures_fee);

  out << "  MARGIN RQ   : $" << std::fixed << std::setprecision(2) << cap_s
      << " (S) + $" << std::left << std::setw(11) << cap_f << "(F)"
      << " ROUND FEES  : $" << std::fixed << std::setprecision(3) << rnd_fee
      << "\033[K\n";

  out << "\033[1;36m==========================================================="
         "=====================================\033[0m\n";

  out << "  \033[1;35m[ RECENT TRADE LOGS ]\033[0m             \033[1;32mPress 'C' to Close Trade | 'Q' to Quit Bot\033[0m\033[K\n";
  {
      std::lock_guard<std::mutex> lock(g_log_mtx);
      for (size_t i = 0; i < 6; ++i) {
         if (i < g_recent_trade_logs.size()) {
             out << g_recent_trade_logs[i] << "\033[K\n";
         } else {
             out << "  \033[K\n";
         }
      }
  }
  out << "\033[1;36m==========================================================="
         "=====================================\033[0m\n";

  out << "\033[u"; // Restore cursor
  std::cout << out.str() << std::flush;
}

void display_boot_summary() {
  if (cfg.api_key == "YOUR_API_KEY")
    return;
  std::cout << "==========================================================\n";
  std::cout << "  ACCOUNT BALANCES\n";
  std::cout << "==========================================================\n";

  json spot_data =
      signed_request("GET", "https://api.binance.com/api/v3/account", "");
  std::cout << "  [SPOT WALLET]\n";
  if (spot_data.contains("balances")) {
    for (auto &b : spot_data["balances"]) {
      double free = std::stod(b["free"].get<std::string>());
      double locked = std::stod(b["locked"].get<std::string>());
      if (free + locked > 0.0001) {
        std::cout << "    " << std::left << std::setw(10)
                  << b["asset"].get<std::string>() << " : " << std::fixed
                  << std::setprecision(6) << (free + locked) << "\n";
      }
    }
  } else {
    std::cout << "    [!] Spot API Error: " << spot_data.dump() << "\n";
  }

  std::cout << "\n  [ISOLATED MARGIN WALLET]  leverage=" << cfg.spot_leverage
            << "x\n";
  json margin_data = signed_request(
      "GET", "https://api.binance.com/sapi/v1/margin/isolated/account", "");
  if (margin_data.contains("assets")) {
    for (auto &p : margin_data["assets"]) {
      std::string sym = p["symbol"];
      double b_free = std::stod(p["baseAsset"]["free"].get<std::string>());
      double b_bor = std::stod(p["baseAsset"]["borrowed"].get<std::string>());
      double q_free = std::stod(p["quoteAsset"]["free"].get<std::string>());
      double q_bor = std::stod(p["quoteAsset"]["borrowed"].get<std::string>());
      std::string ratio = p.contains("marginRatio")
                              ? p["marginRatio"].get<std::string>()
                              : "N/A";

      std::cout << "    " << std::left << std::setw(12) << sym
                << "  BTC=" << std::fixed << std::setprecision(6) << b_free
                << " (bor=" << b_bor << ")  "
                << "USDT=" << std::setprecision(2) << q_free
                << " (bor=" << q_bor << ")  ratio=" << ratio << "\n";

      if (sym == cfg.symbol) {
        bal_spot_btc = b_free;
        bal_spot_usdt = q_free;
      }
    }
  } else {
    std::cout << "    [!] Margin API Error: " << margin_data.dump() << "\n";
  }

  std::cout << "\n  [FUTURES WALLET]  leverage=" << cfg.leverage << "x\n";
  json fut_data =
      signed_request("GET", "https://fapi.binance.com/fapi/v2/account", "");
  if (fut_data.contains("assets")) {
    double unpnl = 0;
    for (auto &a : fut_data["assets"]) {
      double bal = std::stod(a["walletBalance"].get<std::string>());
      unpnl += std::stod(a["unrealizedProfit"].get<std::string>());
      std::string ast = a["asset"];
      if (bal > 0.0001 || ast == "USDT" || ast == "BNB" || ast == "BUSD") {
        std::cout << "    " << std::left << std::setw(10) << ast << " : "
                  << std::fixed << std::setprecision(6) << bal << "\n";
      }
      if (ast == "USDT")
        bal_fut_usdt = bal;
    }
    std::cout << "    UnrealizedPnL : $" << std::fixed << std::setprecision(4)
              << unpnl << "\n";
  } else {
    std::cout << "    [!] Futures API Error: " << fut_data.dump() << "\n";
  }
  std::cout << "==========================================================\n\n";

  // Auto-discover the nearest quarterly futures symbol (e.g. BTCUSDT_260627)
  // by querying /fapi/v1/exchangeInfo and picking the first CURRENT_QUARTER
  // or NEXT_QUARTER delivery-type contract for the configured base pair.
  {
    try {
      json ei =
          signed_request("GET", "https://fapi.binance.com/fapi/v1/exchangeInfo",
                         "", CurlChan::UTIL);
      // Collect quarterly contracts in stable order: CURRENT first, NEXT
      // second. Index with cfg.use_contract (0 = current quarter, 1 = next
      // quarter).
      std::pair<std::string, long long> cur_c = {"", 0};
      std::pair<std::string, long long> next_c = {"", 0};
      for (auto &s : ei["symbols"]) {
        std::string ct = s.value("contractType", "");
        std::string sym = s.value("symbol", "");
        std::string st = s.value("status", "");
        long long d_date = s.value("deliveryDate", 0LL);
        if (st == "TRADING" && sym.rfind(cfg.symbol + "_", 0) == 0) {
          if (ct == "CURRENT_QUARTER" && cur_c.first.empty())
            cur_c = {sym, d_date};
          if (ct == "NEXT_QUARTER" && next_c.first.empty())
            next_c = {sym, d_date};
        }
      }
      std::vector<std::pair<std::string, long long>> contracts;
      if (!cur_c.first.empty())
        contracts.push_back(cur_c);
      if (!next_c.first.empty())
        contracts.push_back(next_c);

      std::pair<std::string, long long> selected = {"", 0};
      if ((size_t)cfg.use_contract < contracts.size())
        selected = contracts[cfg.use_contract];
      else if (!contracts.empty())
        selected = contracts[0]; // fallback

      fut_sym_boot = selected.first;
      global_expiry_ms_boot = selected.second;
    } catch (...) {
    }
  }
  if (fut_sym_boot.empty()) {
    std::cout << "  [!] Could not auto-detect quarterly symbol. Aborting.\n";
    std::exit(1);
  } else {
    std::cout << "Fetching live prices for Fee & Capital Estimate ("
              << fut_sym_boot << ")....\n";
    json sp_res = signed_request(
        "GET",
        "https://api.binance.com/api/v3/ticker/price?symbol=" + cfg.symbol, "");
    json fp_res = signed_request(
        "GET",
        "https://fapi.binance.com/fapi/v1/ticker/price?symbol=" + fut_sym_boot,
        "");

    double sp_price = 0, fp_price = 0;
    if (sp_res.contains("price"))
      sp_price = std::stod(sp_res["price"].get<std::string>());
    if (fp_res.contains("price"))
      fp_price = std::stod(fp_res["price"].get<std::string>());

    if (sp_price > 0 && fp_price > 0) {
      double spot_not = sp_price * cfg.quantity;
      double fut_not = fp_price * cfg.quantity;
      double rnd_fee =
          2.0 * (spot_not * cfg.spot_fee + fut_not * cfg.futures_fee);
      double cap_s = spot_not / cfg.spot_leverage;
      double cap_f = fut_not / cfg.leverage;

      std::cout
          << "==========================================================\n";
      std::cout << "  FEE ESTIMATE + CAPITAL REQUIRED  (qty=" << cfg.quantity
                << " " << cfg.symbol.substr(0, 3) << ")\n";
      std::cout
          << "==========================================================\n";
      std::cout << "  Spot price now         : $" << std::fixed
                << std::setprecision(2) << sp_price << "\n";
      std::cout << "  Futures price now      : $" << std::fixed
                << std::setprecision(2) << fp_price << "\n";
      std::cout << "  Notional (spot leg)    : $" << spot_not << "  ("
                << cfg.quantity << " " << cfg.symbol.substr(0, 3) << " x $"
                << sp_price << ")\n";
      std::cout << "  Notional (futures leg) : $" << fut_not << "  ("
                << cfg.quantity << " " << cfg.symbol.substr(0, 3) << " x $"
                << fp_price << ")\n";
      std::cout << "  Est. round-trip fees   : $" << std::fixed
                << std::setprecision(4) << rnd_fee << "\n";
      std::cout << "  Est. spot margin req.  : $" << std::fixed
                << std::setprecision(2) << cap_s << " (buffered: $"
                << (cap_s * 1.1) << ")\n";
      std::cout << "  Est. futures margin req.: $" << std::fixed
                << std::setprecision(2) << cap_f << " (buffered: $"
                << (cap_f * 1.1) << ")\n";
      std::cout << "  TOTAL CAPITAL REQUIRED : $" << std::fixed
                << std::setprecision(2) << (cap_s * 1.1 + cap_f * 1.1) << "\n";
      std::cout
          << "==========================================================\n\n";
    } // end if (sp_price > 0 && fp_price > 0)
  } // end else (fut_sym_boot found)

  std::cout
      << "[+] Boot checks complete. Launching Live Matrix in 5 seconds...\n";
  if (!cfg.dry_run)
    std::this_thread::sleep_for(std::chrono::seconds(5));
}

// ThreadPool implementation
class ThreadPool {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop = false;
public:
    ThreadPool(size_t n) {
        for(size_t i=0; i<n; ++i)
            workers.emplace_back([this]{
                for(;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
    }
    template<class F>
    auto enqueue(F&& f) -> std::future<typename std::invoke_result_t<F>> {
        using res_type = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<res_type()>>(std::forward<F>(f));
        std::future<res_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if(stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task](){ (*task)(); });
        }
        condition.notify_one();
        return res;
    }
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &w: workers) w.join();
    }
};

ThreadPool pool(4);

int main() {
  enable_vt_mode();
#ifdef _WIN32
  SetConsoleCtrlHandler(consoleHandler, TRUE);
#endif
  load_config();

  // ── Initialise persistent cURL (must come before any signed_request calls)
  // ──
  init_curl(cfg.api_key);
  warmup_connections();
  init_ws_order_sessions();

  // ── Open trade audit log ────────────────────────────────────────────
  {
    auto now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm bt = *std::localtime(&now);
    std::ostringstream log_name;
    log_name << "trade_log_" << std::put_time(&bt, "%Y%m%d_%H%M%S") << ".csv";
    g_trade_log.open(log_name.str(), std::ios::app);
    if (g_trade_log.is_open()) {
      g_trade_log << "event,timestamp_ms,details\n";
      std::cout << "[+] Trade log: " << log_name.str() << "\n";
    } else {
      std::cerr << "[WARN] Could not open trade log file.\n";
    }
  }

  display_boot_summary();

  std::cout << "\033[2J";    // Clear entire screen on boot
  std::cout << "\033[28;r";  // Lock top 27 rows, make bottom scroll dynamically
  std::cout << "\033[28;1H"; // Move cursor instantly into scroll zone

  std::thread(balance_updater_thread).detach();
  std::thread(keyboard_listener_thread).detach();

  std::string fut_symbol = fut_sym_boot;
  long long expiry_ms = global_expiry_ms_boot;

  BinanceWebSocketManager ws_manager(cfg.symbol, fut_symbol);
  ws_manager.start();

  while (ws_manager.get_spot_quote().bid == 0.0 ||
         ws_manager.get_fut_quote().bid == 0.0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::cout << "  \033[1;30m[" << get_time_fmt()
            << "]\033[0m \033[1;36m[SYSTEM]\033[0m Dashboard & Execution "
               "Matrix Active.\n";

  long long last_print = 0;
  long long last_log = 0;
  bool active_trade = false;
  Trade current_trade;

  int session_trades = 0;
  double session_pnl = 0.0;
  long long cooldown_until = 0;

  while (true) {
    Quote sq = ws_manager.get_spot_quote();
    Quote fq = ws_manager.get_fut_quote();
    if (sq.bid == 0.0 || fq.bid == 0.0)
      continue;

    double spot_p = (sq.bid + sq.ask) / 2.0;
    double fut_p = (fq.bid + fq.ask) / 2.0;

    SpreadMetrics m = compute_spread_metrics(spot_p, fut_p, expiry_ms);
    long long now = get_current_ms();

    if (force_ui_update || now - last_print > cfg.print_interval_sec * 1000) {
      long long ping = now - ws_manager.last_fut_time;
      if (ws_manager.last_fut_time == 0 || ping < 0 || ping > 9999)
        ping = 0;

      draw_dashboard(spot_p, fut_p, m, ping, session_trades, session_pnl,
                     active_trade, current_trade, fut_symbol);
      last_print = now;
      force_ui_update = false;
    }

    if (now - last_log > 2000) {
      std::cout << "  \033[1;30m[" << get_time_fmt()
                << "]\033[0m TICK S: " << spot_p << " | F: " << fut_p
                << " | SVD: " << std::showpos << m.spread << std::noshowpos
                << "\n";
      last_log = now;
    }

    // ── ENTRY LOGIC ──
    if (!active_trade) {
      if (force_quit) {
          std::cout << "\n[!] Quitting Bot safely...\n";
          break; // exit loop and wait for any handlers
      }
      if (now < cooldown_until) {
          if (now - last_log > 2000) {
              std::cout << "  [COOLDOWN] Circuit breaker active. Waiting " 
                        << (cooldown_until - now)/1000 << "s before next trade pattern...\n";
              last_log = now;
          }
          continue;
      }
      long long spot_lag = now - sq.recv_time;
      long long fut_lag = now - fq.recv_time;
      long long gap = std::abs(sq.recv_time - fq.recv_time);
      if (spot_lag > 200 || fut_lag > 200 || gap > 25) {
        if (now - last_log > 2000) {
          std::cout << "  [STALE] Skipping entry — spot_lag=" << spot_lag
                    << "ms fut_lag=" << fut_lag << "ms gap=" << gap << "ms\n";
          last_log = now;
        }
        continue;
      }

      double rt_fee = 2.0 * (spot_p * cfg.spot_fee + fut_p * cfg.futures_fee);

      double exec_spread_long = fq.bid - sq.ask;  // buy spot, sell fut
      double exec_spread_short = sq.bid - fq.ask; // sell spot, buy fut

      std::string dir =
          (m.spread > 0) ? "long_spot_short_fut" : "short_spot_long_fut";
      double exec_spread =
          (m.spread > 0) ? exec_spread_long : exec_spread_short;

      double exec_apr = (m.dte > 0)
                            ? ((exec_spread / spot_p) * 100.0 * (365.0 / m.dte))
                            : 0.0;
      bool mode_a = (exec_apr >= cfg.min_apr_pct);
      bool mode_b = (cfg.spread_entry_enabled &&
                     exec_spread >= cfg.spread_entry_min_usdt &&
                     m.dte >= cfg.spread_entry_min_dte);

      if ((mode_a || mode_b) && exec_spread > rt_fee * cfg.fee_safety_mult) {
        {
          std::ostringstream lss;
          lss << "  \033[1;30m[" << get_time_fmt()
              << "]\033[0m \033[1;33m[ENTRY SIGNAL]\033[0m Locking " << dir
              << " (Exec_Spread: " << std::fixed << std::setprecision(4)
              << exec_spread << " Mid_Spread: " << m.spread
              << ") -> Executing PARALLEL via WS Order API...\n";
          add_trade_log_msg(lss.str());
        }

        // --- PRE-FLIGHT VALIDATION: Check quantity > step_size ---
        double step = 0.001;  // BTC quarterly futures step size is usually 0.001
        double rounded_qty = std::floor(cfg.quantity / step) * step;
        if (rounded_qty <= 0.0) {
            std::ostringstream lss;
            lss << "  \033[1;30m[" << get_time_fmt() << "]\033[0m "
                << "\033[1;31m[ABORT]\033[0m Quantity " << cfg.quantity 
                << " rounds to 0 for step " << step << " on Futures. Fix config.";
            add_trade_log_msg(lss.str());
            std::this_thread::sleep_for(std::chrono::seconds(10));
            continue;  // don't fire either leg
        }

        // --- PRE-FLIGHT VALIDATION: Strict Pre-Borrow Check ---
        bool margin_ok = true;
        
        if (m.spread > 0) { // BUY -> need USDT
           double needed_usdt = cfg.quantity * spot_p;
           double free_usdt = bal_spot_usdt.load();
           if (needed_usdt > free_usdt) margin_ok = false;
        } else { // SELL -> need BTC
           double needed_btc = cfg.quantity;
           double free_btc = bal_spot_btc.load();
           if (needed_btc > free_btc) margin_ok = false;
        }
        
        double fut_req = (cfg.quantity * fut_p) / cfg.leverage;
        if (bal_fut_usdt.load() * 0.98 < fut_req) {
            std::ostringstream lss;
            lss << "  \033[1;30m[" << get_time_fmt() << "]\033[0m \033[1;31m[ABORT]\033[0m Fut margin low: " << bal_fut_usdt.load() << " Req: " << fut_req;
            add_trade_log_msg(lss.str());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        if (!margin_ok) {
            std::ostringstream lss;
            lss << "  \033[1;30m[" << get_time_fmt() << "]\033[0m \033[1;31m[ABORT]\033[0m Insufficient PRE-BORROWED Margin equity. Fix balances.";
            add_trade_log_msg(lss.str());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        long long t1 = get_current_ms();
        long long sp_lat = 0, ft_lat = 0;
        // ── Fire spot + futures in PARALLEL ──────────────────────────────
        auto sp_fut = pool.enqueue([&]() {
          long long t_s = get_current_ms();
          auto res = place_spot_order((m.spread > 0) ? "BUY" : "SELL", "NO_SIDE_EFFECT", spot_p);
          sp_lat = get_current_ms() - t_s;
          return res;
        });
        auto ft_fut = pool.enqueue([&]() {
          long long t_s = get_current_ms();
          auto res = place_fut_order_ws(fut_symbol, (m.spread > 0) ? "SELL" : "BUY", false, fut_p);
          ft_lat = get_current_ms() - t_s;
          return res;
        });
        auto sp_res = sp_fut.get();
        auto ft_res = ft_fut.get();
        long long t2 = get_current_ms(); // both legs finished

        current_trade.direction = dir;
        current_trade.entry_mode = mode_a ? "APR" : "SPREAD";
        current_trade.entry_spot = sp_res.second;
        current_trade.entry_fut = ft_res.second;
        current_trade.entry_spread = ft_res.second - sp_res.second;
        current_trade.entry_fees =
            (sp_res.second * cfg.quantity * cfg.spot_fee) +
            (ft_res.second * cfg.quantity * cfg.futures_fee);
        current_trade.held_bars = 0;

        if (!sp_res.first || !ft_res.first || sp_res.second <= 0.0 ||
            ft_res.second <= 0.0) {
          std::cout << "  \033[1;30m[" << get_time_fmt()
                    << "]\033[0m \033[1;31m[FAILED]\033[0m Entry order "
                       "failed/rejected. Spot OK: "
                    << (sp_res.first ? "Y" : "N") << " (" << sp_res.second
                    << ") Fut OK: " << (ft_res.first ? "Y" : "N") << " ("
                    << ft_res.second << ")\n";

          // ── EMERGENCY UNHEDGE: if one leg filled, immediately reverse it ──
          if (sp_res.first && sp_res.second > 0.0 &&
              (!ft_res.first || ft_res.second <= 0.0)) {
            // Spot filled, futures failed → reverse the spot leg
            std::string rev_side = (m.spread > 0) ? "SELL" : "BUY";
            std::string rev_effect = "NO_SIDE_EFFECT";
            std::cout << "  \033[1;31m[EMERGENCY] Reversing spot leg to avoid "
                         "naked position...\033[0m\n";
            auto rev = place_spot_order(rev_side, rev_effect, spot_p);
            std::cout << "  [EMERGENCY] Spot reversal: "
                      << (rev.first ? "OK" : "FAILED") << " @ " << rev.second
                      << "\n\n";
            log_trade("EMERGENCY_REVERSAL," + std::to_string(get_current_ms()) +
                      ",SP_FILL=" + std::to_string(sp_res.second) +
                      ",REV=" + std::to_string(rev.second));
            cooldown_until = now + 300000;
          } else if (ft_res.first && ft_res.second > 0.0 &&
                     (!sp_res.first || sp_res.second <= 0.0)) {
            // Futures filled, spot failed → close the futures position
            std::string rev_side = (m.spread > 0) ? "BUY" : "SELL";
            std::cout << "  \033[1;31m[EMERGENCY] Closing futures leg to avoid "
                         "naked position...\033[0m\n";
            auto rev = place_fut_order(fut_symbol, rev_side, true, fut_p);
            std::cout << "  [EMERGENCY] Futures reversal: "
                      << (rev.first ? "OK" : "FAILED") << " @ " << rev.second
                      << "\n\n";
            log_trade("EMERGENCY_REVERSAL," + std::to_string(get_current_ms()) +
                      ",FT_FILL=" + std::to_string(ft_res.second) +
                      ",REV=" + std::to_string(rev.second));
            cooldown_until = now + 300000;
          }
          // We do NOT set active_trade = true because entry failed!
        } else {
          active_trade = true;

          std::cout << "  \033[1;30m[" << get_time_fmt()
                    << "]\033[0m \033[1;32m[SUCCESS]\033[0m Filled SPOT @ "
                    << current_trade.entry_spot << " FUT @ "
                    << current_trade.entry_fut
                    << "  [ Latency | Spot: " << sp_lat << "ms | Fut(WS): " << ft_lat 
                    << "ms | Total: " << (t2 - t1) << "ms ]\n\n";

          log_trade("ENTRY," + std::to_string(get_current_ms()) +
                    ",dir=" + dir + ",sp=" + std::to_string(sp_res.second) +
                    ",ft=" + std::to_string(ft_res.second) +
                    ",fees=" + std::to_string(current_trade.entry_fees));
        }
      }
    }
    // ── EXIT LOGIC ──
    else {
      current_trade.held_bars++;
      double gross = (current_trade.direction == "long_spot_short_fut")
                         ? ((spot_p - current_trade.entry_spot) * cfg.quantity +
                            (current_trade.entry_fut - fut_p) * cfg.quantity)
                         : ((current_trade.entry_spot - spot_p) * cfg.quantity +
                            (fut_p - current_trade.entry_fut) * cfg.quantity);

      double fee_now = spot_p * cfg.quantity * cfg.spot_fee +
                       fut_p * cfg.quantity * cfg.futures_fee;
      double live_net = gross - current_trade.entry_fees - fee_now;

      double exit_target = (m.dte >= 30)
                               ? (std::abs(current_trade.entry_spread) *
                                  (1.0 - cfg.exit_pct_of_entry))
                               : cfg.exit_spread_usdt;

      bool exit_condition = (std::abs(m.spread) <= exit_target &&
          current_trade.held_bars >= cfg.min_hold_bars && live_net > 0);

      if (force_quit || force_close_trades || exit_condition) {
        {
          std::ostringstream lss;
          if (force_quit || force_close_trades) {
              lss << "  \033[1;30m[" << get_time_fmt() << "]\033[0m \033[1;33m[MANUAL EXIT]\033[0m Closing open positions.";
          } else {
              lss << "  \033[1;30m[" << get_time_fmt() << "]\033[0m \033[1;33m[EXIT SIGNAL]\033[0m Convergence Achieved!";
          }
          add_trade_log_msg(lss.str());
        }

        long long t1 = get_current_ms();
        long long sp_lat = 0, ft_lat = 0;
        // ── Fire exit orders in PARALLEL ────────────────────────────────
        auto sp_fut_ex = pool.enqueue([&]() {
          long long t_s = get_current_ms();
          auto res = place_spot_order(
              (current_trade.direction == "long_spot_short_fut") ? "SELL" : "BUY",
              "NO_SIDE_EFFECT", spot_p);
          sp_lat = get_current_ms() - t_s;
          return res;
        });
        auto ft_fut_ex = pool.enqueue([&]() {
          long long t_s = get_current_ms();
          auto res = place_fut_order_ws(
              fut_symbol,
              (current_trade.direction == "long_spot_short_fut") ? "BUY" : "SELL",
              true, fut_p);
          ft_lat = get_current_ms() - t_s;
          return res;
        });
        auto sp_res = sp_fut_ex.get();
        auto ft_res = ft_fut_ex.get();
        long long t2 = get_current_ms(); // both done

        if (!sp_res.first || !ft_res.first || sp_res.second <= 0.0 ||
            ft_res.second <= 0.0) {
          std::cout << "  \033[1;30m[" << get_time_fmt()
                    << "]\033[0m \033[1;31m[FAILED]\033[0m Exit order "
                       "failed/rejected. Spot OK: "
                    << (sp_res.first ? "Y" : "N") << " (" << sp_res.second
                    << ") Fut OK: " << (ft_res.first ? "Y" : "N") << " ("
                    << ft_res.second << ")\n\n";
          // We keep active_trade = true so it will try again
        } else {
          double final_gross =
              (current_trade.direction == "long_spot_short_fut")
                  ? ((sp_res.second - current_trade.entry_spot) * cfg.quantity +
                     (current_trade.entry_fut - ft_res.second) * cfg.quantity)
                  : ((current_trade.entry_spot - sp_res.second) * cfg.quantity +
                     (ft_res.second - current_trade.entry_fut) * cfg.quantity);

          double final_fees = sp_res.second * cfg.quantity * cfg.spot_fee +
                              ft_res.second * cfg.quantity * cfg.futures_fee;
          double actual_net =
              final_gross - current_trade.entry_fees - final_fees;

          session_trades++;
          session_pnl += actual_net;

          std::cout << "  \033[1;30m[" << get_time_fmt()
                    << "]\033[0m \033[1;32m[SUCCESS]\033[0m Exited SPOT @ "
                    << sp_res.second << " FUT @ " << ft_res.second
                    << "  [ Exit Latency | Spot: " << sp_lat << "ms | Fut(WS): " << ft_lat
                    << "ms | Total: " << (t2 - t1) << "ms ] | \033[1;32mPNL: $" << actual_net << "\033[0m\n\n";

          log_trade("EXIT," + std::to_string(get_current_ms()) +
                    ",dir=" + current_trade.direction +
                    ",sp=" + std::to_string(sp_res.second) +
                    ",ft=" + std::to_string(ft_res.second) +
                    ",net_pnl=" + std::to_string(actual_net) +
                    ",session_total=" + std::to_string(session_pnl));

          active_trade = false;
          force_close_trades = false;
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 0;
}
