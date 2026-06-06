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

constexpr size_t orderBookArenaSize = 128 * 1024 * 1024; // 128 mb
constexpr size_t kOrdersPerTick = 512;
constexpr size_t kOrdersCount = 32768;

enum class Instrument : std::size_t
{
	Water = 0UL,
	Food = 1UL,
	Time = 2UL,

	Count
};

struct Order
{
	std::size_t id = 0;
	std::int64_t quantityLots = 0;
};
enum class OrderType : uint8_t
{
	Buy,
	BuyLimit,
	BuyStop,
	BuyStopLimit,
	Sell,
	SellLimit,
	SellStop,
	SellStopLimit
};

struct ALIGNED OrderMessage
{
	Instrument instrumentId;
	Order order;
	size_t priceTicksLimit;
	size_t priceTicksStop;
	OrderType orderType;
};

struct ALIGNED OrdersForTick
{
	OrdersForTick(std::pmr::monotonic_buffer_resource& allocator);
	size_t priceInTicks = 0u;

	size_t writeIndex = 0;
	size_t readIndex = 0;

	// map of order id to its idx in the orders vec for fast lookup
	std::pmr::unordered_map<size_t, size_t> idToIdx;

	pc::pmr_array<Order, kOrdersPerTick> orders;

	inline size_t count() { return writeIndex - readIndex; }
	inline bool is_empty() { return writeIndex == readIndex; }
};

// Order book per instrument
class OrderBook
{
private:
	// Arena storage
	std::vector<std::byte> arenaBuffer;
	std::pmr::monotonic_buffer_resource arena;

public:
	size_t filledReadIdx = 0;
	size_t filledWriteIdx = 0;
	ALIGNED pc::pmr_array<size_t, kOrdersCount> fulfilled;

	size_t bestBidIdx = 0;
	size_t bestAskIdx = 0;
	ALIGNED pc::pmr_array<OrdersForTick, kOrdersCount> orders;

	explicit OrderBook();
};

int startMatch(std::shared_ptr<dro::SPSCQueue<OrderMessage>> messageQueue);
}