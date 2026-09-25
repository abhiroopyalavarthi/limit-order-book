#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "Order.h"
#include "Trade.h"

// Version 1: std::map of price levels, each level a FIFO std::list of orders.
// Bids are sorted high -> low, asks low -> high, so begin() is always the best price.
class OrderBook {
public:
    // Adds an order and matches it against the other side.
    // Returns the trades it caused (empty if it just rested).
    // Only Limit orders rest. Market and IOC drop whatever doesn't fill,
    // FOK does nothing unless it can fill completely.
    // Duplicate IDs and zero quantity are rejected (no trades, nothing added).
    std::vector<Trade> addOrder(OrderId id, Side side, OrderType type,
                                Price price, Quantity qty);
    // Same, but appends trades to a caller-owned vector. Used by the benchmark.
    void addOrder(OrderId id, Side side, OrderType type, Price price,
                  Quantity qty, std::vector<Trade>& out);

    // Removes a resting order. Returns false if the id isn't in the book
    // (never existed, already cancelled, or already fully filled).
    bool cancelOrder(OrderId id);

    // Cancel + re-add with the new price/qty, so the order goes to the back
    // of the queue (loses time priority). Can trade if the new price crosses.
    // Returns nullopt if the id isn't in the book.
    std::optional<std::vector<Trade>> modifyOrder(OrderId id, Price newPrice,
                                                  Quantity newQty);
    bool modifyOrder(OrderId id, Price newPrice, Quantity newQty,
                     std::vector<Trade>& out);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    std::optional<Price> spread() const;

    Quantity volumeAt(Side side, Price price) const;  // total resting qty at a level
    size_t   orderCount() const { return index_.size(); }

private:
    using Level   = std::list<Order>;
    using BidMap  = std::map<Price, Level, std::greater<Price>>;
    using AskMap  = std::map<Price, Level, std::less<Price>>;

    // What we need to find an order again in O(1) for cancel.
    struct Location {
        Side side;
        Price price;
        Level::iterator it;
    };

    template <typename BookSide>
    void match(Order& incoming, BookSide& opposite, std::vector<Trade>& trades);

    template <typename BookSide>
    bool canFill(const Order& incoming, const BookSide& opposite) const;

    void rest(const Order& order);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Location> index_;
    uint64_t nextSeq_ = 0;
};
