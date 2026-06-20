#include <matcher/matcher.h>

#include <memory_resource>
#include <ranges>
#include <utility>
#include <random>
#include <thread>

namespace
{
	size_t g_limitOrders = 0;
	size_t g_marketOrders = 0;
	size_t g_crossOrder = 0;
	size_t g_restingOrder = 0;
	size_t g_fullyFilledCrossOrder = 0;
	size_t g_generatedMarkets = 0;
	size_t g_generatedBids = 0;
	size_t g_generatedLimits = 0;
}

namespace matcher
{

OrderBook::OrderBook() :
	arenaBuffer(std::vector<std::byte>(orderBookArenaSize)),
	arena(arenaBuffer.data(), arenaBuffer.size(), std::pmr::null_memory_resource()),
	fulfilled(arena),
	orders{arena, arena}
{
}

std::vector<OrderBook> initOrderBooks()
{
	auto symbols = std::vector<OrderBook>(static_cast<std::size_t>(Instrument::Count));
	return symbols;
}

constexpr size_t generatedOrders = 100'000'000ul;

void CreateOrders(dro::SPSCQueue<OrderMessage>& queue)
{

	//std::random_device rd;  // a seed source for the random number engine
	std::mt19937 gen(75); // mersenne_twister_engine seeded with rd()

	size_t id = 0;
	for (size_t i = 0ul; i < generatedOrders; i++)
	{
		std::uniform_int_distribution<int64_t> distribLots(0, 20000);
		std::uniform_int_distribution<size_t> distribTicks(40, 60);
		std::uniform_int_distribution<int8_t> distribType(0, 3);

		// generate random order type
		int8_t ordTypeNum = distribType(gen);
		OrderType ordType = static_cast<OrderType>(distribType(gen));

		int64_t sign = (ordTypeNum < 2) ? 1 : -1; // -1 if selling
		int64_t lots = distribLots(gen) * sign;

		size_t ticks = distribTicks(gen);
		size_t ordIsLimit = (ordType == OrderType::BuyLimit || ordType == OrderType::SellLimit);
		g_generatedMarkets += !ordIsLimit;
		g_generatedLimits += ordIsLimit;
		g_generatedBids += ordTypeNum < 2;

		queue.emplace(OrderMessage{.order = {.id = id++, .quantityLots = lots},
				.instrumentId = Instrument::Time,
				.priceTicksLimit = ordIsLimit * ticks,
				.orderType = ordType});
	}
}

int startMatch()
{
	auto orderBooks = initOrderBooks();
	dro::SPSCQueue<OrderMessage> q{generatedOrders+1};

	//auto makeInput = std::jthread(&CreateOrders, std::ref(q));

	CreateOrders(q);
	//makeInput.join();
	// TODO: stop order book

	OrderMessage ordMsg{};
	const auto start = std::chrono::system_clock::now();
	while (q.try_pop(ordMsg))
	{
		auto& symbol = orderBooks[static_cast<size_t>(ordMsg.instrumentId)];

		// here we are not placing any new orders - we are simply filling it immediately with the best bid/ask.
		if (ordMsg.orderType == OrderType::Buy || ordMsg.orderType == OrderType::Sell)
		{
			//std::println("market order. Id: {}, quantity: {}, price: {}", ordMsg.order.id, ordMsg.order.quantityLots, ordMsg.priceTicksLimit);
			bool filled = HandleMarketOrder(
				ordMsg, symbol, ordMsg.orderType == OrderType::Buy ? std::numeric_limits<size_t>::max() : 0);
			if (!filled)
			{
				// return unableToFill;
			}
			// return filled;
		}
		// here we will be potentially matching at a more granular level
		else if (ordMsg.orderType == OrderType::BuyLimit || ordMsg.orderType == OrderType::SellLimit)
		{
			//std::println("limit order. Id: {}, quantity: {}, price: {}", ordMsg.order.id, ordMsg.order.quantityLots, ordMsg.priceTicksLimit);
			bool filled = HandleLimitOrder(ordMsg, symbol);
			if (!filled)
			{
				// return leftAsResting;
			}
			// return filled;
		}
	}
	const auto finish = std::chrono::system_clock::now();
	const size_t elapsed = std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();
	std::println("elapsed: {}", elapsed);
	std::println("elapsed ns per order: {}", static_cast<double>((finish - start).count()) / generatedOrders);
	std::println("executed_limits: {}, resting_crosses: {}, fully_filled_crosses: {}, resting: {}",
				 g_limitOrders,
				 g_crossOrder - g_fullyFilledCrossOrder,
				 g_fullyFilledCrossOrder,
				 g_restingOrder);
	std::println("executed_markets: {}", g_marketOrders - g_crossOrder);
	std::println("generated_markets: {}, generated_limits: {}, generated_bids: {}, generated_asks: {}", g_generatedMarkets, g_generatedLimits, g_generatedBids, g_generatedLimits + g_generatedMarkets - g_generatedBids);
	std::println("book state: ask: {}, bid: {}", orderBooks[static_cast<size_t>(Instrument::Time)].bestIdx[0],  orderBooks[static_cast<size_t>(Instrument::Time)].bestIdx[1]);

	return 0;
}

bool HandleMarketOrder(OrderMessage& ordMsg, OrderBook& symbol, size_t limit)
{
	g_marketOrders++;
	bool orderIsBuy = ordMsg.orderType == OrderType::Buy || ordMsg.orderType == OrderType::BuyLimit; // 1 for buy, 0 for sell
	size_t& bestIdx = symbol.bestIdx[!orderIsBuy];         // looking for best bid when order type is sell and vv.

	// 1 if order is buy - price moves up to next more expensive ask after you fill current level
	//-1 if order is sell - price moves down to next cheaper bid after you fill current level
	int direction = orderIsBuy * 2 - 1;

	auto& orderCount = symbol.counts[!orderIsBuy];
	// quantityLots is positive if the order is a buy.
	// in that case, direction is 1 -> if we go under 0, it means  we are overfilled: end
	// quantityLots is negative if the order is a sell.
	// if we go above 0, it means we are overfilled: end -> direction is -1 so we flip it
	while (orderCount > 0 &&
		bestIdx != invalidBestIdx &&
		ordMsg.order.quantityLots * direction > 0)
	{
		auto& currTickOrders = symbol.orders[bestIdx & (symbol.orders.size() - 1)];
		if (currTickOrders.readIndex == currTickOrders.writeIndex) [[unlikely]]
		{
			// level is empty; look for more expensive asks or cheaper bids
			bestIdx += direction;
			// check if we're past the limit - in that case, stop.
			// limit == inf if order is market buy, limit == 0 if order is market sell
			// bestIdx can wrap around to size_max if this is a market sell order, and break due to bestIdx == invalidBestIdx
			if ((orderIsBuy && bestIdx > limit) || (!orderIsBuy && bestIdx < limit))
			{
				break; // could not fill
			}
			continue; // we have updated the level - start over.
		}

		auto& readIdx = currTickOrders.readIndex;
		auto& resting = currTickOrders.orders[readIdx & (currTickOrders.orders.size() - 1)];

		ordMsg.order.quantityLots += resting.quantityLots; // fill towards zero

		// remainder is left in the resting if we crossed over the 0 in the dir we are going.
		const bool remainderLeftInResting = ordMsg.order.quantityLots * direction < 0;

		// if we filled the incoming - existing could be partially fulfilled now, so give back what we took.
		// keep the remainder if there is any left (there will be, if we overfilled the incoming order).
		resting.quantityLots = remainderLeftInResting * ordMsg.order.quantityLots;

		// always stage existing id
		// it will get overwritten by the inc if there is remainder left.
		symbol.fulfilled[symbol.filledWriteIdx & (symbol.fulfilled.size() - 1)] = resting.id;
		// only leave inside fulfilled if there is no remainder.
		symbol.filledWriteIdx += !remainderLeftInResting;
		// decrement from counts
		orderCount -= !remainderLeftInResting;
		resting.id *= remainderLeftInResting; // clear the id if there is no remainder.

		readIdx += !remainderLeftInResting; // increment if inc was NOT overfilled.
	}
	// if no more orders left, set bestIdx to SIZE_MAX
	if (orderCount == 0) [[unlikely]]
	{
		bestIdx = invalidBestIdx;
	}

	bool filled  = ordMsg.order.quantityLots * direction <= 0;
	// if the order is filled, there will be a remainder inside (equal to the leftover in the resting limit order it filled against)
	ordMsg.order.quantityLots *= !filled;
	// write the fulfilled order -
	// set the last entry to 0 if not filled (as that would contain the last staged resting order) or
	// set the current order if filled.
	symbol.fulfilled[symbol.filledWriteIdx & (symbol.fulfilled.size() - 1)] = filled * ordMsg.order.id;
	symbol.filledWriteIdx += filled; // increment if we wrote the incoming order id - otherwise we've 0-ed it, dont inc.

	return filled;
}

bool HandleLimitOrder(OrderMessage& ordMsg, OrderBook& symbol)
{

	g_limitOrders++;
	bool orderIsBuy = ordMsg.orderType == OrderType::BuyLimit;

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
			bool filled = HandleMarketOrder(ordMsg, symbol, ordMsg.priceTicksLimit);
			// we have now handled whatever we can - check if we could fully fulfill the order.
			if (filled)
			{
				g_fullyFilledCrossOrder++;
				assert(ordMsg.order.quantityLots == 0);
				return filled;
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

	size_t& bestSameIdx = symbol.bestIdx[orderIsBuy];      // looking for best bid when order type is sell and vv.

	// TODO NOTE: assumes orders is not a ringbuffer. if ringbuffer, need to search for the level with that price ticks.
	auto& currOrdersLevel = symbol.orders[ordMsg.priceTicksLimit];
	currOrdersLevel.orders[currOrdersLevel.writeIndex++ & (currOrdersLevel.orders.size() - 1)] = std::move(ordMsg.order);
	if (currOrdersLevel.writeIndex - currOrdersLevel.readIndex == currOrdersLevel.orders.size()) [[unlikely]]
	{
		std::println("full orders level: {}, readIdx = {}, writeIdx = {} ", ordMsg.priceTicksLimit, currOrdersLevel.readIndex, currOrdersLevel.writeIndex);
		throw std::exception();
	}
	symbol.counts[orderIsBuy]++;

	// check if this is the new best price - update best idx to be that price level.
	if (bestSameIdx == invalidBestIdx ||
		(orderIsBuy && ordMsg.priceTicksLimit > bestSameIdx) || (!orderIsBuy && ordMsg.priceTicksLimit < bestSameIdx))
	{
		bestSameIdx = ordMsg.priceTicksLimit;
	}

	return false; // unfulfilled - left as resting.
}

OrdersForTick::OrdersForTick(std::pmr::monotonic_buffer_resource& allocator) : orders(allocator) {}
}