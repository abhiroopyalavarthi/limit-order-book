#include <gtest/gtest.h>

#include "FastOrderBook.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "OrderGenerator.h"

#include <random>
#include <unordered_map>

// Every test below runs twice: once on the std::map version (OrderBook)
// and once on the array + pool version (FastOrderBook).
// Prices are in cents: 5000 = $50.00

template <typename T>
class BookTest : public ::testing::Test {};

using BookTypes = ::testing::Types<OrderBook, FastOrderBook>;
TYPED_TEST_SUITE(BookTest, BookTypes);

TYPED_TEST(BookTest, EmptyBookHasNoTopOfBook) {
    TypeParam book;
    EXPECT_FALSE(book.bestBid());
    EXPECT_FALSE(book.bestAsk());
    EXPECT_FALSE(book.spread());
}

TYPED_TEST(BookTest, NonCrossingOrdersRest) {
    TypeParam book;
    EXPECT_TRUE(book.addOrder(1, Side::Buy, OrderType::Limit, 5000, 100).empty());
    EXPECT_TRUE(book.addOrder(2, Side::Sell, OrderType::Limit, 5005, 100).empty());
    EXPECT_EQ(book.bestBid(), 5000);
    EXPECT_EQ(book.bestAsk(), 5005);
    EXPECT_EQ(book.spread(), 5);
    EXPECT_EQ(book.orderCount(), 2u);
}

TYPED_TEST(BookTest, BestBidIsHighestAndBestAskIsLowest) {
    TypeParam book;
    book.addOrder(1, Side::Buy, OrderType::Limit, 4990, 10);
    book.addOrder(2, Side::Buy, OrderType::Limit, 4995, 10);
    book.addOrder(3, Side::Sell, OrderType::Limit, 5010, 10);
    book.addOrder(4, Side::Sell, OrderType::Limit, 5005, 10);
    EXPECT_EQ(book.bestBid(), 4995);
    EXPECT_EQ(book.bestAsk(), 5005);
}

TYPED_TEST(BookTest, TradeHappensAtRestingPrice) {
    TypeParam book;
    book.addOrder(1, Side::Sell, OrderType::Limit, 5005, 100);
    auto trades = book.addOrder(2, Side::Buy, OrderType::Limit, 5010, 100);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buyId, 2u);
    EXPECT_EQ(trades[0].sellId, 1u);
    EXPECT_EQ(trades[0].price, 5005);  // not 5010
    EXPECT_EQ(trades[0].quantity, 100u);
    EXPECT_EQ(book.orderCount(), 0u);
}

TYPED_TEST(BookTest, FillsAcrossSeveralPriceLevels) {
    TypeParam book;
    book.addOrder(1, Side::Sell, OrderType::Limit, 5005, 100);
    book.addOrder(2, Side::Sell, OrderType::Limit, 5010, 100);
    book.addOrder(3, Side::Sell, OrderType::Limit, 5020, 100);

    auto trades = book.addOrder(4, Side::Buy, OrderType::Limit, 5010, 250);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].sellId, 1u);
    EXPECT_EQ(trades[0].price, 5005);
    EXPECT_EQ(trades[1].sellId, 2u);
    EXPECT_EQ(trades[1].price, 5010);

    // 50 left over rests as the new best bid; 5020 ask untouched
    EXPECT_EQ(book.bestBid(), 5010);
    EXPECT_EQ(book.volumeAt(Side::Buy, 5010), 50u);
    EXPECT_EQ(book.bestAsk(), 5020);
}

TYPED_TEST(BookTest, SamePriceFillsInArrivalOrder) {
    TypeParam book;
    book.addOrder(1, Side::Buy, OrderType::Limit, 5000, 100);
    book.addOrder(2, Side::Buy, OrderType::Limit, 5000, 100);
    book.addOrder(3, Side::Buy, OrderType::Limit, 5000, 100);

    auto trades = book.addOrder(4, Side::Sell, OrderType::Limit, 5000, 150);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].buyId, 1u);
    EXPECT_EQ(trades[0].quantity, 100u);
    EXPECT_EQ(trades[1].buyId, 2u);
    EXPECT_EQ(trades[1].quantity, 50u);
    EXPECT_EQ(book.volumeAt(Side::Buy, 5000), 150u);  // 50 of #2 + 100 of #3
}

TYPED_TEST(BookTest, PartialFillLeavesRestingRemainder) {
    TypeParam book;
    book.addOrder(1, Side::Sell, OrderType::Limit, 5005, 300);
    book.addOrder(2, Side::Buy, OrderType::Limit, 5005, 100);
    EXPECT_EQ(book.volumeAt(Side::Sell, 5005), 200u);
    EXPECT_FALSE(book.bestBid());
}

TYPED_TEST(BookTest, CancelRemovesOrder) {
    TypeParam book;
    book.addOrder(1, Side::Buy, OrderType::Limit, 5000, 100);
    EXPECT_TRUE(book.cancelOrder(1));
    EXPECT_FALSE(book.bestBid());
    EXPECT_FALSE(book.cancelOrder(1));  // second cancel fails
}

TYPED_TEST(BookTest, CancelUnknownIdFails) {
    TypeParam book;
    EXPECT_FALSE(book.cancelOrder(42));
}

TYPED_TEST(BookTest, CancelFullyFilledOrderFails) {
    TypeParam book;
    book.addOrder(1, Side::Sell, OrderType::Limit, 5005, 100);
    book.addOrder(2, Side::Buy, OrderType::Limit, 5005, 100);
    EXPECT_FALSE(book.cancelOrder(1));
    EXPECT_FALSE(book.cancelOrder(2));
}

TYPED_TEST(BookTest, CancelMiddleOfQueueKeepsOthersInOrder) {
    TypeParam book;
    book.addOrder(1, Side::Sell, OrderType::Limit, 5005, 10);
    book.addOrder(2, Side::Sell, OrderType::Limit, 5005, 10);
    book.addOrder(3, Side::Sell, OrderType::Limit, 5005, 10);
    book.cancelOrder(2);

    auto trades = book.addOrder(4, Side::Buy, OrderType::Limit, 5005, 20);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].sellId, 1u);
    EXPECT_EQ(trades[1].sellId, 3u);
}

TYPED_TEST(BookTest, MarketOrderAgainstEmptySideDoesNothing) {
    TypeParam book;
    auto trades = book.addOrder(1, Side::Buy, OrderType::Market, 0, 100);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.orderCount(), 0u);  // market orders never rest
}

TYPED_TEST(BookTest, MarketOrderSweepsAndDropsRemainder) {
    TypeParam book;
    book.addOrder(1, Side::Buy, OrderType::Limit, 5000, 50);
    book.addOrder(2, Side::Buy, OrderType::Limit, 4990, 50);

    auto trades = book.addOrder(3, Side::Sell, OrderType::Market, 0, 200);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 5000);
    EXPECT_EQ(trades[1].price, 4990);
    EXPECT_EQ(book.orderCount(), 0u);
    EXPECT_FALSE(book.bestAsk());
}

TYPED_TEST(BookTest, ModifyLosesTimePriority) {
    TypeParam book;
    book.addOrder(1, Side::Buy, OrderType::Limit, 5000, 100);
    book.addOrder(2, Side::Buy, OrderType::Limit, 5000, 100);

    // Move #1 away and back: it should now be behind #2.
    ASSERT_TRUE(book.modifyOrder(1, 4990, 100));
    ASSERT_TRUE(book.modifyOrder(1, 5000, 100));

    auto trades = book.addOrder(3, Side::Sell, OrderType::Limit, 5000, 100);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buyId, 2u);
}

TYPED_TEST(BookTest, ModifyIntoCrossTrades) {
    TypeParam book;
    book.addOrder(1, Side::Sell, OrderType::Limit, 5005, 100);
    book.addOrder(2, Side::Buy, OrderType::Limit, 5000, 100);

    auto trades = book.modifyOrder(2, 5005, 100);
    ASSERT_TRUE(trades);
    ASSERT_EQ(trades->size(), 1u);
    EXPECT_EQ(book.orderCount(), 0u);
}

TYPED_TEST(BookTest, ModifyUnknownIdFails) {
    TypeParam book;
    EXPECT_FALSE(book.modifyOrder(99, 5000, 10));
}

TYPED_TEST(BookTest, DuplicateIdIsRejected) {
    TypeParam book;
    book.addOrder(1, Side::Buy, OrderType::Limit, 5000, 100);
    book.addOrder(1, Side::Buy, OrderType::Limit, 4990, 100);
    EXPECT_EQ(book.orderCount(), 1u);
    EXPECT_EQ(book.volumeAt(Side::Buy, 4990), 0u);
}

TYPED_TEST(BookTest, ZeroQuantityIsRejected) {
    TypeParam book;
    book.addOrder(1, Side::Buy, OrderType::Limit, 5000, 0);
    EXPECT_EQ(book.orderCount(), 0u);
}

// ---------- IOC / FOK ----------

TYPED_TEST(BookTest, IocFillsWhatItCanAndDropsTheRest) {
    TypeParam book;
    book.addOrder(1, Side::Sell, OrderType::Limit, 5005, 100);
    auto trades = book.addOrder(2, Side::Buy, OrderType::IOC, 5005, 300);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 100u);
    EXPECT_FALSE(book.bestBid());  // the other 200 did not rest
    EXPECT_EQ(book.orderCount(), 0u);
}

TYPED_TEST(BookTest, IocRespectsLimitPrice) {
    TypeParam book;
    book.addOrder(1, Side::Sell, OrderType::Limit, 5010, 100);
    auto trades = book.addOrder(2, Side::Buy, OrderType::IOC, 5005, 100);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.volumeAt(Side::Sell, 5010), 100u);
}

TYPED_TEST(BookTest, FokFillsCompletelyAcrossLevels) {
    TypeParam book;
    book.addOrder(1, Side::Sell, OrderType::Limit, 5005, 100);
    book.addOrder(2, Side::Sell, OrderType::Limit, 5010, 100);
    auto trades = book.addOrder(3, Side::Buy, OrderType::FOK, 5010, 200);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(book.orderCount(), 0u);
}

TYPED_TEST(BookTest, FokKilledIfNotEnoughQuantity) {
    TypeParam book;
    book.addOrder(1, Side::Sell, OrderType::Limit, 5005, 100);
    book.addOrder(2, Side::Sell, OrderType::Limit, 5020, 100);  // above the limit
    auto trades = book.addOrder(3, Side::Buy, OrderType::FOK, 5010, 150);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.volumeAt(Side::Sell, 5005), 100u);  // book untouched
    EXPECT_EQ(book.orderCount(), 2u);
}

TYPED_TEST(BookTest, FokSellSide) {
    TypeParam book;
    book.addOrder(1, Side::Buy, OrderType::Limit, 5000, 60);
    book.addOrder(2, Side::Buy, OrderType::Limit, 4995, 60);
    EXPECT_TRUE(book.addOrder(3, Side::Sell, OrderType::FOK, 5000, 100).empty());
    EXPECT_EQ(book.addOrder(4, Side::Sell, OrderType::FOK, 4995, 100).size(), 2u);
    EXPECT_EQ(book.volumeAt(Side::Buy, 4995), 20u);
}

// ---------- V2-specific ----------

TEST(FastOrderBook, RejectsPricesOutsideBand) {
    FastOrderBook book(1000, 2000);
    book.addOrder(1, Side::Buy, OrderType::Limit, 999, 10);
    book.addOrder(2, Side::Sell, OrderType::Limit, 2001, 10);
    EXPECT_EQ(book.orderCount(), 0u);
    book.addOrder(3, Side::Buy, OrderType::Limit, 1000, 10);
    book.addOrder(4, Side::Sell, OrderType::Limit, 2000, 10);
    EXPECT_EQ(book.bestBid(), 1000);
    EXPECT_EQ(book.bestAsk(), 2000);
}

TEST(FastOrderBook, BestPriceMovesWhenLevelEmpties) {
    FastOrderBook book;
    book.addOrder(1, Side::Buy, OrderType::Limit, 5000, 10);
    book.addOrder(2, Side::Buy, OrderType::Limit, 4900, 10);
    book.cancelOrder(1);
    EXPECT_EQ(book.bestBid(), 4900);
    book.cancelOrder(2);
    EXPECT_FALSE(book.bestBid());
}

TEST(FastOrderBook, PoolSlotsGetReused) {
    FastOrderBook book;
    for (OrderId id = 1; id <= 1000; ++id) {
        book.addOrder(id, Side::Buy, OrderType::Limit, 5000, 10);
        book.cancelOrder(id);
    }
    EXPECT_EQ(book.orderCount(), 0u);
    book.addOrder(5000, Side::Buy, OrderType::Limit, 5000, 10);
    EXPECT_EQ(book.volumeAt(Side::Buy, 5000), 10u);
}

// ---------- differential fuzz test ----------
// Feed the same random stream into both books. They must produce exactly the
// same trades and the same top of book at every step. V1 is simple enough to
// trust, so this is how V2 is checked on cases nobody wrote a test for.

TEST(Differential, BothBooksAgreeOnRandomStreams) {
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        auto ops = generateOps(20'000, seed);
        OrderBook v1;
        FastOrderBook v2;
        std::vector<Trade> t1, t2;
        for (size_t i = 0; i < ops.size(); ++i) {
            t1.clear();
            t2.clear();
            applyOp(v1, ops[i], t1);
            applyOp(v2, ops[i], t2);
            ASSERT_EQ(t1.size(), t2.size()) << "seed " << seed << " op " << i;
            for (size_t k = 0; k < t1.size(); ++k) {
                ASSERT_EQ(t1[k].buyId, t2[k].buyId);
                ASSERT_EQ(t1[k].sellId, t2[k].sellId);
                ASSERT_EQ(t1[k].price, t2[k].price);
                ASSERT_EQ(t1[k].quantity, t2[k].quantity);
            }
            ASSERT_EQ(v1.bestBid(), v2.bestBid()) << "seed " << seed << " op " << i;
            ASSERT_EQ(v1.bestAsk(), v2.bestAsk()) << "seed " << seed << " op " << i;
            ASSERT_EQ(v1.orderCount(), v2.orderCount());
        }
    }
}

// ---------- FlatHashMap ----------
// Erase with backward shift is the easiest thing to get wrong, so force long
// collision chains and check every key is still found after deletes.

TEST(FlatHashMap, EraseKeepsCollidingKeysReachable) {
    FlatHashMap map(8);  // 16 slots
    // with either hash, a few hundred keys in a small table means lots of
    // collisions and several grows
    for (uint64_t k = 1; k <= 500; ++k) ASSERT_TRUE(map.insert(k * 16, uint32_t(k)));
    for (uint64_t k = 1; k <= 500; k += 3) ASSERT_TRUE(map.erase(k * 16));
    for (uint64_t k = 1; k <= 500; ++k) {
        const uint32_t* v = map.find(k * 16);
        if (k % 3 == 1) {
            EXPECT_EQ(v, nullptr) << k;
        } else {
            ASSERT_NE(v, nullptr) << k;
            EXPECT_EQ(*v, k);
        }
    }
    EXPECT_FALSE(map.insert(32, 99));  // duplicate
    EXPECT_FALSE(map.erase(16));       // already gone
}

TEST(FlatHashMap, RandomOpsMatchStdUnorderedMap) {
    FlatHashMap map;
    std::unordered_map<uint64_t, uint32_t> ref;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 200'000; ++i) {
        uint64_t k = rng() % 5000;
        switch (rng() % 3) {
            case 0: EXPECT_EQ(map.insert(k, uint32_t(i)), ref.emplace(k, uint32_t(i)).second); break;
            case 1: EXPECT_EQ(map.erase(k), ref.erase(k) == 1); break;
            case 2: {
                const uint32_t* v = map.find(k);
                auto it = ref.find(k);
                ASSERT_EQ(v != nullptr, it != ref.end());
                if (v) EXPECT_EQ(*v, it->second);
            }
        }
    }
    EXPECT_EQ(map.size(), ref.size());
}

// ---------- multi-symbol ----------

TEST(MatchingEngine, SymbolsAreIndependent) {
    MatchingEngine<FastOrderBook> engine;
    engine.addSymbol("AAPL", 10'000, 30'000);
    engine.addSymbol("MSFT", 30'000, 60'000);
    std::vector<Trade> trades;

    engine.addOrder("AAPL", 1, Side::Sell, OrderType::Limit, 18955, 100, trades);
    // same price on the other symbol must not trade with AAPL
    engine.addOrder("MSFT", 2, Side::Buy, OrderType::Limit, 41230, 100, trades);
    EXPECT_TRUE(trades.empty());

    engine.addOrder("AAPL", 3, Side::Buy, OrderType::Limit, 18955, 40, trades);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellId, 1u);
    EXPECT_EQ(engine.book("AAPL")->volumeAt(Side::Sell, 18955), 60u);
    EXPECT_EQ(engine.book("MSFT")->bestBid(), 41230);
}

TEST(MatchingEngine, UnknownSymbolIsRejected) {
    MatchingEngine<OrderBook> engine;
    std::vector<Trade> trades;
    EXPECT_FALSE(engine.addOrder("TSLA", 1, Side::Buy, OrderType::Limit, 100, 1, trades));
    EXPECT_FALSE(engine.cancelOrder("TSLA", 1));
    EXPECT_EQ(engine.book("TSLA"), nullptr);
}
