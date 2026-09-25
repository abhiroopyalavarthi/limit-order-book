#include "OrderBook.h"

#include <algorithm>

namespace {

// Does the incoming order's limit allow trading at this resting price?
bool crosses(const Order& incoming, Price restingPrice) {
    if (incoming.type == OrderType::Market) return true;
    if (incoming.side == Side::Buy) return incoming.price >= restingPrice;
    return incoming.price <= restingPrice;
}

}  // namespace

std::vector<Trade> OrderBook::addOrder(OrderId id, Side side, OrderType type,
                                       Price price, Quantity qty) {
    std::vector<Trade> trades;
    addOrder(id, side, type, price, qty, trades);
    return trades;
}

void OrderBook::addOrder(OrderId id, Side side, OrderType type, Price price,
                         Quantity qty, std::vector<Trade>& trades) {
    if (qty == 0 || index_.count(id)) return;

    Order order{id, side, type, price, qty, qty, nextSeq_++};

    if (type == OrderType::FOK) {
        bool ok = side == Side::Buy ? canFill(order, asks_) : canFill(order, bids_);
        if (!ok) return;
    }

    if (side == Side::Buy)
        match(order, asks_, trades);
    else
        match(order, bids_, trades);

    if (order.remaining > 0 && type == OrderType::Limit) rest(order);
}

template <typename BookSide>
void OrderBook::match(Order& incoming, BookSide& opposite,
                      std::vector<Trade>& trades) {
    // Walk price levels best-first until we're filled or prices stop crossing.
    while (incoming.remaining > 0 && !opposite.empty()) {
        auto levelIt = opposite.begin();
        if (!crosses(incoming, levelIt->first)) break;

        Level& level = levelIt->second;
        // Inside a level, oldest order is at the front (FIFO).
        while (incoming.remaining > 0 && !level.empty()) {
            Order& resting = level.front();
            Quantity fill = std::min(incoming.remaining, resting.remaining);

            bool incomingIsBuy = incoming.side == Side::Buy;
            trades.push_back(Trade{
                incomingIsBuy ? incoming.id : resting.id,
                incomingIsBuy ? resting.id : incoming.id,
                resting.price,
                fill});

            incoming.remaining -= fill;
            resting.remaining -= fill;

            if (resting.remaining == 0) {
                index_.erase(resting.id);
                level.pop_front();
            }
        }
        if (level.empty()) opposite.erase(levelIt);
    }
}

// FOK check: is there enough quantity at acceptable prices?
// Walks levels without changing anything.
template <typename BookSide>
bool OrderBook::canFill(const Order& incoming, const BookSide& opposite) const {
    Quantity needed = incoming.remaining;
    for (const auto& [price, level] : opposite) {
        if (!crosses(incoming, price)) break;
        for (const Order& o : level) {
            if (o.remaining >= needed) return true;
            needed -= o.remaining;
        }
    }
    return false;
}

void OrderBook::rest(const Order& order) {
    if (order.side == Side::Buy) {
        Level& level = bids_[order.price];
        level.push_back(order);
        index_[order.id] = {order.side, order.price, std::prev(level.end())};
    } else {
        Level& level = asks_[order.price];
        level.push_back(order);
        index_[order.id] = {order.side, order.price, std::prev(level.end())};
    }
}

bool OrderBook::cancelOrder(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;

    const Location& loc = found->second;
    if (loc.side == Side::Buy) {
        auto levelIt = bids_.find(loc.price);
        levelIt->second.erase(loc.it);
        if (levelIt->second.empty()) bids_.erase(levelIt);
    } else {
        auto levelIt = asks_.find(loc.price);
        levelIt->second.erase(loc.it);
        if (levelIt->second.empty()) asks_.erase(levelIt);
    }
    index_.erase(found);
    return true;
}

bool OrderBook::modifyOrder(OrderId id, Price newPrice, Quantity newQty,
                            std::vector<Trade>& out) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;

    Side side = found->second.side;
    cancelOrder(id);
    // modify to 0 = plain cancel
    if (newQty > 0) addOrder(id, side, OrderType::Limit, newPrice, newQty, out);
    return true;
}

std::optional<std::vector<Trade>> OrderBook::modifyOrder(OrderId id,
                                                         Price newPrice,
                                                         Quantity newQty) {
    std::vector<Trade> trades;
    if (!modifyOrder(id, newPrice, newQty, trades)) return std::nullopt;
    return trades;
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const {
    auto bid = bestBid();
    auto ask = bestAsk();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}

Quantity OrderBook::volumeAt(Side side, Price price) const {
    auto sum = [](const Level& level) {
        Quantity total = 0;
        for (const Order& o : level) total += o.remaining;
        return total;
    };
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : sum(it->second);
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : sum(it->second);
}
