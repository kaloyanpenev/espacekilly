#include <matcher/matcher.h>

#include <memory_resource>
#include <ranges>
#include <utility>

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

int startMatch()
{
	auto orderBooks = initOrderBooks();
	dro::SPSCQueue<OrderMessage> q{15000};

	// 1) SellLimit rests at tick 502 — no matching buy exists yet
	q.emplace(OrderMessage{.order = {.id = 1001, .quantityLots = -100},
		.instrumentId = Instrument::Time,
		.priceTicksLimit = 502,
		.orderType = OrderType::SellLimit});

	// 2) SellLimit rests at tick 503 — also rests, one level above
	q.emplace(OrderMessage{.order = {.id = 1002, .quantityLots = -50},
		.instrumentId = Instrument::Time,
		.priceTicksLimit = 503,
		.orderType = OrderType::SellLimit});

	// 3) BuyLimit at tick 500 — below all resting sells, so it just rests
	q.emplace(OrderMessage{.order = {.id = 2001, .quantityLots = 75},
		.instrumentId = Instrument::Time,
		.priceTicksLimit = 500,
		.orderType = OrderType::BuyLimit});

	// 4) BuyLimit at tick 503 — crosses the resting sell at 502,
	//    should fill 100 lots against order 1001, leaving 20 lots unfilled,
	//    then attempt to fill against the sell at 503 (order 1002)
	q.emplace(OrderMessage{.order = {.id = 2002, .quantityLots = 120},
		.instrumentId = Instrument::Time,
		.priceTicksLimit = 503,
		.orderType = OrderType::BuyLimit});

	// 5) Market Sell — fills against best resting buy (order 2001 at tick 500)
	q.emplace(OrderMessage{
		.order = {.id = 3001, .quantityLots = -40}, .instrumentId = Instrument::Time, .orderType = OrderType::Sell});

	// 6) Market Buy — fills against best resting sell (whatever remains)
	q.emplace(OrderMessage{
		.order = {.id = 3002, .quantityLots = 30}, .instrumentId = Instrument::Time, .orderType = OrderType::Buy});

	// TODO: stop order book

	OrderMessage ordMsg{};
	while (q.try_pop(ordMsg))
	{
		auto& symbol = orderBooks[static_cast<size_t>(ordMsg.instrumentId)];

		// here we are not placing any new orders - we are simply filling it immediately with the best bid/ask.
		if (ordMsg.orderType == OrderType::Buy || ordMsg.orderType == OrderType::Sell)
		{
			std::println("market order. Id: {}, quantity: {}, price: {}", ordMsg.order.id, ordMsg.order.quantityLots, ordMsg.priceTicksLimit);
			bool filled = HandleMarketOrder(
				ordMsg, symbol, ordMsg.orderType == OrderType::Buy ? std::numeric_limits<size_t>::max() : 0);
			if (!filled)
			{
				throw "TODO handle me: not enough resting orders to complete order!";
			}
		}
		// here we will be potentially matching at a more granular level
		else if (ordMsg.orderType == OrderType::BuyLimit || ordMsg.orderType == OrderType::SellLimit)
		{
			std::println("limit order. Id: {}, quantity: {}, price: {}", ordMsg.order.id, ordMsg.order.quantityLots, ordMsg.priceTicksLimit);
			bool filled = HandleLimitOrder(ordMsg, symbol);
			if (!filled)
			{
				std::println("limit order unfulfilled. Id: {}, quantity: {}, price: {}", ordMsg.order.id, ordMsg.order.quantityLots, ordMsg.priceTicksLimit);
			}

		}
	}

	const auto& symbol = orderBooks[static_cast<size_t>(Instrument::Time)];
	for (const auto& fulfilled : symbol.fulfilled)
	{
		if (fulfilled != 0)
		{
			std::println("fulfilled: {}", fulfilled);
		}
	}
	return 0;
}

bool HandleMarketOrder(OrderMessage& ordMsg, OrderBook& symbol, size_t limit)
{
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
		bestIdx != SIZE_MAX &&
		ordMsg.order.quantityLots * direction > 0)
	{
		auto& currTickOrders = symbol.orders[bestIdx & (symbol.orders.size() - 1)];
		if (currTickOrders.readIndex == currTickOrders.writeIndex) [[unlikely]]
		{
			// level is empty; look for more expensive asks or cheaper bids
			bestIdx += direction;
			// check if we're past the limit - in that case, stop.
			// in a market sell, limit will be 0UL, so bestIdx < limit is always false:
			// 		- in this case, we rely on underflow to max, which breaks the loop from the while condition and sets bestIdx to the correct sentinel.
			if ((orderIsBuy && bestIdx > limit) || (!orderIsBuy && bestIdx < limit))
			{
				break; // could not fill
			}
			continue; // we have updated the level - start over.
		}

		auto& readIdx = currTickOrders.readIndex;
		auto& resting = currTickOrders.orders[readIdx];

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
		bestIdx = SIZE_MAX;
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

	bool orderIsBuy = ordMsg.orderType == OrderType::BuyLimit;

	const size_t& bestOppositeIdx = symbol.bestIdx[!orderIsBuy]; // looking for best bid when order type is sell and vv.

	// 1) match - if the incoming crosses over the other type's best idx
	// then fill it, starting with the best idx level. any remainder, leave in that level.

	if (bestOppositeIdx != SIZE_MAX)
	{
		const bool betterThanBestBuy = orderIsBuy && ordMsg.priceTicksLimit >= bestOppositeIdx;
		const bool betterThanBestSell = !orderIsBuy && ordMsg.priceTicksLimit <= bestOppositeIdx;
		if (betterThanBestBuy || betterThanBestSell)
		{

			bool filled = HandleMarketOrder(ordMsg, symbol, ordMsg.priceTicksLimit);
			// we have now handled whatever we can - check if we could fully fulfill the order.
			if (filled)
			{
				assert(ordMsg.order.quantityLots == 0);
				return filled;
			}
		}
	}
	// 2) no immediate match or resting remainder left:
	// add the order to its requested level

	if (symbol.bestIdx[orderIsBuy] == SIZE_MAX)
	{
		symbol.bestIdx[orderIsBuy] = ordMsg.priceTicksLimit;
	}

	size_t& bestSameIdx = symbol.bestIdx[orderIsBuy];      // looking for best bid when order type is sell and vv.

	// TODO NOTE: assumes orders is not a ringbuffer. if ringbuffer, need to search for the level with that price ticks.
	auto& currOrdersLevel = symbol.orders[ordMsg.priceTicksLimit];
	currOrdersLevel.orders[currOrdersLevel.writeIndex++ & (currOrdersLevel.orders.size() - 1)] = std::move(ordMsg.order);
	if (currOrdersLevel.writeIndex - currOrdersLevel.readIndex == currOrdersLevel.orders.size() - 1) [[unlikely]]
	{
		std::println("full orders level: {}, readIdx = {}, writeIdx = {} ", ordMsg.priceTicksLimit, currOrdersLevel.readIndex, currOrdersLevel.writeIndex);
	}
	symbol.counts[orderIsBuy]++;

	// check if this is the new best price - update best idx to be that price level.
	if ((orderIsBuy && ordMsg.priceTicksLimit > bestSameIdx)
		|| (!orderIsBuy && ordMsg.priceTicksLimit < bestSameIdx))
	{
		bestSameIdx = ordMsg.priceTicksLimit;
	}

	return false; // unfulfilled - left as resting.
}

OrdersForTick::OrdersForTick(std::pmr::monotonic_buffer_resource& allocator) : orders(allocator) {}
}