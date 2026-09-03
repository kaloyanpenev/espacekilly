#include <matcher/matcher.h>

#include "perfController.h"
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <list>
#include <memory_resource>
#include <random>
#include <ranges>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <x86intrin.h>

namespace
{
size_t g_limitOrders = 0;
size_t g_noOps = 0;
size_t g_marketOrders = 0;
size_t g_cancels = 0;
size_t g_cancelsNotFound = 0;
size_t g_crossOrder = 0;
size_t g_restingOrder = 0;
size_t g_fullyFilledCrossOrder = 0;

// helper type for the visitor #4
template <class... Ts>
struct overloaded : Ts...
{
	using Ts::operator()...;
};

constexpr size_t kOrdersPerTickShift = std::countr_zero(matcher::kOrdersPerTick);

size_t GetPriceLevelForOrder(const matcher::OrderNode* node, const matcher::OrderNode* startNode)
{
	return (node - startNode) >> kOrdersPerTickShift;
}

}

namespace matcher
{


OrderBook::OrderBook() :
	arenaAlloc(orderBookArenaSize),
	arena(arenaAlloc.data(), arenaAlloc.size(), std::pmr::null_memory_resource()),
	fulfilled(arena),
	orders(arena, arena),
	ordersData(arena),
	idToOrder(arena)
{
}



std::atomic<bool> g_done = false;

[[clang::xray_never_instrument]]
void ProcessResponses(dro::SPSCQueue<MessageResponse, 0, std::pmr::polymorphic_allocator<MessageResponse> >& responses)
{
	while (!g_done.load(std::memory_order::relaxed))
	{
		MessageResponse response{};
		(void)responses.try_pop(response);
		std::this_thread::yield();
	}
}


int startMatch(int marketFd)
{
	size_t capacity = static_cast<size_t>(arrSize * 1.05); // some slack
	//dro::SPSCQueue<NewRequest> q(capacity);
	HugepageAllocation hgpgresponses(capacity * sizeof(MessageResponse));
	auto allocBuf = std::pmr::monotonic_buffer_resource(hgpgresponses.data(),
		hgpgresponses.size(),
		std::pmr::null_memory_resource());
	std::pmr::polymorphic_allocator<MessageResponse> pmrAlloc(&allocBuf);
	using outqueue = dro::SPSCQueue<MessageResponse, 0, std::pmr::polymorphic_allocator<MessageResponse> >;
	dro::SPSCQueue<MessageResponse, 0, std::pmr::polymorphic_allocator<MessageResponse> > responses(capacity, pmrAlloc);

	//auto makeInput = std::jthread(&CreateMarket, std::ref(q));
	//makeInput.join();
	//
	//auto processResponses = std::jthread(&ProcessResponses, std::ref(responses));

	//CreateMarket(q);
	//makeInput.join();
	// TODO: stop order book

	//	PerfCounter branches    (PerfCounter::Event::Branches);
	//	PerfCounter branchMisses(PerfCounter::Event::BranchMisses);
	//	PerfCounter instructions(PerfCounter::Event::Instructions);
	//
	//	branches.start();
	//	branchMisses.start();
	//	instructions.start();
	HugepageAllocation durs(arrSize * 2 * sizeof(uint64_t));
	auto dursAllocBuf = std::pmr::monotonic_buffer_resource(durs.data(),
		durs.size(),
		std::pmr::null_memory_resource());

	std::array<OrderBook, static_cast<size_t>(Instrument::Count)> orderBooks{OrderBook{}};

	std::pmr::vector<std::pair<uint64_t, uint64_t>> durations(arrSize, {0,0}, std::pmr::polymorphic_allocator<std::pair<uint64_t, uint64_t>>(&dursAllocBuf));


	matchAllOrders(orderBooks, responses, durations, marketFd);

	//	instructions.stop();
	//	branches.stop()f
	//	branchMisses.stop();
	// auto worst = std::ranges::max_element(durations);
	//
	// remove first 100 elements - cold caches / branching, etc.
	durations.erase(durations.begin(), std::next(durations.begin(), 100));

	//std::println("worst idx: {}", std::distance(durations.begin(), worst));

	std::ranges::sort(durations);

	//	static constexpr std::string_view path = "durations.yaml";
	//
	//	auto file = std::ofstream(path.data(), std::ios::trunc);
	//	if (file.is_open())
	//	{
	//		for (const auto& dur : durations)
	//		{
	////			std::string count = std::to_string(dur.count());
	////			file.write(count.data(), count.size());
	////			file.write("\n", 1);
	////			file.flush();
	//			file << (dur / 3) << std::endl;
	//
	//		}
	//	}

	size_t idx99999 = static_cast<size_t>(0.99999 * durations.size());
	size_t idx9999 = static_cast<size_t>(0.9999 * durations.size());
	size_t idx999 = static_cast<size_t>(0.999 * durations.size());
	size_t idx99 = static_cast<size_t>(0.99 * durations.size());
	size_t idx95 = static_cast<size_t>(0.95 * durations.size());
	size_t idx50 = static_cast<size_t>(0.5 * durations.size());

	// rdtsc
	constexpr int cyclesForRdtsc = 58;
	constexpr double cyclesPerNs = 2.8945;
	std::println("p99.999, idx {}: {:.0f}ns, fills/order: {}", idx99999, ((durations[idx99999].first - cyclesForRdtsc) / cyclesPerNs), durations[idx99999].second);
	std::println("p99.99, idx {}: {:.0f}ns, fills/order: {}", idx9999, ((durations[idx9999].first - cyclesForRdtsc) / cyclesPerNs), durations[idx9999].second);
	std::println("p99.9, idx {}: {:.0f}ns, fills/order: {}", idx999, ((durations[idx999].first - cyclesForRdtsc) / cyclesPerNs),durations[idx999].second);
	std::println("p99, idx {}: {:.0f}ns, fills/order: {}", idx99, ((durations[idx99].first - cyclesForRdtsc) / cyclesPerNs), durations[idx99].second);
	std::println("p95, idx {}: {:.0f}ns, fills/order: {}", idx95, ((durations[idx95].first - cyclesForRdtsc) / cyclesPerNs), durations[idx95].second);
	std::println("p50, idx {}: {:.0f}ns, fills/order: {}", idx50, ((durations[idx50].first - cyclesForRdtsc) / cyclesPerNs), durations[idx50].second);
	std::println("last: {:.0f}ns, fills/order: {}", ((durations.back().first - cyclesForRdtsc) / cyclesPerNs), durations.back().second);

	std::println("executed_limits: {}, resting_crosses: {}, fully_filled_crosses: {}, resting: {}",
		g_limitOrders,
		g_crossOrder - g_fullyFilledCrossOrder,
		g_fullyFilledCrossOrder,
		g_restingOrder);
	std::println("no ops (aggressive order found the opposite side empty): {}", g_noOps);
	std::println("executed_markets: {}", g_marketOrders - g_crossOrder);
	std::println("executed_cancels: {}, of which not found (already filled): {}", g_cancels, g_cancelsNotFound);
	std::println("book state: ask: {}, bid: {}",
		orderBooks[0].bestIdx[0],
		orderBooks[0].bestIdx[1]);
	std::println("book width: asks: {}, bids: {}", orderBooks[0].counts[0], orderBooks[0].counts[1]);

	// the real ladder: walk every level's intrusive list. a level holds one side
	// only, and the sign of quantityLots says which. high price first.
	std::println("book ladder (non-empty levels):");
	std::println("  {:>6} {:>8} {:>5} {:>6} {:>8}", "bids", "bidLots", "price", "asks", "askLots");
	const OrderBook& book = orderBooks[0];
	for (size_t p = kPriceLevelCount; p-- > 0;)
	{
		size_t bids = 0, asks = 0;
		int64_t bidLots = 0, askLots = 0;
		const OrderNode& sentinel = book.orders[p].listSentinel;
		for (const OrderNode* n = sentinel.next; n != &sentinel; n = n->next)
		{
			const bool isBid = n->order.quantityLots > 0;
			(isBid ? bids : asks)++;
			(isBid ? bidLots : askLots) += n->order.quantityLots;
		}
		if (bids == 0 && asks == 0)
		{
			continue;
		}
		const char* mark = p == book.bestIdx[1] ? "  <- best bid" : p == book.bestIdx[0] ? "  <- best ask" : "";
		std::println("  {:>6} {:>8} {:>5} {:>6} {:>8}{}", bids, bidLots, p, asks, -askLots, mark);
	}
	//	std::println("instructions  : {}", instructions.read());
	//	std::println("branches      : {}", branches.read());
	//	std::println("branch misses : {}", branchMisses.read());
	//	std::println("miss rate     : {}",
	//		   branchMisses.read() / branches.read());
	//processResponses.join();
	return 0;
}

[[clang::xray_always_instrument]] void matchAllOrders(std::array<OrderBook,1>& orderBooks,
	dro::SPSCQueue<MessageResponse, 0, std::pmr::polymorphic_allocator<MessageResponse> >& processedQueue,
	std::pmr::vector<std::pair<uint64_t, uint64_t>>& durations,
	int marketFd
	)
{
	void* vma = mmap(nullptr, allocSize, PROT_READ, MAP_SHARED | MAP_POPULATE, marketFd, 0);
	if (vma == MAP_FAILED)
	{
		std::perror("mmap market");
		return;
	}

	NewRequest* orders = static_cast<NewRequest*>(vma);


	for (size_t i = 0; i < arrSize; i++)
	{
		size_t start{0};
		size_t filledIdx{0};
		_mm_lfence();
		start = __rdtsc();

		uint32_t aux{0};
		std::visit(
			overloaded{[&orderBooks, &processedQueue, &filledIdx](NewOrderRequest& ordMsg) -> void
			           {
				           if (ordMsg.order.quantityLots == 0) [[unlikely]]
				           {
					           (void)processedQueue.try_emplace(
						           MessageResponse{.oOrder = std::nullopt,
						                           .result = MessageResponse::Result::Rejected});
					           return;
				           }
				           auto& symbol = orderBooks[static_cast<size_t>(ordMsg.instrumentId)];
							auto writeIdxAtStart = symbol.filledWriteIdx;
				           // TODO: use look-up table for functions instead of branching - we expect >75% mispredict

				           MessageResponse response{};
				           // market - we are simply filling it immediately starting with the best bid/ask. If unable to fully fill, reject.
				           if (ordMsg.orderType == OrderType::Buy || ordMsg.orderType == OrderType::Sell)
				           {
					           //std::println("market order. Id: {}, quantity: {}, price: {}", ordMsg.order.id, ordMsg.order.quantityLots, ordMsg.priceTicksLimit);
					           response = HandleMarketOrder(ordMsg,
						           symbol,
						           ordMsg.orderType == OrderType::Buy ? SIZE_MAX : 0);
				           }
				           // limit order - may rest immediately or get partially filled, or fully filled
				           else if (ordMsg.orderType == OrderType::BuyLimit || ordMsg.orderType == OrderType::SellLimit)
				           {
					           //std::println("limit order. Id: {}, quantity: {}, price: {}", ordMsg.order.id, ordMsg.order.quantityLots, ordMsg.priceTicksLimit);
					           response = HandleLimitOrder(ordMsg, symbol);
				           }

				           while (symbol.filledWriteIdx != symbol.filledReadIdx)
				           {
					           (void)processedQueue.try_emplace(
						           symbol.fulfilled[symbol.filledReadIdx & (symbol.fulfilled.size() - 1)]);
					           symbol.filledReadIdx++;
				           }

				           (void)processedQueue.try_emplace(std::move(response));
							filledIdx = symbol.filledWriteIdx - writeIdxAtStart;

			           },
			           [&orderBooks, &processedQueue](CancelOrderRequest& cancelMsg) -> void
			           {
				           auto& symbol = orderBooks[static_cast<size_t>(cancelMsg.instrumentId)];
				           MessageResponse result = HandleCancellation(cancelMsg, symbol);
				           (void)processedQueue.try_emplace(std::move(result));
			           }},
			orders[i]);

			const auto end = __rdtscp(&aux);
			_mm_lfence();
			durations[i] = {(end - start), filledIdx};
	}
	g_done.store(true, std::memory_order::relaxed);

	munmap(vma, allocSize);
}

[[clang::xray_always_instrument]] MessageResponse HandleMarketOrder(const NewOrderRequest& ordMsg,
	OrderBook& symbol,
	size_t limit)
{
	g_marketOrders++;
	bool orderIsBuy = ordMsg.order.quantityLots > 0; // 1 for buy, 0 for sell
	size_t& bestIdx = symbol.bestIdx[!orderIsBuy];   // looking for best bid when order type is sell and vv.
	int64_t newLots = ordMsg.order.quantityLots;

	// 1 if order is buy - price moves up to next more expensive ask after you fill current level
	//-1 if order is sell - price moves down to next cheaper bid after you fill current level
	int direction = orderIsBuy * 2 - 1;

	auto& orderCount = symbol.counts[!orderIsBuy];
	g_noOps += orderCount == 0;

	// quantityLots is positive if the order is a buy.
	// in that case, direction is 1 -> if we go under 0, it means  we are overfilled: end
	// quantityLots is negative if the order is a sell.
	// if we go above 0, it means we are overfilled: end -> direction is -1 so we flip it
	while (orderCount > 0 && bestIdx != invalidBestIdx && newLots * direction > 0)
	{
		auto& currTickOrders = symbol.orders[bestIdx & (symbol.orders.size() - 1)];
		if (intrusiveList::isEmpty(currTickOrders.listSentinel))
		{
			// no more orders in this level. fix the tail to be ready to insert.
			// level is empty; look for more expensive asks or cheaper bids
			bestIdx += direction;
			// check if we're past the limit - in that case, stop.
			// limit == inf if order is market buy, limit == 0 if order is market sell
			// bestIdx can wrap around to size_max if this is a market sell order, and break due to bestIdx == invalidBestIdx
			if ((orderIsBuy && bestIdx > limit) || (!orderIsBuy && bestIdx < limit)) [[unlikely]]
			{
				break; // could not fill
			}
			continue; // we have updated the level - start over.
		}

		OrderNode* resting = intrusiveList::head(currTickOrders.listSentinel);
		__builtin_prefetch(resting->next); // get the next one ready
		newLots += resting->order.quantityLots; // fill towards zero

		// remainder is left in the resting if we crossed over the 0 in the dir we are going.
		const bool remainderLeftInResting = newLots * direction < 0;

		// if we filled the incoming - existing could be partially fulfilled now, so give back what we took.
		// keep the remainder if there is any left (there will be, if we overfilled the incoming order).
		resting->order.quantityLots = remainderLeftInResting * newLots;

		// side-effects
		symbol.fulfilled[symbol.filledWriteIdx & (symbol.fulfilled.size() - 1)] = MessageResponse{resting->order,
			remainderLeftInResting ? MessageResponse::Result::PartiallyFilled : MessageResponse::Result::Filled};

		symbol.filledWriteIdx++;
		// decrement from counts
		orderCount -= !remainderLeftInResting;
		if (!remainderLeftInResting)
		{
			intrusiveList::unlink(resting);
			symbol.ordersData.ReleaseToFree(resting, GetPriceLevelForOrder(resting, &symbol.ordersData.ordersData[0]));
		}
	}
	// if no more orders left, set bestIdx to SIZE_MAX
	if (orderCount == 0) [[unlikely]]
	{
		bestIdx = invalidBestIdx;
	}

	bool unfilled = newLots * direction > 0;
	bool rejected = newLots == ordMsg.order.quantityLots;
	// if the order is filled, there will be a remainder inside (equal to the leftover in the resting limit order it filled against)
	newLots *= unfilled;

	constexpr static std::array<MessageResponse::Result, 3> resArray{
		MessageResponse::Result::Filled, MessageResponse::Result::PartiallyFilled, MessageResponse::Result::Rejected};
	return MessageResponse{.oOrder = Order{ordMsg.order.id, newLots}, .result = resArray[unfilled + rejected]};
}

[[clang::xray_always_instrument]] MessageResponse HandleLimitOrder(const NewOrderRequest& ordMsg, OrderBook& symbol)
{

	g_limitOrders++;
	bool orderIsBuy = ordMsg.order.quantityLots > 0; // 1 for buy, 0 for sell

	const size_t& bestOppositeIdx = symbol.bestIdx[!orderIsBuy]; // looking for best bid when order type is sell and vv.

	Order newOrder = ordMsg.order;
	// 1) match - if the incoming crosses over the other type's best idx
	// then fill it, starting with the best idx level. any remainder, leave in that level.

	if (bestOppositeIdx != invalidBestIdx)
	{
		const bool betterThanBestBuy = orderIsBuy && ordMsg.priceTicksLimit >= bestOppositeIdx;
		const bool betterThanBestSell = !orderIsBuy && ordMsg.priceTicksLimit <= bestOppositeIdx;
		if (betterThanBestBuy || betterThanBestSell)
		{
			g_crossOrder++;
			MessageResponse response = HandleMarketOrder(ordMsg, symbol, ordMsg.priceTicksLimit);
			// we have now handled whatever we can - check if we could fully fulfill the order.
			if (response.result == MessageResponse::Result::Filled)
			{
				g_fullyFilledCrossOrder++;
				assert(response.oOrder->quantityLots == 0);
				return response;
			}
			newOrder = *response.oOrder;
		}
	}

	// 2) no immediate match or resting remainder left:
	// add the order to its requested level
	g_restingOrder++;

	if (symbol.bestIdx[orderIsBuy] == invalidBestIdx)
	{
		symbol.bestIdx[orderIsBuy] = ordMsg.priceTicksLimit;
	}

	size_t& bestSameIdx = symbol.bestIdx[orderIsBuy]; // looking for best bid when order type is sell and vv.

	// TODO NOTE: assumes orders is not a ringbuffer. if ringbuffer, need to search for the level with that price ticks.
	auto& currOrdersLevel = symbol.orders[ordMsg.priceTicksLimit];

	OrderNode* incOrder = symbol.ordersData.GetFree(ordMsg.priceTicksLimit);
	intrusiveList::append(currOrdersLevel.listSentinel, incOrder);

	incOrder->order = newOrder;
	symbol.idToOrder[newOrder.id] = incOrder;

	symbol.counts[orderIsBuy]++;

	// check if this is the new best price - update best idx to be that price level.
	if (bestSameIdx == invalidBestIdx || (orderIsBuy && ordMsg.priceTicksLimit > bestSameIdx)
	    || (!orderIsBuy && ordMsg.priceTicksLimit < bestSameIdx))
	{
		bestSameIdx = ordMsg.priceTicksLimit;
	}

	return MessageResponse{.oOrder = std::move(newOrder), .result = MessageResponse::Result::Resting};
}

MessageResponse HandleCancellation(const CancelOrderRequest& cancelMsg, OrderBook& symbol)
{
	g_cancels++;
	if (auto* extractedOrd = symbol.idToOrder[cancelMsg.toCancel];
		extractedOrd && extractedOrd->order.id == cancelMsg.toCancel)
	{
		Order order = std::move(extractedOrd->order);
		symbol.counts[order.quantityLots > 0]--;
		intrusiveList::unlink(extractedOrd);
		symbol.ordersData.ReleaseToFree(extractedOrd, GetPriceLevelForOrder(extractedOrd, &symbol.ordersData.ordersData[0]));
		return MessageResponse{order, MessageResponse::Result::Cancelled};
	}
	g_cancelsNotFound++;
	return {.oOrder = std::nullopt, .result = MessageResponse::Result::NotFound};
}

OrdersForTick::OrdersForTick(std::pmr::monotonic_buffer_resource& allocator)
{
}

// list ops
bool intrusiveList::isValidNode(const OrderNode& listSentinel, OrderNode* n)
{
	return n == &listSentinel;
}

OrderNode* intrusiveList::tail(const OrderNode& listSentinel)
{
	return listSentinel.prev;
}

OrderNode* intrusiveList::head(const OrderNode& listSentinel)
{
	return listSentinel.next;
}

bool intrusiveList::isEmpty(const OrderNode& listSentinel)
{
	return intrusiveList::isValidNode(listSentinel, listSentinel.prev);
}

void intrusiveList::append(OrderNode& listSentinel, OrderNode* n)
{
	n->prev = listSentinel.prev; // set the prev of new node to the tail of the list.
	n->next = &listSentinel;     // set the next of new node to the sentinel.
	listSentinel.prev->next = n; // point tail node at the new node.
	listSentinel.prev = n;       // point the sentinel prev to the new node.
}

void intrusiveList::unlink(OrderNode* n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
}

OrdersData::OrdersData(std::pmr::monotonic_buffer_resource& allocator) :
	ordersData(allocator),
	free_lists(allocator)
{
	for (int plevel = 0; plevel < free_lists.size(); plevel++)
	{
		OrderNode* tail = &ordersData[0 + plevel * kOrdersPerTick];
		free_lists[plevel] = tail;

		for (int64_t ordIdx = 1; ordIdx < kOrdersPerTick; ordIdx++)
		{
			tail->next = &ordersData[ordIdx + plevel * kOrdersPerTick];
			tail = tail->next;
		}
	}
}

OrderNode* OrdersData::GetFree(size_t level)
{
	assert(free_lists[level]); // check if we have ran out of memory.
	auto ret = std::exchange(free_lists[level], free_lists[level]->next);
	ret->next = nullptr;
	return ret;
}

void OrdersData::ReleaseToFree(OrderNode* freed, size_t level)
{
	freed->order.id = 0;
	freed->order.quantityLots = 0;
	auto temp = free_lists[level];
	free_lists[level] = freed;
	free_lists[level]->next = temp;
}
}