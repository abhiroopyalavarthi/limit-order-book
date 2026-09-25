#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "FlatHashMap.h"
#include "Order.h"
#include "Trade.h"

// Version 2. Same interface as OrderBook, different insides:
//  - price levels live in a flat array indexed by (price - minPrice), so
//    finding a level is an array index instead of a tree walk
//  - orders live in a pool (one big vector) and are linked into their level
//    by index, so adding an order doesn't call new
//  - order id -> pool slot goes through a flat open-addressing hash map
//
// The trade-off: prices must fall inside [minPrice, maxPrice] set up front.
// Limit/IOC/FOK orders outside that band are rejected.
class FastOrderBook {
public:
    FastOrderBook(Price minPrice = 0, Price maxPrice = 100'000,
                  size_t expectedOrders = 1 << 16);

    std::vector<Trade> addOrder(OrderId id, Side side, OrderType type,
                                Price price, Quantity qty);
    // Same, but appends trades to a caller-owned vector (no allocation
    // once the vector has capacity). Used by the benchmark.
    void addOrder(OrderId id, Side side, OrderType type, Price price,
                  Quantity qty, std::vector<Trade>& out);

    bool cancelOrder(OrderId id);

    std::optional<std::vector<Trade>> modifyOrder(OrderId id, Price newPrice,
                                                  Quantity newQty);
    bool modifyOrder(OrderId id, Price newPrice, Quantity newQty,
                     std::vector<Trade>& out);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    std::optional<Price> spread() const;

    Quantity volumeAt(Side side, Price price) const;
    size_t   orderCount() const { return index_.size(); }

private:
    static constexpr uint32_t kNil = UINT32_MAX;

    struct Node {
        Order order;
        uint32_t prev;
        uint32_t next;
    };

    struct Level {
        uint32_t head = kNil;   // oldest order (fills first)
        uint32_t tail = kNil;   // newest order
        uint64_t total = 0;     // sum of remaining qty at this price
    };

    bool inRange(Price p) const { return p >= minPrice_ && p <= maxPrice_; }
    size_t toIdx(Price p) const { return static_cast<size_t>(p - minPrice_); }

    uint32_t allocNode(const Order& o);
    void     freeNode(uint32_t n);
    void     pushBack(Level& level, uint32_t n);
    void     unlink(Level& level, uint32_t n);

    void matchBuy(Order& incoming, std::vector<Trade>& out);
    void matchSell(Order& incoming, std::vector<Trade>& out);
    bool canFill(const Order& incoming) const;
    void rest(const Order& order);

    void fixBestBidAfterEmpty();
    void fixBestAskAfterEmpty();

    Price minPrice_;
    Price maxPrice_;
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    // best level indexes. "none" = -1 for bids, levels count for asks
    int64_t bestBid_;
    int64_t bestAsk_;

    std::vector<Node> pool_;
    std::vector<uint32_t> freeList_;
    FlatHashMap index_;
    uint64_t nextSeq_ = 0;
};
