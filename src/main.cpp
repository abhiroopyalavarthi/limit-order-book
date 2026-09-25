#include <cstdio>

#include "OrderBook.h"

static void printTrades(const std::vector<Trade>& trades) {
    for (const Trade& t : trades)
        std::printf("  TRADE buy=%llu sell=%llu price=%.2f qty=%u\n",
                    (unsigned long long)t.buyId, (unsigned long long)t.sellId,
                    t.price / 100.0, t.quantity);
}

static void printPrice(const char* label, std::optional<Price> p) {
    if (p)
        std::printf("  %s %.2f", label, *p / 100.0);
    else
        std::printf("  %s  -  ", label);
}

static void printTop(const OrderBook& book) {
    printPrice("best bid:", book.bestBid());
    printPrice("best ask:", book.bestAsk());
    std::putchar('\n');
}

int main() {
    OrderBook book;

    std::puts("Resting asks: 100 @ 50.05 (id 1), 200 @ 50.10 (id 2)");
    book.addOrder(1, Side::Sell, OrderType::Limit, 5005, 100);
    book.addOrder(2, Side::Sell, OrderType::Limit, 5010, 200);
    std::puts("Resting bid: 150 @ 49.95 (id 3)");
    book.addOrder(3, Side::Buy, OrderType::Limit, 4995, 150);
    printTop(book);

    std::puts("\nBuy 250 @ 50.10 (id 4), sweeps two levels:");
    printTrades(book.addOrder(4, Side::Buy, OrderType::Limit, 5010, 250));
    printTop(book);

    std::puts("\nMarket sell 100 (id 5):");
    printTrades(book.addOrder(5, Side::Sell, OrderType::Market, 0, 100));
    printTop(book);
}
