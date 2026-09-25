// Throughput + latency benchmark for both order book versions.
//
//   ./build/bench            -> 1M and 10M ops
//   ./build/bench 5000000    -> just 5M ops
//
// Each "op" is an add (limit/market/IOC/FOK), cancel or modify, taken from
// a stream that is generated before the clock starts.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "FastOrderBook.h"
#include "OrderBook.h"
#include "OrderGenerator.h"

using Clock = std::chrono::steady_clock;

struct Result {
    double opsPerSec;
    double p50, p99, p999, max;  // nanoseconds
    size_t trades;
    size_t resting;
};

// Every book gets a fresh instance per run so runs don't affect each other.
template <typename Book, typename MakeBook>
Result run(const std::vector<Op>& ops, MakeBook make) {
    Result r{};
    std::vector<Trade> trades;
    trades.reserve(1024);

    // 1) Throughput: time the whole loop, no per-op timing overhead.
    //    Best of 3, since the first run also pays for page faults etc.
    double bestSec = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        Book book = make();
        size_t tradeCount = 0;
        auto start = Clock::now();
        for (const Op& op : ops) {
            trades.clear();
            applyOp(book, op, trades);
            tradeCount += trades.size();
        }
        double sec = std::chrono::duration<double>(Clock::now() - start).count();
        bestSec = std::min(bestSec, sec);
        r.trades = tradeCount;
        r.resting = book.orderCount();
    }
    r.opsPerSec = ops.size() / bestSec;

    // 2) Latency: time every op individually.
    //    Includes ~20-40ns of clock overhead per sample on most machines.
    std::vector<uint32_t> lat(ops.size());
    {
        Book book = make();
        for (size_t i = 0; i < ops.size(); ++i) {
            trades.clear();
            auto t0 = Clock::now();
            applyOp(book, ops[i], trades);
            auto t1 = Clock::now();
            lat[i] = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
    }
    auto pct = [&](double p) {
        size_t k = static_cast<size_t>(p * (lat.size() - 1));
        std::nth_element(lat.begin(), lat.begin() + k, lat.end());
        return static_cast<double>(lat[k]);
    };
    r.p50 = pct(0.50);
    r.p99 = pct(0.99);
    r.p999 = pct(0.999);
    r.max = *std::max_element(lat.begin(), lat.end());
    return r;
}

static void print(const char* name, const Result& r) {
    std::printf("  %-26s %8.2f M ops/s   p50 %6.0f ns   p99 %6.0f ns   p99.9 %7.0f ns   max %8.0f ns\n",
                name, r.opsPerSec / 1e6, r.p50, r.p99, r.p999, r.max);
}

// Only replays ops into V2, nothing else. Handy under a profiler:
//   valgrind --tool=callgrind ./build/bench 200000 profile
static void profileOnly(size_t n) {
    auto ops = generateOps(n, 42);
    FastOrderBook book(0, 20'000, n / 4);
    std::vector<Trade> trades;
    size_t count = 0;
    for (const Op& op : ops) {
        trades.clear();
        applyOp(book, op, trades);
        count += trades.size();
    }
    std::printf("%zu trades\n", count);
}

int main(int argc, char** argv) {
    std::vector<size_t> sizes = {1'000'000, 10'000'000};
    if (argc > 1) sizes = {static_cast<size_t>(std::strtoull(argv[1], nullptr, 10))};
    if (argc > 2 && std::string(argv[2]) == "profile") {
        profileOnly(sizes[0]);
        return 0;
    }

    for (size_t n : sizes) {
        std::printf("\n%zu ops (seed 42)\n", n);
        auto ops = generateOps(n, 42);

        Result v1 = run<OrderBook>(ops, [] { return OrderBook(); });
        print("V1 map + list", v1);

        Result v2 = run<FastOrderBook>(ops, [n] { return FastOrderBook(0, 20'000, n / 4); });
        print("V2 array + pool", v2);

        std::printf("  speedup: %.2fx throughput, p99 %.2fx lower\n",
                    v2.opsPerSec / v1.opsPerSec, v1.p99 / v2.p99);
        // Same input, so both versions must end in the same state.
        std::printf("  check: trades %zu vs %zu, resting orders %zu vs %zu -> %s\n",
                    v1.trades, v2.trades, v1.resting, v2.resting,
                    (v1.trades == v2.trades && v1.resting == v2.resting) ? "match" : "MISMATCH");
    }
}
