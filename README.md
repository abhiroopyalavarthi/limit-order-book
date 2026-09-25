# Limit Order Book Matching Engine (C++20)

[![tests](https://github.com/abhiroopyalavarthi/limit-order-book/actions/workflows/tests.yml/badge.svg)](https://github.com/abhiroopyalavarthi/limit-order-book/actions/workflows/tests.yml)

A matching engine for a limit order book with price-time priority. It supports limit, market, IOC and FOK orders, cancel and modify by order ID, partial fills with a trade report per fill, and one book per symbol.

There are two implementations with the same interface:

- **V1 (`OrderBook`)**: `std::map` price levels holding FIFO `std::list`s. Simple and easy to trust.
- **V2 (`FastOrderBook`)**: a flat array of price levels indexed by tick, orders in a preallocated pool, and an open-addressing hash map for order IDs.

The test suite runs every case against both books, and a differential test feeds the same random streams into both and checks that every trade matches.

## Build and run

```bash
cmake -B build
cmake --build build
./build/tests                              # 54 GoogleTest cases
./build/demo                               # small walkthrough
./build/bench                              # V1 vs V2, 1M and 10M ops
./build/replay data/sample_orders.csv      # replay an order file
```

Needs CMake 3.20+ and a C++20 compiler. GoogleTest is downloaded at configure time.

## How matching works

- A buy crosses if its price is >= the best ask, and a sell crosses if its price is <= the best bid. Market orders always cross.
- The best price fills first. At the same price, the earliest order fills first (FIFO).
- A trade happens at the **resting** order's price.
- What's left of a limit order rests in the book. Market and IOC orders drop the remainder. A FOK order checks first whether it can fill completely, and does nothing if it can't.
- Modify is a cancel plus a re-add, so the order goes to the back of the queue at its new price.
- Prices are `int64_t` cents. A `double` can't represent 0.1 exactly, so two prices that should be equal can compare unequal, and a price level keyed by a `double` could split in two. The replay tool parses `"189.52"` straight into `18952` without going through floating point.

## Data structures

### V1: `std::map<Price, std::list<Order>>` per side

Bids use `std::greater` and asks use `std::less`, so `begin()` is always the best price. `std::unordered_map<OrderId, {side, price, list iterator}>` finds an order for cancel without searching its queue.

| Operation | V1 | V2 |
|---|---|---|
| Add, no match | O(log L) map lookup, plus a heap allocation for the list node | O(1) array index, plus a pool slot |
| Cancel | O(1) to find the order, plus O(log L) to find its level | O(1) |
| Match, per fill | O(1), plus O(log L) to erase an emptied level | O(1) |
| Next best after a level empties | O(1) (`begin()`) | O(gap) scan to the next non-empty tick |
| Best bid / ask | O(1) | O(1) |

L = number of price levels on that side.

### V2: tick-indexed array, pool and flat hash map

- **Price levels:** `bids_[price - minPrice]` is a direct index. Each level stores head and tail indices into the pool plus its total quantity, so `volumeAt` and the FOK check don't walk the orders.
- **Order pool:** one `std::vector<Node>`. Each order is linked to its neighbors by 32-bit index rather than by pointer, so the vector can grow without invalidating links. Freed slots go on a free list and get reused. There is no `new` per order.
- **ID map:** `FlatHashMap` uses open addressing with linear probing in one flat array. Erase uses backward shift instead of tombstones, so the probe chains don't degrade over time.
- **Trade-off:** the price band has to be fixed up front, and orders outside it are rejected. That's normal for a real exchange, which has a tick size and price bands. A symbol whose price drifts a long way would need the array re-centered.

## Benchmark

`bench` generates a stream of operations before the clock starts: 60% limit orders, 5% market, 3% IOC, 2% FOK, 22% cancels and 8% modifies. Prices are normally distributed around a mid price. Cancels and modifies target random earlier IDs, so some of them hit orders that have already filled.

- **Throughput:** the whole loop is timed, best of 3 runs.
- **Latency:** each operation is timed separately with `steady_clock`, which adds tens of nanoseconds of clock overhead per sample.
- **Check:** both versions end with the same trade count and resting-order count.

Results on a 13" MacBook Pro (M2, 8 GB, macOS 27), Apple clang, Release build (`-O3`), seed 42:

| Ops | Version | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| 1M | V1 map + list | 14.4 M ops/s | 83 ns | 333 ns | 667 ns |
| 1M | V2 array + pool | **36.0 M ops/s** | 42 ns | 167 ns | 750 ns |
| 10M | V1 map + list | 9.6 M ops/s | 83 ns | 541 ns | 834 ns |
| 10M | V2 array + pool | **29.5 M ops/s** | 42 ns | 250 ns | 2,583 ns |

At 10M ops V2 has **3.1x the throughput and half the p99 latency** of V1. The gap grows with book size: at 10M ops about 875k orders are resting, and V1's tree nodes and list nodes are scattered across the heap, while V2's levels and pool stay in a few contiguous arrays.

Two caveats:

- `steady_clock` on Apple Silicon ticks every ~41.7 ns, so the p50 values (42 and 83 ns) are one and two clock ticks, not exact measurements. Throughput is the more reliable comparison; p99 and above are well above the tick size.
- V1's worst case (max ~49 ms at 10M ops) comes from `std::unordered_map` rehashing all the IDs at once. V2's max was 0.7 ms.

## Day 6: profiling

V2 was profiled with `valgrind --tool=callgrind --cache-sim=yes` on a 300k-op run (`./build/bench 300000 profile`).

No function stood out by instruction count, so the cost was memory access. With cache simulation on, the order-ID lookup inside `addOrder` caused **about 32% of all L1 read misses** in the program. Every add probes the ID map to reject duplicates. The map used a splitmix64 hash, which deliberately scatters sequential IDs across the table, so every probe landed on a random cache line in a table with millions of slots.

The fix: order IDs are assigned in increasing order, so the ID itself is used as the hash. Orders that arrived close together sit in neighboring slots, and most adds, fills and cancels touch recent orders.

| | L1 read misses in `addOrder` ID lookups (callgrind, 300k ops) | V2 throughput, 10M ops (M2 MacBook Pro) |
|---|---|---|
| splitmix64 hash | 211,519 | 22.4 M ops/s |
| identity hash | 52,326 (-75%) | 29.5 M ops/s (+32%) |

One cost showed up: in the Mac run, V2's p99.9 went from 375 ns with the mixed hash to 2.6 µs with the identity hash, while p50 and p99 stayed the same. I haven't tracked down why yet. One possibility is long linear-probe runs once the IDs wrap past the table size, because an old resting order and a new one then share a home slot inside a long block of occupied slots. The next step is to count probe lengths and run each version several times to rule out noise.

Cancels still miss, as expected: a cancel targets a random old order, and no hash function makes that local. The downside is that IDs that are all multiples of a large power of two would collide, which is fine for exchange-assigned IDs but not for arbitrary client IDs. To reproduce the comparison:

```bash
cmake -B build-mix -DORDERBOOK_MIXED_HASH=ON && cmake --build build-mix
./build-mix/bench 10000000
```

Valgrind doesn't run on Apple Silicon. On a Mac, use Instruments (Time Profiler, or CPU Counters for cache misses), or `sample` against a running `bench`.

## Stretch features

- **IOC and FOK** in both books. The FOK check sums level totals in V2 and walks the orders in V1.
- **Multiple symbols:** `MatchingEngine<Book>` keeps one book per ticker. The books share nothing, which is also the easiest way to scale: shard symbols across threads with no locks between them.
- **Replay:** `replay` reads a CSV (`symbol,action,id,side,type,price,qty`), parses everything before the timer starts, sizes each symbol's V2 price band from the prices in the file, then prints the trades and a per-symbol summary. `replay --generate N file.csv` writes a random file for five symbols. I didn't have access to real exchange data, so this was tested on generated files; real data such as LOBSTER or ITCH would need a small converter to this format.

## What I'd do next

- **Symbol lookup in replay:** it's a `std::map<std::string, ...>` lookup per row. Mapping symbols to integer IDs once while parsing would remove it.
- **Best-price scan:** the V2 scan after a level empties is O(gap). A bitmap of non-empty levels with `ctz` or `clz` would find the next level in a few instructions even when the book is sparse.
- **Latency spikes:** the max-latency outliers come from vector growth (pool, hash map) and, in V1, `unordered_map` rehashing. Reserving to the real peak, or growing in chunks, would flatten them.
- **Threading:** one thread per symbol shard, fed by a single-producer single-consumer ring buffer, keeps each book single-threaded, so the matching code itself needs no locks.

## Layout

```
include/  Order.h Trade.h OrderBook.h FastOrderBook.h FlatHashMap.h
          MatchingEngine.h OrderGenerator.h
src/      OrderBook.cpp FastOrderBook.cpp main.cpp (demo) replay.cpp
tests/    test_orderbook.cpp
bench/    bench.cpp
data/     sample_orders.csv
```
