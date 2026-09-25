#pragma once

#include <cstdint>

using OrderId  = uint64_t;
using Price    = int64_t;   // price in cents (ticks). never double.
using Quantity = uint32_t;

enum class Side { Buy, Sell };
// Limit:  trade what crosses, rest the remainder
// Market: trade at any price, drop the remainder
// IOC:    immediate-or-cancel. like limit but the remainder is dropped
// FOK:    fill-or-kill. fill the whole thing at the limit price or do nothing
enum class OrderType { Limit, Market, IOC, FOK };

struct Order {
    OrderId   id;
    Side      side;
    OrderType type;
    Price     price;      // ignored for market orders
    Quantity  quantity;   // original size
    Quantity  remaining;  // what's still unfilled
    uint64_t  seq;        // arrival counter, used for time priority (FIFO)
};
