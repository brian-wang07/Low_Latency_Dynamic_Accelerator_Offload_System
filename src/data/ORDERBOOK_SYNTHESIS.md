# Plan: Full Depth Limit Orderbook Data Synthesis

## Context
The current `DataGenerator` synthesizes a single mid price. The correct model mirrors an actual exchange: the generator produces **only bids and asks**. Spread, mid, and depth are quantities consumers derive after the fact — they play no role in generation. Each tick is a single-sided order event (one bid OR one ask). A mean-reversion force on the spread is embedded directly into how each side's price walks, so the emergent spread stays bounded without ever computing mid. Burst logic, config structure, and `next()` interface are preserved.

---

## Stochastic Model

### Private state
```
current_bid_   — best bid price
current_ask_   — best ask price
```
`current_price_` (the existing field) is repurposed and renamed internally to `current_bid_`; `current_ask_` is added. Neither is labeled "mid" — they are the raw book prices.

Initialized from existing config fields at construction:
```
current_bid_ = start_price - start_spread / 2
current_ask_ = start_price + start_spread / 2
```
(This is only initialization, not generation. `start_price` serves as a convenient anchor.)

### Per-tick generation

Each tick represents **one order event on one side**:

```
spread  = current_ask_ - current_bid_        // derived internally for reversion only
dt      = actual_wait_ns / base_interval_ns  // normalized time step

// Common GBM component (same volatility parameter)
ε ~ N(0, 1)
common_move = ε * current_volatility * time_scaling

// Spread mean-reversion force (OU): pushes bid up / ask down when spread is wide
reversion_force = κ * (θ_spread - spread) * dt / 2

side = sample(BID with prob_bid, else ASK)

if side == BID:
    current_bid_ += common_move + reversion_force
    current_bid_  = max(current_bid_, 0.01)
    // ensure ask stays above bid
    current_ask_  = max(current_ask_, current_bid_ + min_spread)

if side == ASK:
    current_ask_ += common_move - reversion_force
    current_ask_  = max(current_ask_, current_bid_ + min_spread)
```

**Why this works:**
- `common_move` is the same N(0,1) shock on both sides → emergent mid follows GBM
- `reversion_force` nudges bid up / ask down when spread > θ (and vice-versa) → spread is mean-reverting
- Bid and ask are evolved directly; mid is never touched

### Depth at BBO (log-normal, independent each tick)
```
depth = exp(N(depth_log_mean, depth_log_sigma))
```
One draw per tick (the depth of the event's side).

### Order event indicators (categorical draws)
```
order_type = uniform < prob_limit  ? LIMIT  : MARKET
action     = u < prob_new          ? NEW    : (u < prob_new + prob_cancel ? CANCEL : MODIFY)
```
`side` is already sampled above.

---

## Files to Modify

### 1. `src/data/data_generator.hpp`

Add enums:
```cpp
enum class OrderType   : uint8_t { LIMIT = 0, MARKET = 1 };
enum class Side        : uint8_t { BID = 0,   ASK = 1    };
enum class OrderAction : uint8_t { NEW = 0,   CANCEL = 1, MODIFY = 2 };
```

Update `MarketDataSnapshot` — **only bid/ask are synthesized quantities**; no mid, no spread:
```cpp
struct MarketDataSnapshot {
    uint64_t    sequence_number;
    double      bid;          // best bid price (synthesized directly)
    double      ask;          // best ask price (synthesized directly)
    double      depth;        // size at the event side (log-normal)
    OrderType   order_type;
    Side        side;
    OrderAction action;
    uint64_t    timestamp;
};
```

Extend `DataGeneratorConfig` (keep all existing fields, add):
```cpp
// Spread mean-reversion params
double spread_mean;              // θ: long-run mean spread       (e.g. 0.10)
double spread_reversion_speed;   // κ: reversion strength         (e.g. 5.0)
double min_spread;               // hard floor ask - bid          (e.g. 0.01)
double start_spread;             // initial ask - bid             (e.g. 0.10)

// Depth log-normal params
double depth_log_mean;           // μ  (e.g. 4.0 → median ~55)
double depth_log_sigma;          // σ  (e.g. 1.0)

// Order event probabilities
double prob_limit;               // P(LIMIT), rest MARKET         (e.g. 0.70)
double prob_bid;                 // P(BID),   rest ASK            (e.g. 0.50)
double prob_new;                 // P(NEW)                        (e.g. 0.60)
double prob_cancel;              // P(CANCEL), rest MODIFY        (e.g. 0.30)
```

Add/rename private members:
```cpp
double current_bid_;             // replaces current_price_
double current_ask_;
std::lognormal_distribution<double> depth_dist_;
std::uniform_real_distribution<double> uniform_{0.0, 1.0};
// dist_ (already exists) reused for all N(0,1) draws
```

### 2. `src/data/data_generator.cpp`

- Rename `current_price_` → `current_bid_`, initialize both `current_bid_` and `current_ask_` in constructor
- Initialize `depth_dist_` from `cfg_.depth_log_mean` / `cfg_.depth_log_sigma`
- Rewrite `next()` per the model above; burst-timing block is **unchanged**

### 3. `src/common/shm_types.hpp`

Replace `price` in `MarketData` with the new fields. Keep a `price` field (computed as `(bid+ask)/2` by the writer) so the runtime engine requires no changes for now:

```cpp
struct alignas(64) MarketData {
    std::atomic<uint64_t> sequence_number;  // 8
    std::atomic<double>   bid;              // 8
    std::atomic<double>   ask;              // 8
    std::atomic<double>   depth;            // 8
    std::atomic<double>   price;            // 8  mid=(bid+ask)/2, runtime compat
    std::atomic<uint8_t>  order_type;       // 1
    std::atomic<uint8_t>  side;             // 1
    std::atomic<uint8_t>  action;           // 1
    uint8_t               _pad[5];          // 5
    std::atomic<uint64_t> timestamp;        // 8
    // total = 64 bytes
};
```

Add: `static_assert(std::atomic<uint8_t>::is_always_lock_free)`.

### 4. `src/data/main.cpp`

- Add new config fields to existing `cfg{}` initializer
- Update SHM write block: store `bid`, `ask`, `depth`, `order_type`, `side`, `action`; write `price = (tick.data.bid + tick.data.ask) / 2.0` for runtime compat
- Update `std::cout` to print bid, ask, spread (`ask-bid`), depth, type, side, action

---

## Config Defaults for `main.cpp`

```cpp
.spread_mean             = 0.10,
.spread_reversion_speed  = 5.0,
.min_spread              = 0.01,
.start_spread            = 0.10,
.depth_log_mean          = 4.0,
.depth_log_sigma         = 1.0,
.prob_limit              = 0.70,
.prob_bid                = 0.50,
.prob_new                = 0.60,
.prob_cancel             = 0.30,
```

---

## Verification

1. Build: `cmake --build build` — zero errors/warnings
2. Run `./build/data_sim`: `bid < ask` every tick; spread fluctuates around ~0.10; `bid` and `ask` both trend (GBM behavior visible in mid)
3. Run `./build/data_test` — existing timing/burst tests pass unchanged
4. Spot check ~1000 ticks: ~70% LIMIT, ~50% BID, ~60% NEW (within statistical noise)
