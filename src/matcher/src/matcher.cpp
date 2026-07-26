#include <matcher/matcher.h>

#include "perfController.h"
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <list>
#include <memory_resource>
#include <random>
#include <ranges>
#include <thread>
#include <unistd.h>
#include <utility>
#include <x86intrin.h>

namespace
{
size_t g_limitOrders = 0;
size_t g_marketOrders = 0;
size_t g_cancels = 0;
size_t g_crossOrder = 0;
size_t g_restingOrder = 0;
size_t g_fullyFilledCrossOrder = 0;
size_t g_generatedMarkets = 0;
size_t g_generatedBids = 0;
size_t g_generatedLimits = 0;

// helper type for the visitor #4
template <class... Ts>
struct overloaded : Ts...
{
	using Ts::operator()...;
};
}

namespace matcher
{

OrderBook::OrderBook() :
	arenaAlloc(orderBookArenaSize),
	arena(arenaAlloc.data(), arenaAlloc.size(), std::pmr::null_memory_resource()),
	fulfilled(arena),
	orders{arena, arena},
	ordersData{arena}
{
	idToOrder.reserve(totalOrderCount);
}

std::vector<OrderBook> initOrderBooks()
{
	auto symbols = std::vector<OrderBook>(static_cast<std::size_t>(Instrument::Count));
	return symbols;
}

constexpr size_t generatedOrders = 10'000'000ul;

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

std::vector<NewRequest> SerializeAndDeserialize(const std::list<NewRequest>& list)
{
	std::vector<NewRequest> requests;

	static constexpr std::string_view path = "marketData.bin";

	int wfd = ::open(path.data(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (wfd == -1)
	{
		std::perror("open for write");
		return {};
	}

	std::println("open for wr: {}", path);

	static constexpr uint16_t requestByteSize = std::max(sizeof(NewOrderRequest), sizeof(CancelOrderRequest));
	for (const auto& el : list)
	{

		char buf[std::numeric_limits<uint16_t>::max()]{};
		std::memset(buf, 0, sizeof(buf));

		int offset = 0;
		int messageSize = 0;
		MessageHeader head{.seqnum = 0,
		                   .numOfMessages = 1,
		                   .timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count()};

		std::memcpy(buf, &head, sizeof(head));
		offset += sizeof(head);
		messageSize += offset;
		std::visit(overloaded{[&buf, &offset](const NewOrderRequest& req)
		                      {
			                      OrderRequestHeader reqHeader{.size = requestByteSize, .type = RequestType::NewOrder};
			                      std::memcpy(buf + offset, &reqHeader, sizeof(reqHeader));
			                      offset += sizeof(reqHeader);

			                      std::memcpy(buf + offset, &req, sizeof(req));
			                      offset += sizeof(req);
		                      },
		                      [&buf, &offset](const CancelOrderRequest& req)
		                      {
			                      OrderRequestHeader reqHeader{.size = requestByteSize,
			                                                   .type = RequestType::CancelOrder};
			                      std::memcpy(buf + offset, &reqHeader, sizeof(reqHeader));
			                      offset += sizeof(reqHeader);
			                      std::memcpy(buf + offset, &req, sizeof(req));
			                      offset += sizeof(req);
		                      }},
			el);

		messageSize += sizeof(OrderRequestHeader) + requestByteSize;
		int64_t written = write(wfd, buf, messageSize);
	}

	::close(wfd);

	int rfd = ::open(path.data(), O_RDONLY);
	if (rfd == -1)
	{
		std::perror("open for read");
		return {};
	}

	std::println("open for rd: {}", path);

	MessageHeader rdHeader{};
	int bytes = ::read(rfd, &rdHeader, sizeof(rdHeader));
	while (bytes > 0 && rdHeader.numOfMessages > 0)
	{
		OrderRequestHeader rdReqHead{};

		bytes = ::read(rfd, &rdReqHead, sizeof(rdReqHead));
		if (!bytes)
			throw std::runtime_error("problem");
		assert(rdReqHead.size >= std::max(sizeof(NewOrderRequest), sizeof(CancelOrderRequest)));

		std::variant<NewOrderRequest, CancelOrderRequest> rdNewReq{};

		switch (rdReqHead.type)
		{
		case RequestType::NewOrder:
		{
			NewOrderRequest rdOrdReq{};
			bytes = ::read(rfd, &rdOrdReq, rdReqHead.size);
			if (!bytes)
				throw std::runtime_error("problem");
			rdNewReq = std::move(rdOrdReq);
			break;
		}
		case RequestType::CancelOrder:
		{
			CancelOrderRequest rdCancelReq{};
			bytes = ::read(rfd, &rdCancelReq, rdReqHead.size - sizeof(CancelOrderRequest));
			if (!bytes)
				throw std::runtime_error("problem");
			rdNewReq = std::move(rdCancelReq);

			break;
		}
		}

		requests.emplace_back(std::move(rdNewReq));
		bytes = ::read(rfd, &rdHeader, sizeof(rdHeader));
	}

	return requests;
}

[[clang::xray_never_instrument]]
void CreateMarket(dro::SPSCQueue<NewRequest>& queue)
{
	//std::random_device rd;  // a seed source for the random number engine
	std::mt19937 gen(75); // mersenne_twister_engine seeded with rd()

	std::list<NewRequest> orders{};
	size_t msgId = 1;

	std::uniform_int_distribution<int64_t> distribLots(500, 20000);
	std::uniform_int_distribution<size_t> distribTicks(40, 60);
	std::uniform_int_distribution<int32_t> distribType(0, 3);

	for (size_t i = 0ul; i < generatedOrders; i++)
	{
		// generate random order type
		int8_t ordTypeNum = static_cast<int8_t>(distribType(gen));
		OrderType ordType = static_cast<OrderType>(ordTypeNum);

		int64_t sign = (ordTypeNum < 2) ? 1 : -1; // -1 if selling
		int64_t lots = distribLots(gen) * sign;

		size_t ticks = distribTicks(gen);
		size_t ordIsLimit = (ordType == OrderType::BuyLimit || ordType == OrderType::SellLimit);

		g_generatedMarkets += !ordIsLimit;
		g_generatedLimits += ordIsLimit;
		g_generatedBids += ordTypeNum < 2;

		// elapsed: 2416707
		//elapsed ns per order: 24.16707869
		//executed_limits: 50000804, resting_crosses: 6279364, fully_filled_crosses: 9458869, resting: 40541935
		//executed_markets: 49999196
		//generated_markets: 49999196, generated_limits: 50000804, generated_bids: 49996144, generated_asks: 50003856
		//book state: ask: 53, bid: 49
		orders.emplace_back(NewOrderRequest{.msgId = msgId,
		                                    .order = {.id = msgId++, .quantityLots = lots},
		                                    .orderType = ordType,
		                                    .instrumentId = Instrument::Time,
		                                    .priceTicksLimit = ordIsLimit * ticks});
	}

	bool skip = false;
	std::uniform_int_distribution<int32_t> cancelDistrib(0, 1);

	for (std::list<NewRequest>::iterator itr = orders.begin(); itr != orders.end(); std::advance(itr, 1))
	{
		if (std::exchange(skip, false))
		{
			continue;
		}

		bool cancel = static_cast<bool>(cancelDistrib(gen));
		if (!cancel)
		{
			continue;
		}
		skip = true; // we can't cancel a cancellation, so skip the next one.

		NewOrderRequest& orderMsg = std::get<NewOrderRequest>(*itr);
		orders.emplace(std::next(itr, 1),
			CancelOrderRequest{.msgId = msgId++, .toCancel = orderMsg.order.id, .instrumentId = orderMsg.instrumentId});
	}

	for (auto& el : orders)
	{
		queue.emplace(std::move(el));
	}
}

int startMatch()
{
	auto orderBooks = initOrderBooks();
	size_t capacity = static_cast<size_t>(std::ceil(generatedOrders * 1.6)); // 50% cancels, 15% slack
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
	auto processResponses = std::jthread(&ProcessResponses, std::ref(responses));

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
	std::vector<uint64_t> durations{};
	durations.reserve(generatedOrders);
	matchAllOrders(orderBooks, responses, durations);

	//	instructions.stop();
	//	branches.stop()f
	//	branchMisses.stop();

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

	std::println("p99.999, idx {}: {}", idx99999, (durations[idx99999] / 3));
	std::println("p99.99, idx {}: {}", idx9999, (durations[idx9999] / 3));
	std::println("p99.9, idx {}: {}", idx999, (durations[idx999] / 3));
	std::println("p99, idx {}: {}", idx99, (durations[idx99] / 3));
	std::println("p95, idx {}: {}", idx95, (durations[idx95] / 3));
	std::println("p50, idx {}: {}", idx50, (durations[idx50] / 3));
	std::println("last: {}", (durations.back() / 3));

	std::println("executed_limits: {}, resting_crosses: {}, fully_filled_crosses: {}, resting: {}",
		g_limitOrders,
		g_crossOrder - g_fullyFilledCrossOrder,
		g_fullyFilledCrossOrder,
		g_restingOrder);
	std::println("executed_markets: {}", g_marketOrders - g_crossOrder);
	std::println("executed_cancels: {}", g_cancels);
	std::println("generated_markets: {}, generated_limits: {}, generated_bids: {}, generated_asks: {}",
		g_generatedMarkets,
		g_generatedLimits,
		g_generatedBids,
		g_generatedLimits + g_generatedMarkets - g_generatedBids);
	std::println("book state: ask: {}, bid: {}",
		orderBooks[static_cast<size_t>(Instrument::Time)].bestIdx[0],
		orderBooks[static_cast<size_t>(Instrument::Time)].bestIdx[1]);
	//	std::println("instructions  : {}", instructions.read());
	//	std::println("branches      : {}", branches.read());
	//	std::println("branch misses : {}", branchMisses.read());
	//	std::println("miss rate     : {}",
	//		   branchMisses.read() / branches.read());
	processResponses.join();
	return 0;
}

[[clang::xray_always_instrument]] void matchAllOrders(std::vector<OrderBook>& orderBooks,
	dro::SPSCQueue<MessageResponse, 0, std::pmr::polymorphic_allocator<MessageResponse> >& processedQueue,
	std::vector<uint64_t>& durations
	)
{

	NewRequest vOrdMsg{};
	while (q.try_pop(vOrdMsg))
	{
		const auto start = __rdtsc();
		_mm_lfence();
		uint32_t aux{0};
		std::visit(
			overloaded{[&orderBooks, &processedQueue](NewOrderRequest& ordMsg) -> void
			           {
				           if (ordMsg.order.quantityLots == 0) [[unlikely]]
				           {
					           (void)processedQueue.try_emplace(
						           MessageResponse{.oOrder = std::nullopt,
						                           .result = MessageResponse::Result::Rejected});
					           return;
				           }
				           auto& symbol = orderBooks[static_cast<size_t>(ordMsg.instrumentId)];

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
			           },
			           [&orderBooks, &processedQueue](CancelOrderRequest& cancelMsg) -> void
			           {
				           auto& symbol = orderBooks[static_cast<size_t>(cancelMsg.instrumentId)];
				           MessageResponse result = HandleCancellation(cancelMsg, symbol);
				           (void)processedQueue.try_emplace(std::move(result));
			           }},
			vOrdMsg);

		_mm_lfence();
		const auto end = __rdtscp(&aux);

		durations.emplace_back(end - start);
	}
	g_done.store(true, std::memory_order::relaxed);
}

[[clang::xray_always_instrument]] MessageResponse HandleMarketOrder(NewOrderRequest& ordMsg,
	OrderBook& symbol,
	size_t limit)
{
	g_marketOrders++;
	bool orderIsBuy = ordMsg.order.quantityLots > 0; // 1 for buy, 0 for sell
	size_t& bestIdx = symbol.bestIdx[!orderIsBuy];   // looking for best bid when order type is sell and vv.
	int64_t initialQuantity = ordMsg.order.quantityLots;

	// 1 if order is buy - price moves up to next more expensive ask after you fill current level
	//-1 if order is sell - price moves down to next cheaper bid after you fill current level
	int direction = orderIsBuy * 2 - 1;

	auto& orderCount = symbol.counts[!orderIsBuy];
	// quantityLots is positive if the order is a buy.
	// in that case, direction is 1 -> if we go under 0, it means  we are overfilled: end
	// quantityLots is negative if the order is a sell.
	// if we go above 0, it means we are overfilled: end -> direction is -1 so we flip it
	while (orderCount > 0 && bestIdx != invalidBestIdx && ordMsg.order.quantityLots * direction > 0)
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
		ordMsg.order.quantityLots += resting->order.quantityLots; // fill towards zero

		// remainder is left in the resting if we crossed over the 0 in the dir we are going.
		const bool remainderLeftInResting = ordMsg.order.quantityLots * direction < 0;

		// if we filled the incoming - existing could be partially fulfilled now, so give back what we took.
		// keep the remainder if there is any left (there will be, if we overfilled the incoming order).
		resting->order.quantityLots = remainderLeftInResting * ordMsg.order.quantityLots;

		// side-effects
		symbol.fulfilled[symbol.filledWriteIdx & (symbol.fulfilled.size() - 1)] = MessageResponse{resting->order,
			remainderLeftInResting ? MessageResponse::Result::PartiallyFilled : MessageResponse::Result::Filled};

		symbol.filledWriteIdx++;
		// decrement from counts
		orderCount -= !remainderLeftInResting;
		if (!remainderLeftInResting)
		{
			intrusiveList::unlink(resting);
			symbol.idToOrder.erase(resting->order.id);
			symbol.ordersData.ReleaseToFree(resting);
		}
	}
	// if no more orders left, set bestIdx to SIZE_MAX
	if (orderCount == 0) [[unlikely]]
	{
		bestIdx = invalidBestIdx;
	}

	bool unfilled = ordMsg.order.quantityLots * direction > 0;
	bool rejected = ordMsg.order.quantityLots == initialQuantity;
	// if the order is filled, there will be a remainder inside (equal to the leftover in the resting limit order it filled against)
	ordMsg.order.quantityLots *= unfilled;

	constexpr static std::array<MessageResponse::Result, 3> resArray{
		MessageResponse::Result::Filled, MessageResponse::Result::PartiallyFilled, MessageResponse::Result::Rejected};
	return MessageResponse{.oOrder = ordMsg.order, .result = resArray[unfilled + rejected]};
}

[[clang::xray_always_instrument]] MessageResponse HandleLimitOrder(NewOrderRequest& ordMsg, OrderBook& symbol)
{

	g_limitOrders++;
	bool orderIsBuy = ordMsg.order.quantityLots > 0; // 1 for buy, 0 for sell

	const size_t& bestOppositeIdx = symbol.bestIdx[!orderIsBuy]; // looking for best bid when order type is sell and vv.

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
				assert(ordMsg.order.quantityLots == 0);
				return response;
			}
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

	OrderNode* incOrder = symbol.ordersData.GetFree();
	intrusiveList::append(currOrdersLevel.listSentinel, incOrder);

	incOrder->order = ordMsg.order;
	symbol.idToOrder[ordMsg.order.id] = incOrder;

	symbol.counts[orderIsBuy]++;

	// check if this is the new best price - update best idx to be that price level.
	if (bestSameIdx == invalidBestIdx || (orderIsBuy && ordMsg.priceTicksLimit > bestSameIdx)
	    || (!orderIsBuy && ordMsg.priceTicksLimit < bestSameIdx))
	{
		bestSameIdx = ordMsg.priceTicksLimit;
	}

	return MessageResponse{.oOrder = std::move(ordMsg.order), .result = MessageResponse::Result::Resting};
}

MessageResponse HandleCancellation(CancelOrderRequest& cancelMsg, OrderBook& symbol)
{
	g_cancels++;
	if (auto extracted = symbol.idToOrder.find(cancelMsg.toCancel); extracted != symbol.idToOrder.end())
	{
		Order order = std::move(extracted->second->order);
		symbol.counts[order.quantityLots > 0]--;
		intrusiveList::unlink(extracted->second);
		symbol.ordersData.ReleaseToFree(extracted->second);
		symbol.idToOrder.erase(extracted);
		return MessageResponse{order, MessageResponse::Result::Cancelled};
	}
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
	ordersData(allocator)
{
	free_list = &ordersData[0];
	OrderNode* tail = free_list;
	for (int64_t i = 1; i < ordersData.size(); i++)
	{
		tail->next = &ordersData[i];
		tail = tail->next;
	}
}

OrderNode* OrdersData::GetFree()
{
	assert(free_list); // check if we have ran out of memory.
	auto ret = std::exchange(free_list, free_list->next);
	ret->next = nullptr;
	return ret;
}

void OrdersData::ReleaseToFree(OrderNode* freed)
{
	auto temp = free_list;
	free_list = freed;
	free_list->next = temp;
}
}