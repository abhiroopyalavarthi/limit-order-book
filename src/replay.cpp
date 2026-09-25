// Replays an order file through the matching engine, one book per symbol.
//
//   ./build/replay data/sample_orders.csv            prints every trade
//   ./build/replay big.csv --quiet                   summary + speed only
//   ./build/replay --generate 1000000 big.csv        writes a random file
//
// File format (header line optional):
//   symbol,action,id,side,type,price,qty
//   AAPL,A,1,B,L,189.52,100     add: side B/S, type L(imit) M(arket) I(OC) F(OK)
//   AAPL,M,1,,,189.55,50        modify id 1 to a new price/qty
//   AAPL,C,1,,,,                cancel id 1
// Prices are dollars with up to 2 decimals and are parsed straight into
// integer cents, never through a double.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "FastOrderBook.h"
#include "MatchingEngine.h"

namespace {

struct Row {
    std::string symbol;
    char action;  // A, C, M
    OrderId id;
    Side side;
    OrderType type;
    Price price;
    Quantity qty;
    size_t line;
};

// "189.52" -> 18952, "189.5" -> 18950, "189" -> 18900
std::optional<Price> parsePrice(const std::string& s) {
    if (s.empty()) return std::nullopt;
    size_t dot = s.find('.');
    std::string whole = s.substr(0, dot);
    std::string frac = dot == std::string::npos ? "" : s.substr(dot + 1);
    if (whole.empty() || frac.size() > 2) return std::nullopt;
    for (char c : whole + frac)
        if (c < '0' || c > '9') return std::nullopt;
    while (frac.size() < 2) frac += '0';
    return std::stoll(whole) * 100 + std::stoll(frac);
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string col;
    while (std::getline(ss, col, ',')) cols.push_back(col);
    if (!line.empty() && line.back() == ',') cols.push_back("");
    return cols;
}

bool parseRow(const std::string& line, size_t lineNo, Row& row, std::string& err) {
    auto c = splitCsv(line);
    while (c.size() < 7) c.push_back("");
    row = Row{};
    row.line = lineNo;
    row.symbol = c[0];
    if (row.symbol.empty() || c[1].size() != 1) { err = "bad symbol/action"; return false; }
    row.action = c[1][0];
    try {
        row.id = std::stoull(c[2]);
    } catch (...) { err = "bad id"; return false; }

    if (row.action == 'C') return true;

    if (row.action == 'A') {
        if (c[3] == "B") row.side = Side::Buy;
        else if (c[3] == "S") row.side = Side::Sell;
        else { err = "side must be B or S"; return false; }
        if (c[4] == "L") row.type = OrderType::Limit;
        else if (c[4] == "M") row.type = OrderType::Market;
        else if (c[4] == "I") row.type = OrderType::IOC;
        else if (c[4] == "F") row.type = OrderType::FOK;
        else { err = "type must be L, M, I or F"; return false; }
    } else if (row.action != 'M') {
        err = "action must be A, C or M";
        return false;
    }

    if (row.type != OrderType::Market || row.action == 'M') {
        auto p = parsePrice(c[5]);
        if (!p) { err = "bad price '" + c[5] + "'"; return false; }
        row.price = *p;
    }
    try {
        row.qty = static_cast<Quantity>(std::stoul(c[6]));
    } catch (...) { err = "bad qty"; return false; }
    return true;
}

std::string money(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld.%02lld", (long long)(p / 100), (long long)(p % 100));
    return buf;
}

int generate(size_t n, const std::string& path) {
    // a few made-up tickers at different price levels
    struct Sym { const char* name; Price mid; };
    const Sym syms[] = {{"AAPL", 18950}, {"MSFT", 41220}, {"NVDA", 12575}, {"SPY", 55310}, {"F", 1142}};
    std::mt19937_64 rng(123);
    std::uniform_int_distribution<int> pick(0, 4), pct(0, 99), side(0, 1);
    std::normal_distribution<double> off(0.0, 15.0);
    std::uniform_int_distribution<int> qty(1, 500);
    std::map<std::string, std::vector<OrderId>> live;

    std::ofstream out(path);
    if (!out) { std::cerr << "can't write " << path << "\n"; return 1; }
    out << "symbol,action,id,side,type,price,qty\n";
    OrderId nextId = 1;
    for (size_t i = 0; i < n; ++i) {
        const Sym& s = syms[pick(rng)];
        auto& ids = live[s.name];
        int roll = pct(rng);
        bool buy = side(rng);
        Price p = s.mid + static_cast<Price>(buy ? off(rng) - 2 : off(rng) + 2);
        if (p < 1) p = 1;
        if (roll < 70 || ids.empty()) {
            const char* type = roll < 60 ? "L" : roll < 65 ? "M" : roll < 68 ? "I" : "F";
            OrderId id = nextId++;
            ids.push_back(id);
            out << s.name << ",A," << id << ',' << (buy ? 'B' : 'S') << ',' << type << ','
                << (type[0] == 'M' ? "" : money(p)) << ',' << qty(rng) << '\n';
        } else {
            OrderId id = ids[std::uniform_int_distribution<size_t>(0, ids.size() - 1)(rng)];
            if (roll < 92) out << s.name << ",C," << id << ",,,,\n";
            else out << s.name << ",M," << id << ",,," << money(p) << ',' << qty(rng) << '\n';
        }
    }
    std::cout << "wrote " << n << " rows to " << path << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--generate")
        return generate(std::strtoull(argv[2], nullptr, 10), argv[3]);
    if (argc < 2) {
        std::cerr << "usage: replay <file.csv> [--quiet]\n"
                     "       replay --generate <rows> <out.csv>\n";
        return 1;
    }
    bool quiet = argc > 2 && std::string(argv[2]) == "--quiet";

    // 1) Parse the whole file first so the timed part is only matching.
    std::ifstream in(argv[1]);
    if (!in) { std::cerr << "can't open " << argv[1] << "\n"; return 1; }
    std::vector<Row> rows;
    std::string line;
    size_t lineNo = 0, bad = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("symbol,", 0) == 0) continue;
        Row row;
        std::string err;
        if (parseRow(line, lineNo, row, err)) rows.push_back(row);
        else if (++bad <= 5) std::cerr << "line " << lineNo << ": " << err << " -> skipped\n";
    }

    // 2) FastOrderBook needs a price band per symbol. Take the min/max price
    //    seen in the file for that symbol, with some room on each side.
    std::map<std::string, std::pair<Price, Price>> band;
    std::map<std::string, size_t> orderCount;
    for (const Row& r : rows) {
        if (r.action == 'C' || (r.action == 'A' && r.type == OrderType::Market)) continue;
        auto [it, first] = band.try_emplace(r.symbol, r.price, r.price);
        it->second.first = std::min(it->second.first, r.price);
        it->second.second = std::max(it->second.second, r.price);
    }
    MatchingEngine<FastOrderBook> engine;
    for (const Row& r : rows) {
        if (engine.book(r.symbol)) continue;
        auto it = band.find(r.symbol);
        Price lo = it == band.end() ? 0 : it->second.first;
        Price hi = it == band.end() ? 1 : it->second.second;
        Price pad = std::max<Price>(100, (hi - lo) / 2);
        engine.addSymbol(r.symbol, std::max<Price>(0, lo - pad), hi + pad, rows.size() / 8 + 16);
    }

    // 3) Replay.
    std::map<std::string, size_t> tradeCount;
    std::map<std::string, uint64_t> volume;
    std::vector<Trade> trades;
    auto start = std::chrono::steady_clock::now();
    for (const Row& r : rows) {
        trades.clear();
        if (r.action == 'A') {
            engine.addOrder(r.symbol, r.id, r.side, r.type, r.price, r.qty, trades);
            ++orderCount[r.symbol];
        } else if (r.action == 'C') {
            engine.cancelOrder(r.symbol, r.id);
        } else {
            engine.modifyOrder(r.symbol, r.id, r.price, r.qty, trades);
        }
        tradeCount[r.symbol] += trades.size();
        for (const Trade& t : trades) {
            volume[r.symbol] += t.quantity;
            if (!quiet)
                std::printf("%-5s TRADE buy=%llu sell=%llu %u @ %s\n", r.symbol.c_str(),
                            (unsigned long long)t.buyId, (unsigned long long)t.sellId,
                            t.quantity, money(t.price).c_str());
        }
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // 4) Summary.
    std::printf("\n%-6s %9s %9s %11s %10s %10s %8s\n", "symbol", "orders", "trades", "volume",
                "best bid", "best ask", "resting");
    for (const auto& [sym, book] : engine.books()) {
        auto bid = book.bestBid(), ask = book.bestAsk();
        std::printf("%-6s %9zu %9zu %11llu %10s %10s %8zu\n", sym.c_str(), orderCount[sym],
                    tradeCount[sym], (unsigned long long)volume[sym],
                    bid ? money(*bid).c_str() : "-", ask ? money(*ask).c_str() : "-",
                    book.orderCount());
    }
    std::printf("\n%zu rows replayed in %.3f s (%.2f M rows/s)%s\n", rows.size(), sec,
                rows.size() / sec / 1e6, bad ? ", some rows skipped" : "");
}
