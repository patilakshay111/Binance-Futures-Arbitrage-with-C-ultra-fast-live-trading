#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
  std::string api_key = "YOUR_API_KEY";
  std::string api_secret = "YOUR_API_SECRET";
  std::string symbol = "BTCUSDT";
  double quantity = 0.0001;
  int use_contract = 0;

  double spot_fee = 0.001;
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
const std::string MARGIN_ORDER_URL = "https://api.binance.com/sapi/v1/margin/order";
const std::string FUT_ORDER_URL = FUT_URL + "/order";

Config cfg;
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
  ((std::string *)userp)->append((char *)contents, size * nmemb);
  return size * nmemb;
}

// =========================================================================
// HMAC SHA256 & CURL REQUESTS
// =========================================================================
std::string hmac_sha256(const std::string &key, const std::string &data) {
  unsigned char hash[32];
  unsigned int len = 32;
  HMAC_CTX *hmac = HMAC_CTX_new();
  HMAC_Init_ex(hmac, &key[0], key.length(), EVP_sha256(), NULL);
  HMAC_Update(hmac, (unsigned char *)&data[0], data.length());
  HMAC_Final(hmac, hash, &len);
  HMAC_CTX_free(hmac);

  std::stringstream ss;
  for (unsigned int i = 0; i < len; i++) {
    ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
  }
  return ss.str();
}

long long get_current_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

json signed_request(const std::string &method, const std::string &url_base, std::string query_string) {
  long long ts = get_current_ms();
  if (!query_string.empty()) query_string += "&";
  query_string += "timestamp=" + std::to_string(ts);

  std::string signature = hmac_sha256(cfg.api_secret, query_string);
  std::string full_url = url_base + "?" + query_string + "&signature=" + signature;

  CURL *curl = curl_easy_init();
  std::string readBuffer;
  if (curl) {
    struct curl_slist *headers = NULL;
    std::string apiKeyHeader = "X-MBX-APIKEY: " + cfg.api_key;
    headers = curl_slist_append(headers, apiKeyHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    if (method == "POST") {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    }
    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  }
  try {
    return json::parse(readBuffer);
  } catch (...) {
    return json{};
  }
}

// =========================================================================
// WEBSOCKET MANAGER
// =========================================================================
class BinanceWebSocketManager {
public:
  std::atomic<double> spot_bid{0.0}, spot_ask{0.0};
  std::atomic<double> fut_bid{0.0}, fut_ask{0.0};
  std::atomic<long long> last_fut_time{0};

  BinanceWebSocketManager(const std::string &s_sym, const std::string &f_sym) {
    spot_symbol = s_sym;
    fut_symbol = f_sym;
    for (char &c : spot_symbol) c = std::tolower(c);
    for (char &c : fut_symbol) c = std::tolower(c);
  }

  void start() {
    std::thread([this]() { run_stream("stream.binance.com", "9443", "/ws/" + spot_symbol + "@bookTicker", true); }).detach();
    std::thread([this]() { run_stream("fstream.binance.com", "443", "/ws/" + fut_symbol + "@bookTicker", false); }).detach();
  }

  double get_spot_price() { return (spot_bid + spot_ask) / 2.0; }
  double get_fut_price() { return (fut_bid + fut_ask) / 2.0; }

private:
  std::string spot_symbol, fut_symbol;

  void run_stream(const std::string &host, const std::string &port, const std::string &target, bool is_spot) {
    try {
      asio::io_context ioc;
      ssl::context ctx{ssl::context::tlsv12_client};

      tcp::resolver resolver{ioc};
      websocket::stream<ssl::stream<tcp::socket>> ws{ioc, ctx};

      auto const results = resolver.resolve(host, port);
      auto ep = asio::connect(ws.next_layer().next_layer(), results);

      if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
        return;
      }

      ws.next_layer().handshake(ssl::stream_base::client);
      ws.set_option(websocket::stream_base::decorator([](websocket::request_type &req) {
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
          if (is_spot) {
            spot_bid = bid;
            spot_ask = ask;
          } else {
            fut_bid = bid;
            fut_ask = ask;
            if (j.contains("E")) last_fut_time = j["E"].get<long long>();
          }
        }
      }
    } catch (...) {}
  }
};

// =========================================================================
// TRADING EXECUTION ENGINE
// =========================================================================
struct SpreadMetrics { double spread, spread_rate, apr, dte; };
struct Trade {
  std::string direction;
  std::string entry_mode;
  double entry_spot;
  double entry_fut;
  double entry_spread;
  double entry_fees;
  int held_bars = 0;
};

SpreadMetrics compute_spread_metrics(double spot_p, double fut_p, long long expiry_ms) {
  SpreadMetrics m;
  m.spread = fut_p - spot_p;
  m.spread_rate = (m.spread / spot_p) * 100.0;
  double now_sec = get_current_ms() / 1000.0;
  double exp_sec = expiry_ms / 1000.0;
  m.dte = (exp_sec - now_sec) / 86400.0;
  m.apr = (m.dte > 0) ? (m.spread_rate * (365.0 / m.dte)) : 0.0;
  return m;
}

double get_fill_price(const json& resp, double request_qty) {
    if (resp.contains("fills") && resp["fills"].is_array() && !resp["fills"].empty()) {
        double total_qty = 0, total_vol = 0;
        for (auto& f : resp["fills"]) {
            double f_qty = std::stod(f["qty"].get<std::string>());
            double f_px = std::stod(f["price"].get<std::string>());
            total_qty += f_qty; total_vol += f_qty * f_px;
        }
        return total_qty > 0 ? (total_vol / total_qty) : 0.0;
    } else if (resp.contains("avgPrice")) {
        return std::stod(resp["avgPrice"].get<std::string>());
    } else if (resp.contains("price")) {
        return std::stod(resp["price"].get<std::string>());
    }
    return 0.0;
}

std::pair<bool, double> place_spot_order(const std::string& side, const std::string& side_effect, double price_hint) {
    if (cfg.dry_run) return {true, price_hint};
    std::ostringstream qs;
    qs << std::fixed << std::setprecision(5);
    qs << "symbol=" << cfg.symbol << "&side=" << side << "&type=MARKET&quantity=" << cfg.quantity 
       << "&isIsolated=TRUE&sideEffectType=" << side_effect;
    json resp = signed_request("POST", MARGIN_ORDER_URL, qs.str());
    if (resp.contains("code")) return {false, 0.0};
    return {true, get_fill_price(resp, cfg.quantity)};
}

std::pair<bool, double> place_fut_order(const std::string& fut_symbol, const std::string& side, bool reduce_only, double price_hint) {
    if (cfg.dry_run) return {true, price_hint};
    std::ostringstream qs;
    qs << std::fixed << std::setprecision(5);
    qs << "symbol=" << fut_symbol << "&side=" << side << "&type=MARKET&quantity=" << cfg.quantity 
       << "&reduceOnly=" << (reduce_only ? "true" : "false");
    json resp = signed_request("POST", FUT_ORDER_URL, qs.str());
    if (resp.contains("code")) return {false, 0.0};
    double fill = get_fill_price(resp, cfg.quantity);
    if (fill == 0.0 && resp.contains("orderId")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        json q_resp = signed_request("GET", FUT_ORDER_URL, "symbol=" + fut_symbol + "&orderId=" + resp["orderId"].dump());
        fill = get_fill_price(q_resp, cfg.quantity);
    }
    return {true, fill};
}

// =========================================================================
// UI HELPERS
// =========================================================================
std::string get_time_fmt() {
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  auto timer = std::chrono::system_clock::to_time_t(now);
  std::tm bt = *std::localtime(&timer);
  std::ostringstream oss;
  oss << std::put_time(&bt, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
  return oss.str();
}

void load_config() {
    std::ifstream f("../config.json");
    if (f.is_open()) {
        try {
            json j = json::parse(f);
            if(j.contains("api_key")) cfg.api_key = j["api_key"];
            if(j.contains("api_secret")) cfg.api_secret = j["api_secret"];
            if(j.contains("symbol")) cfg.symbol = j["symbol"];
            if(j.contains("quantity")) cfg.quantity = j["quantity"];
            if(j.contains("print_interval_sec")) cfg.print_interval_sec = j["print_interval_sec"];
            if(j.contains("dry_run")) cfg.dry_run = j["dry_run"];
            if(j.contains("min_apr_pct")) cfg.min_apr_pct = j["min_apr_pct"];
            if(j.contains("spot_fee")) cfg.spot_fee = j["spot_fee"];
            if(j.contains("futures_fee")) cfg.futures_fee = j["futures_fee"];
        } catch(...) {
            std::cerr << "[!] Failed to parse config.json. Using defaults.\n";
        }
    } else {
        std::cerr << "[-] config.json not found, using static defaults.\n";
    }
}

int main() {
  load_config();
  std::cout << "==========================================================\n";
  std::cout << "  SPOT-QUARTERLY FUTURES C++ HFT BOT\n";
  std::cout << "  Symbol        : " << cfg.symbol << "\n";
  std::cout << "  Quantity      : " << cfg.quantity << " " << cfg.symbol.substr(0, 3) << "\n";
  std::cout << "  Operating Mode: " << (cfg.dry_run ? "PAPER TRADING (DRY RUN)" : "LIVE TRADING (DANGER)") << "\n";
  std::cout << "==========================================================\n";

  std::string fut_symbol = "BTCUSDT_260327";
  long long expiry_ms = 1774598400000; 

  BinanceWebSocketManager ws_manager(cfg.symbol, fut_symbol);
  ws_manager.start();

  while (ws_manager.get_spot_price() == 0.0 || ws_manager.get_fut_price() == 0.0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::cout << "[+] Streaming active. Clean UI Enabled...\n\n";

  long long last_print = 0;
  bool active_trade = false;
  Trade current_trade;
  
  int session_trades = 0;
  double session_pnl = 0.0;

  while (true) {
    double spot_p = ws_manager.get_spot_price();
    double fut_p = ws_manager.get_fut_price();
    if (spot_p == 0.0 || fut_p == 0.0) continue;

    SpreadMetrics m = compute_spread_metrics(spot_p, fut_p, expiry_ms);
    long long now = get_current_ms();
    
    // UI Output (Using \r to overwrite dynamically)
    if (now - last_print > cfg.print_interval_sec * 1000) {
        long long ping = now - ws_manager.last_fut_time;
        if (ws_manager.last_fut_time == 0 || ping < 0 || ping > 9999) ping = 0;
        
        std::cout << "\r[" << get_time_fmt() << "] SPOT=" << std::fixed << std::setprecision(2) << spot_p 
                  << " FUT=" << fut_p << " SVD=" << std::showpos << m.spread << std::noshowpos 
                  << " APR=" << std::fixed << std::setprecision(2) << m.apr << "% DTE=" << std::setprecision(1) << m.dte 
                  << "d PING=" << ping << "ms      " << std::flush;
        last_print = now;
    }

    // ── ENTRY LOGIC ──
    if (!active_trade) {
        double rt_fee = 2.0 * (spot_p * cfg.spot_fee + fut_p * cfg.futures_fee);
        bool mode_a = (m.apr >= cfg.min_apr_pct);
        bool mode_b = (cfg.spread_entry_enabled && std::abs(m.spread) >= cfg.spread_entry_min_usdt && m.dte >= cfg.spread_entry_min_dte);

        if ((mode_a || mode_b) && std::abs(m.spread) > rt_fee * cfg.fee_safety_mult) {
            std::string dir = (m.spread > 0) ? "long_spot_short_fut" : "short_spot_long_fut";
            
            std::cout << "\n\n  [TRADE ENTRY DETECTED] " << dir << " Spread: " << m.spread << "\n";
            
            long long t1 = get_current_ms();
            auto sp_res = place_spot_order((m.spread > 0) ? "BUY" : "SELL", (m.spread > 0) ? "MARGIN_BUY" : "AUTO_REPAY", spot_p);
            long long t2 = get_current_ms();
            auto ft_res = place_fut_order(fut_symbol, (m.spread > 0) ? "SELL" : "BUY", false, fut_p);
            long long t3 = get_current_ms();

            current_trade.direction = dir;
            current_trade.entry_mode = mode_a ? "APR" : "SPREAD";
            current_trade.entry_spot = sp_res.second;
            current_trade.entry_fut = ft_res.second;
            current_trade.entry_spread = ft_res.second - sp_res.second;
            current_trade.entry_fees = (sp_res.second * cfg.quantity * cfg.spot_fee) + (ft_res.second * cfg.quantity * cfg.futures_fee);
            current_trade.held_bars = 0;
            active_trade = true;

            std::cout << "  => SUCCESS! Filled SPOT @ " << current_trade.entry_spot << " (" << (t2 - t1) << "ms)  FUT @ " 
                      << current_trade.entry_fut << " (" << (t3 - t2) << "ms)\n\n";
        }
    } 
    // ── EXIT LOGIC ──
    else {
        current_trade.held_bars++;
        double gross = (current_trade.direction == "long_spot_short_fut") ?
            ((spot_p - current_trade.entry_spot) * cfg.quantity + (current_trade.entry_fut - fut_p) * cfg.quantity) :
            ((current_trade.entry_spot - spot_p) * cfg.quantity + (fut_p - current_trade.entry_fut) * cfg.quantity);
        
        double fee_now = spot_p * cfg.quantity * cfg.spot_fee + fut_p * cfg.quantity * cfg.futures_fee;
        double live_net = gross - current_trade.entry_fees - fee_now;

        double exit_target = (m.dte >= 30) ? (std::abs(current_trade.entry_spread) * (1.0 - cfg.exit_pct_of_entry)) : cfg.exit_spread_usdt;
        
        if (std::abs(m.spread) <= exit_target && current_trade.held_bars >= cfg.min_hold_bars && live_net > 0) {
            std::cout << "\n\n  [TRADE EXIT DETECTED] Convergence Achieved! Securing Profit.\n";

            long long t1 = get_current_ms();
            auto sp_res = place_spot_order((current_trade.direction == "long_spot_short_fut") ? "SELL" : "BUY", 
                                           (current_trade.direction == "long_spot_short_fut") ? "AUTO_REPAY" : "MARGIN_BUY", spot_p);
            long long t2 = get_current_ms();
            auto ft_res = place_fut_order(fut_symbol, (current_trade.direction == "long_spot_short_fut") ? "BUY" : "SELL", true, fut_p);
            long long t3 = get_current_ms();

            double final_gross = (current_trade.direction == "long_spot_short_fut") ?
                ((sp_res.second - current_trade.entry_spot) * cfg.quantity + (current_trade.entry_fut - ft_res.second) * cfg.quantity) :
                ((current_trade.entry_spot - sp_res.second) * cfg.quantity + (ft_res.second - current_trade.entry_fut) * cfg.quantity);
            
            double final_fees = sp_res.second * cfg.quantity * cfg.spot_fee + ft_res.second * cfg.quantity * cfg.futures_fee;
            double actual_net = final_gross - current_trade.entry_fees - final_fees;

            session_trades++;
            session_pnl += actual_net;

            std::cout << "  => SUCCESS! Exited SPOT @ " << sp_res.second << " (" << (t2 - t1) << "ms)  FUT @ " 
                      << ft_res.second << " (" << (t3 - t2) << "ms)\n";
            std::cout << "  => NET PROFIT: $" << actual_net << "  | TOTAL BOT PROFIT: $" << session_pnl << " (" << session_trades << " trades)\n\n";

            active_trade = false;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 0;
}
