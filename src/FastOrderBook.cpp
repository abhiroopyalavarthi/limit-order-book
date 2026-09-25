#include "FastOrderBook.h"

#include <algorithm>

FastOrderBook::FastOrderBook(Price minPrice, Price maxPrice,
                             size_t expectedOrders)
    : minPrice_(minPrice),
      maxPrice_(maxPrice),
      bids_(static_cast<size_t>(maxPrice - minPrice + 1)),
      asks_(static_cast<size_t>(maxPrice - minPrice + 1)),
      bestBid_(-1),
      bestAsk_(static_cast<int64_t>(asks_.size())),
      index_(expectedOrders) {
    pool_.reserve(expectedOrders);
    freeList_.reserve(expectedOrders);
}

// ---------- pool + intrusive list helpers ----------

uint32_t FastOrderBook::allocNode(const Order& o) {
    uint32_t n;
    if (!freeList_.empty()) {
        n = freeList_.back();
        freeList_.pop_back();
        pool_[n] = Node{o, kNil, kNil};
    } else {
        n = static_cast<uint32_t>(pool_.size());
        pool_.push_back(Node{o, kNil, kNil});
    }
    return n;
}

void FastOrderBook::freeNode(uint32_t n) { freeList_.push_back(n); }

void FastOrderBook::pushBack(Level& level, uint32_t n) {
    Node& node = pool_[n];
    node.prev = level.tail;
    node.next = kNil;
    if (level.tail != kNil)
        pool_[level.tail].next = n;
    else
        level.head = n;
    level.tail = n;
    level.total += node.order.remaining;
}

void FastOrderBook::unlink(Level& level, uint32_t n) {
    Node& node = pool_[n];
    if (node.prev != kNil) pool_[node.prev].next = node.next;
    else level.head = node.next;
    if (node.next != kNil) pool_[node.next].prev = node.prev;
    else level.tail = node.prev;
    level.total -= node.order.remaining;
}

// ---------- best price tracking ----------
// When the best level empties, walk the array to the next non-empty one.

void FastOrderBook::fixBestBidAfterEmpty() {
    while (bestBid_ >= 0 && bids_[bestBid_].head == kNil) --bestBid_;
}

void FastOrderBook::fixBestAskAfterEmpty() {
    const int64_t n = static_cast<int64_t>(asks_.size());
    while (bestAsk_ < n && asks_[bestAsk_].head == kNil) ++bestAsk_;
}

// ---------- add / match ----------

std::vector<Trade> FastOrderBook::addOrder(OrderId id, Side side,
                                           OrderType type, Price price,
                                           Quantity qty) {
    std::vector<Trade> trades;
    addOrder(id, side, type, price, qty, trades);
    return trades;
}

void FastOrderBook::addOrder(OrderId id, Side side, OrderType type,
                             Price price, Quantity qty,
                             std::vector<Trade>& out) {
    if (qty == 0 || id == FlatHashMap::kEmpty || index_.contains(id)) return;
    if (type != OrderType::Market && !inRange(price)) return;

    Order order{id, side, type, price, qty, qty, nextSeq_++};

    if (type == OrderType::FOK && !canFill(order)) return;

    if (side == Side::Buy)
        matchBuy(order, out);
    else
        matchSell(order, out);

    if (order.remaining > 0 && type == OrderType::Limit) rest(order);
}

void FastOrderBook::matchBuy(Order& in, std::vector<Trade>& out) {
    const int64_t n = static_cast<int64_t>(asks_.size());
    const bool market = in.type == OrderType::Market;
    const int64_t limit = market ? n - 1 : static_cast<int64_t>(toIdx(in.price));

    while (in.remaining > 0 && bestAsk_ < n && bestAsk_ <= limit) {
        Level& level = asks_[bestAsk_];
        while (in.remaining > 0 && level.head != kNil) {
            uint32_t r = level.head;
            Order& resting = pool_[r].order;
            Quantity fill = std::min(in.remaining, resting.remaining);
            out.push_back(Trade{in.id, resting.id, resting.price, fill});
            in.remaining -= fill;
            resting.remaining -= fill;
            level.total -= fill;
            if (resting.remaining == 0) {
                level.head = pool_[r].next;
                if (level.head != kNil) pool_[level.head].prev = kNil;
                else level.tail = kNil;
                index_.erase(resting.id);
                freeNode(r);
            }
        }
        if (level.head == kNil) fixBestAskAfterEmpty();
    }
}

void FastOrderBook::matchSell(Order& in, std::vector<Trade>& out) {
    const bool market = in.type == OrderType::Market;
    const int64_t limit = market ? 0 : static_cast<int64_t>(toIdx(in.price));

    while (in.remaining > 0 && bestBid_ >= 0 && bestBid_ >= limit) {
        Level& level = bids_[bestBid_];
        while (in.remaining > 0 && level.head != kNil) {
            uint32_t r = level.head;
            Order& resting = pool_[r].order;
            Quantity fill = std::min(in.remaining, resting.remaining);
            out.push_back(Trade{resting.id, in.id, resting.price, fill});
            in.remaining -= fill;
            resting.remaining -= fill;
            level.total -= fill;
            if (resting.remaining == 0) {
                level.head = pool_[r].next;
                if (level.head != kNil) pool_[level.head].prev = kNil;
                else level.tail = kNil;
                index_.erase(resting.id);
                freeNode(r);
            }
        }
        if (level.head == kNil) fixBestBidAfterEmpty();
    }
}

// FOK: level totals make this a sum over levels, not over orders.
bool FastOrderBook::canFill(const Order& in) const {
    uint64_t needed = in.remaining;
    const int64_t limit = static_cast<int64_t>(toIdx(in.price));
    if (in.side == Side::Buy) {
        const int64_t n = static_cast<int64_t>(asks_.size());
        for (int64_t i = bestAsk_; i < n && i <= limit; ++i) {
            if (asks_[i].total >= needed) return true;
            needed -= asks_[i].total;
        }
    } else {
        for (int64_t i = bestBid_; i >= 0 && i >= limit; --i) {
            if (bids_[i].total >= needed) return true;
            needed -= bids_[i].total;
        }
    }
    return false;
}

void FastOrderBook::rest(const Order& order) {
    uint32_t n = allocNode(order);
    index_.insert(order.id, n);
    int64_t idx = static_cast<int64_t>(toIdx(order.price));
    if (order.side == Side::Buy) {
        pushBack(bids_[idx], n);
        if (idx > bestBid_) bestBid_ = idx;
    } else {
        pushBack(asks_[idx], n);
        if (idx < bestAsk_) bestAsk_ = idx;
    }
}

// ---------- cancel / modify ----------

bool FastOrderBook::cancelOrder(OrderId id) {
    const uint32_t* found = index_.find(id);
    if (!found) return false;
    uint32_t n = *found;
    const Order& o = pool_[n].order;
    int64_t idx = static_cast<int64_t>(toIdx(o.price));

    if (o.side == Side::Buy) {
        unlink(bids_[idx], n);
        if (idx == bestBid_ && bids_[idx].head == kNil) fixBestBidAfterEmpty();
    } else {
        unlink(asks_[idx], n);
        if (idx == bestAsk_ && asks_[idx].head == kNil) fixBestAskAfterEmpty();
    }
    index_.erase(id);
    freeNode(n);
    return true;
}

bool FastOrderBook::modifyOrder(OrderId id, Price newPrice, Quantity newQty,
                                std::vector<Trade>& out) {
    const uint32_t* found = index_.find(id);
    if (!found) return false;
    Side side = pool_[*found].order.side;
    cancelOrder(id);
    if (newQty > 0) addOrder(id, side, OrderType::Limit, newPrice, newQty, out);
    return true;
}

std::optional<std::vector<Trade>> FastOrderBook::modifyOrder(OrderId id,
                                                             Price newPrice,
                                                             Quantity newQty) {
    std::vector<Trade> trades;
    if (!modifyOrder(id, newPrice, newQty, trades)) return std::nullopt;
    return trades;
}

// ---------- queries ----------

std::optional<Price> FastOrderBook::bestBid() const {
    if (bestBid_ < 0) return std::nullopt;
    return minPrice_ + bestBid_;
}

std::optional<Price> FastOrderBook::bestAsk() const {
    if (bestAsk_ >= static_cast<int64_t>(asks_.size())) return std::nullopt;
    return minPrice_ + bestAsk_;
}

std::optional<Price> FastOrderBook::spread() const {
    auto bid = bestBid();
    auto ask = bestAsk();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}

Quantity FastOrderBook::volumeAt(Side side, Price price) const {
    if (!inRange(price)) return 0;
    const Level& level = side == Side::Buy ? bids_[toIdx(price)] : asks_[toIdx(price)];
    return static_cast<Quantity>(level.total);
}
