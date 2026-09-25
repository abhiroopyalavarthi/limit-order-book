#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "Order.h"
#include "Trade.h"

// One action fed to a book: add, cancel or modify.
struct Op {
    enum Kind : uint8_t { Add, Cancel, Modify } kind;
    Side side;
    OrderType type;
    OrderId id;
    Price price;
    Quantity qty;
};

// Builds a random but realistic-ish stream of ops ahead of time, so the
// benchmark only times the book and not the random number generator.
//
// Mix: 60% limit adds, 5% market, 3% IOC, 2% FOK, 22% cancels, 8% modifies.
// Prices cluster around a mid price (normal distribution), so most orders
// land near the top of the book like they do on a real exchange.
// Cancels/modifies pick a random earlier id; some of those are already filled,
// which exercises the "cancel an order that's gone" path too.
inline std::vector<Op> generateOps(size_t count, uint64_t seed,
                                   Price mid = 10'000, double stddevTicks = 20.0) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> side(0, 1);
    std::normal_distribution<double> offset(0.0, stddevTicks);
    std::uniform_int_distribution<Quantity> qty(1, 500);

    std::vector<Op> ops;
    ops.reserve(count);
    std::vector<OrderId> ids;
    ids.reserve(count);
    OrderId nextId = 1;

    auto randomPrice = [&](Side s) {
        // buyers sit a bit below mid, sellers a bit above, with overlap so trades happen
        double o = offset(rng);
        Price p = mid + static_cast<Price>(s == Side::Buy ? o - 2 : o + 2);
        return p < 1 ? 1 : p;
    };

    for (size_t i = 0; i < count; ++i) {
        int roll = pct(rng);
        Side s = side(rng) ? Side::Buy : Side::Sell;

        if (roll < 70 || ids.empty()) {
            OrderType t = OrderType::Limit;
            if (roll >= 60 && roll < 65) t = OrderType::Market;
            else if (roll >= 65 && roll < 68) t = OrderType::IOC;
            else if (roll >= 68 && roll < 70) t = OrderType::FOK;
            OrderId id = nextId++;
            ids.push_back(id);
            ops.push_back({Op::Add, s, t, id, t == OrderType::Market ? 0 : randomPrice(s), qty(rng)});
        } else {
            std::uniform_int_distribution<size_t> pick(0, ids.size() - 1);
            OrderId id = ids[pick(rng)];
            if (roll < 92)
                ops.push_back({Op::Cancel, s, OrderType::Limit, id, 0, 0});
            else
                ops.push_back({Op::Modify, s, OrderType::Limit, id, randomPrice(s), qty(rng)});
        }
    }
    return ops;
}

// Applies one op to any book type (OrderBook or FastOrderBook).
template <typename Book>
inline void applyOp(Book& book, const Op& op, std::vector<Trade>& out) {
    switch (op.kind) {
        case Op::Add:    book.addOrder(op.id, op.side, op.type, op.price, op.qty, out); break;
        case Op::Cancel: book.cancelOrder(op.id); break;
        case Op::Modify: book.modifyOrder(op.id, op.price, op.qty, out); break;
    }
}
