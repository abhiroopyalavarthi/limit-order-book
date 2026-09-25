#pragma once

#include "Order.h"

struct Trade {
    OrderId  buyId;
    OrderId  sellId;
    Price    price;     // always the resting order's price
    Quantity quantity;
};
