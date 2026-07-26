#pragma once

#include "defs.h"
#include "dro/spsc-queue.hpp"
#include "hugepage_allocation.h"
#include "ring_buffer.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <memory_resource>
#include <unordered_map>
#include <variant>
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

using OrderID = std::size_t;
using MessageID = std::size_t;

enum class OrderType : uint8_t
{
	Buy = 0,
	BuyLimit = 1,
	Sell = 2,
	SellLimit = 3,
	// TODO: stop book fairy, separate process.
};

struct Order
{
	OrderID id = 0;
	std::int64_t quantityLots = 0; // positive for buy orders. negative for sell orders.
};

struct OrderNode
{
	Order order;

	OrderNode* next{nullptr};
	OrderNode* prev{nullptr};
};

namespace intrusiveList
{
	// list ops
	bool isValidNode(const OrderNode& listSentinel, OrderNode* n);
	OrderNode* tail(const OrderNode& listSentinel);
	OrderNode* head(const OrderNode& listSentinel);
	bool isEmpty(const OrderNode& listSentinel);
	void append(OrderNode& listSentinel, OrderNode* n);
	void unlink(OrderNode* n);
}

struct OrdersForTick
{
	OrdersForTick(std::pmr::monotonic_buffer_resource& allocator);

	// map of order id to its idx in the orders vec for fast lookup

	// linux kernel list_head pattern. Effectively a circular list.
	// sentinel.prev always points at the tail.
	// lastElement.next always points to sentinel.
	// traversing the list in any order will at some point result in the sentinel.
	OrderNode listSentinel{.order = {}, .next = &listSentinel, .prev = &listSentinel};
};

struct MessageResponse
{
	std::optional<Order> oOrder;
	enum class Result : uint32_t
	{
		Filled,
		PartiallyFilled,
		Resting,
		Rejected,
		NotFound,
		Cancelled,
	} result;
};


constexpr size_t kPriceLevelCount = 128; // MUST be power of 2
constexpr size_t kFulfilledOrdersCount = 2048; // MUST be power of 2
constexpr size_t kOrdersPerTick = 8192; // MUST be power of 2
static_assert(std::has_single_bit(kPriceLevelCount));
static_assert(std::has_single_bit(kFulfilledOrdersCount));
static_assert(std::has_single_bit(kOrdersPerTick));

// sizeof the ordersForTick struct + sizeof the orders array
constexpr size_t totalOrderCount = kPriceLevelCount * kOrdersPerTick;
constexpr size_t sizeofOrderLevels = kPriceLevelCount * sizeof(OrdersForTick);
constexpr size_t sizeofOrderNodes = totalOrderCount * sizeof(OrderNode);
constexpr size_t sizeofFulfilled = kFulfilledOrdersCount * sizeof(MessageResponse);

constexpr size_t orderBookArenaSize = sizeofOrderLevels + sizeofOrderNodes + sizeofFulfilled + /*// slack for misalignment*/ sizeofFulfilled + totalOrderCount * 128;
constexpr size_t processQueueSize = sizeof(MessageResponse) * totalOrderCount;

enum class Instrument : std::size_t
{
//	Water = 0UL,
//	Food = 1UL,
	Time = 0UL,

	Count
};

struct MessageHeader
{
	uint16_t seqnum;
	uint32_t numOfMessages;
	int64_t timestamp_ns;
};

enum class RequestType : int32_t
{
	NewOrder = 0,
	CancelOrder = 1
};

struct OrderRequestHeader
{
	uint16_t size;
	RequestType type;
};

struct NewOrderRequest
{
	MessageID msgId;
	Order order;
	OrderType orderType;
	Instrument instrumentId;
	uint64_t priceTicksLimit;
	uint64_t priceTicksStop;
};

struct CancelOrderRequest
{
	MessageID msgId;
	OrderID toCancel;
	Instrument instrumentId;
};

using NewRequest = std::variant<NewOrderRequest, CancelOrderRequest>;

constexpr size_t hugepagesize = 2ull * 1024ull * 1024ull;
constexpr size_t count = 10'000'000;
// exactly arrSize requests are written, densely, with no gaps: both the
// generator and the matcher size themselves off this one constant, so no
// end-of-stream sentinel is needed.
constexpr size_t arrSize = (count * 3ull) / 2ull;
constexpr size_t arrSizeBytes = arrSize * sizeof(NewRequest);
// hugetlbfs ftruncate rejects any length that is not a multiple of the huge
// page size, so round up. mmap does not care and may use arrSizeBytes directly.
constexpr size_t allocSize = ((arrSizeBytes + hugepagesize - 1) / hugepagesize) * hugepagesize;

struct OrdersData
{
	OrdersData(std::pmr::monotonic_buffer_resource& allocator);
	pc::pmr_array<OrderNode, totalOrderCount> ordersData;
	OrderNode* GetFree();
	void ReleaseToFree(OrderNode *freed);
private:
	OrderNode* free_list{};


};

constexpr size_t invalidBestIdx = SIZE_MAX; // needs to be size_max for underflow logic to work right
// Order book per instrument
class OrderBook
{
private:
	// Arena storage
	HugepageAllocation arenaAlloc;
	std::pmr::monotonic_buffer_resource arena;

public:
	std::array<size_t, 2> counts{0,0}; // 0 for askCount, 1 for bidCount

	size_t filledReadIdx = 0;
	size_t filledWriteIdx = 0;
	pc::pmr_array<MessageResponse, kFulfilledOrdersCount> fulfilled; // at most we will have orders per tick fulfilled

	std::array<size_t, 2> bestIdx{invalidBestIdx, invalidBestIdx}; // 0 - ask, 1 - bid.  size::max for unset.

	pc::pmr_array<OrdersForTick, kPriceLevelCount> orders;

	OrdersData ordersData;

	// cancellation helper
	std::pmr::unordered_map<size_t, OrderNode*> idToOrder;

	explicit OrderBook();
	OrderBook(const OrderBook&) = delete;
	OrderBook(OrderBook&&) = delete;
	OrderBook& operator=(const OrderBook&) = delete;
	OrderBook& operator=(OrderBook&&) = delete;
};


//int startMatch(std::shared_ptr<dro::SPSCQueue<NewOrderRequest>> messageQueue);
// marketFd: a read-only descriptor for market_generator's memfd, obtained by
// opening the /proc/<pid>/fd/<fd> path it prints. Ownership stays with the caller.
int startMatch(int marketFd);
[[clang::xray_always_instrument]] MessageResponse HandleLimitOrder(const NewOrderRequest&ordMsg, OrderBook &symbol);
[[clang::xray_always_instrument]] MessageResponse HandleMarketOrder(const NewOrderRequest&ordMsg, OrderBook &symbol, size_t limit);
[[clang::xray_always_instrument]] MessageResponse HandleCancellation(const CancelOrderRequest&cancelMsg, OrderBook &symbol);
[[gnu::noinline]] void matchAllOrders(std::array<OrderBook, 1>& orderBooks, dro::SPSCQueue<MessageResponse, 0, std::pmr::polymorphic_allocator<MessageResponse> >& processedQueue, std::pmr::vector<uint64_t>& durations, int marketFd);

}