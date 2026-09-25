#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Order.h"
#include "Trade.h"

// One order book per ticker. Books are completely independent, which is
// also what makes this easy to scale: different symbols can live on
// different threads (or machines) without any locking between them.
template <typename Book>
class MatchingEngine {
public:
    // Creates the book for a symbol. Extra args go to the Book constructor
    // (FastOrderBook needs a price band per symbol).
    template <typename... Args>
    Book& addSymbol(const std::string& symbol, Args&&... args) {
        auto [it, inserted] = books_.try_emplace(symbol, std::forward<Args>(args)...);
        return it->second;
    }

    Book* book(const std::string& symbol) {
        auto it = books_.find(symbol);
        return it == books_.end() ? nullptr : &it->second;
    }

    // Returns false if the symbol isn't known.
    bool addOrder(const std::string& symbol, OrderId id, Side side, OrderType type,
                  Price price, Quantity qty, std::vector<Trade>& out) {
        Book* b = book(symbol);
        if (!b) return false;
        b->addOrder(id, side, type, price, qty, out);
        return true;
    }

    bool cancelOrder(const std::string& symbol, OrderId id) {
        Book* b = book(symbol);
        return b && b->cancelOrder(id);
    }

    bool modifyOrder(const std::string& symbol, OrderId id, Price price,
                     Quantity qty, std::vector<Trade>& out) {
        Book* b = book(symbol);
        return b && b->modifyOrder(id, price, qty, out);
    }

    const std::map<std::string, Book>& books() const { return books_; }

private:
    std::map<std::string, Book> books_;  // ordered so reports print A-Z
};
