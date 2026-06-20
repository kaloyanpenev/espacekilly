#pragma once

#include "defs.h"
#include "dro/spsc-queue.hpp"
#include "ring_buffer.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <memory_resource>
#include <unordered_map>
#include <vector>

namespace matcher
{

namespace
{
#ifdef ALIGNED
#undef ALIGNED
#endif
#define ALIGNED alignas(cacheLineSize)
}

struct Order
{
	std::size_t id = 0;
	std::int64_t quantityLots = 0;
};

constexpr size_t kOrdersPerTick = 8192;
static_assert(std::has_single_bit(kOrdersPerTick));

struct OrdersForTick
{
	OrdersForTick(std::pmr::monotonic_buffer_resource& allocator);

	// map of order id to its idx in the orders vec for fast lookup

	pc::pmr_array<Order, kOrdersPerTick> orders;
	std::pmr::unordered_map<size_t, size_t> idToIdx;

	size_t writeIndex = 0;
	size_t readIndex = 0;

	inline size_t count() { return writeIndex - readIndex; }
	inline bool is_empty() { return writeIndex == readIndex; }
};


constexpr size_t kPriceLevelCount = 128;
constexpr size_t kFulfilledOrdersCount = 2048;
static_assert(std::has_single_bit(kPriceLevelCount));
static_assert(std::has_single_bit(kFulfilledOrdersCount));

// sizeof the ordersForTick struct + sizeof the orders array
constexpr size_t ordersSizeInBytes = kPriceLevelCount * sizeof(OrdersForTick) + kPriceLevelCount * kOrdersPerTick * sizeof(Order);
constexpr size_t fulfilledSizeInBytes = kFulfilledOrdersCount * sizeof(Order::id);

constexpr size_t orderBookArenaSize = ordersSizeInBytes + fulfilledSizeInBytes;

enum class Instrument : std::size_t
{
	Water = 0UL,
	Food = 1UL,
	Time = 2UL,

	Count
};


enum class OrderType : uint8_t
{
	Buy = 0,
	Sell = 1,
	BuyLimit = 2,
	SellLimit = 3,
	StopSell,
	StopSellLimit,
	StopBuy,
	StopBuyLimit,
};

struct OrderMessage
{
	Order order;
	Instrument instrumentId;
	size_t priceTicksLimit;
	size_t priceTicksStop;
	OrderType orderType;
};


constexpr size_t invalidBestIdx = SIZE_MAX; // needs to be size_max for underflow logic to work right
// Order book per instrument
class OrderBook
{
private:
	// Arena storage
	std::vector<std::byte> arenaBuffer;
	std::pmr::monotonic_buffer_resource arena;

public:
	std::array<size_t, 2> counts{0,0}; // 0 for askCount, 1 for bidCount

	size_t filledReadIdx = 0;
	size_t filledWriteIdx = 0;
	pc::pmr_array<size_t, kFulfilledOrdersCount> fulfilled; // at most we will have orders per tick fulfilled

	std::array<size_t, 2> bestIdx{invalidBestIdx, invalidBestIdx}; // 0 - ask, 1 - bid.  size::max for unset.
	pc::pmr_array<OrdersForTick, kPriceLevelCount> orders;

	explicit OrderBook();
};


//int startMatch(std::shared_ptr<dro::SPSCQueue<OrderMessage>> messageQueue);
int startMatch();
bool HandleLimitOrder(OrderMessage &ordMsg, OrderBook &symbol);

bool HandleMarketOrder(OrderMessage &ordMsg, OrderBook &symbol, size_t limit);

}