#include <matcher/matcher.h>

#include <memory_resource>
#include <ranges>
#include <utility>

namespace matcher {

	OrderBook::OrderBook() :
			arenaBuffer(std::vector<std::byte>(orderBookArenaSize)),
			arena(arenaBuffer.data(), arenaBuffer.size(), std::pmr::null_memory_resource()),
			fulfilled(),
			orders{arena, arena} {

	}

	std::vector<OrderBook> initOrderBooks() {
		auto symbols = std::vector<OrderBook>(static_cast<std::size_t>(Instrument::Count));

		for (auto &book: symbols) {
			size_t priceOffset = 500;
			book.tickOffset = priceOffset;
			for (auto &tickLevel: book.orders) {
				tickLevel.priceInTicks = priceOffset++;
			}
		}
		return symbols;
	}

	int startMatch() {
		auto orderBooks = initOrderBooks();
		OrderMessage order1{.order = {.id = 198259182, .quantityLots = 200},
				.instrumentId = Instrument::Time,
				.priceTicksLimit = 500,
				.orderType = OrderType::Sell};

		dro::SPSCQueue<OrderMessage> q{15000};
		q.emplace(std::move(order1));

		// TODO: stop order book

		OrderMessage ordMsg{};
		while (q.try_pop(ordMsg)) {
			auto &symbol = orderBooks[static_cast<size_t>(ordMsg.instrumentId)];

			// here we are not placing any new orders - we are simply filling it immediately with the best bid/ask.
			if (ordMsg.orderType == OrderType::Buy || ordMsg.orderType == OrderType::Sell) {
				HandleMarketOrder(ordMsg, symbol, ordMsg.orderType == OrderType::Buy ? std::numeric_limits<size_t>::max() : 0);
			}
				// here we will be potentially matching at a more granular level
			else if (ordMsg.orderType == OrderType::BuyLimit || ordMsg.orderType == OrderType::SellLimit) {
				HandleLimitOrder(ordMsg, symbol);
			}
		}
		return 0;
	}

	void HandleMarketOrder(OrderMessage &ordMsg, OrderBook &symbol, size_t limit) {
		bool orderIsBuy = ordMsg.orderType == OrderType::Buy; // 1 for buy, 0 for sell
		size_t &bestIdx = symbol.bestIdx[orderIsBuy]; // looking for best bid when order type is sell and vv.

		// 1 if order is buy - price moves up to next more expensive ask after you fill current level
		//-1 if order is sell - price moves down to next cheaper bid after you fill current level
		int direction = orderIsBuy * 2 - 1;

		auto &orderCount = symbol.counts[orderIsBuy];
		// quantityLots is positive if the order is a buy.
		// in that case, direction is 1 -> if we go under 0, it means  we are overfilled: end
		// quantityLots is negative if the order is a sell.
		// if we go above 0, it means we are overfilled: end -> direction is -1 so we flip it
		while (orderCount > 0 && ordMsg.order.quantityLots * direction > 0) {
			auto &currTickOrders =
					symbol.orders[bestIdx & (symbol.orders.size() - 1)];
			if (currTickOrders.readIndex == currTickOrders.writeIndex) [[unlikely]] {
				// level is empty; look for more expensive asks or cheaper bids
				bestIdx += direction;
				// check if we're past the limit - in that case, stop.
				if ((orderIsBuy && bestIdx > limit) || (!orderIsBuy && bestIdx < limit))
				{
					return;
				}
				continue;
			}

			auto &readIdx = currTickOrders.readIndex;
			auto &resting = currTickOrders.orders[readIdx];

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
		// mark incoming filled - it will overwrite the last resting order which has remainder.
		symbol.fulfilled[symbol.filledWriteIdx++ & (symbol.fulfilled.size() - 1)] = ordMsg.order.id;

		if (orderCount == 0 && ordMsg.order.quantityLots * direction > 0) {
			// unfulfilled - report as unfulfilled.
			throw "handle_me";
		}
	}

	void HandleLimitOrder(OrderMessage &ordMsg, OrderBook &symbol) {

		bool orderIsBuy = ordMsg.orderType == OrderType::BuyLimit;

		size_t &bestSameIdx = symbol.bestIdx[orderIsBuy]; // looking for best bid when order type is sell and vv.
		size_t &bestOppositeIdx = symbol.bestIdx[!orderIsBuy]; // looking for best bid when order type is sell and vv.

		int direction = orderIsBuy * 2 - 1;

		// 1) match - if the incoming crosses over the other type's best idx
		// then fill it, starting with the best idx level. any remainder, leave in that level.

		const bool betterThanBestBuy = orderIsBuy && ordMsg.priceTicksLimit >= bestOppositeIdx;
		const bool betterThanBestSell = !orderIsBuy && ordMsg.priceTicksLimit <= bestOppositeIdx;

		if (betterThanBestBuy || betterThanBestSell) {

			HandleMarketOrder(ordMsg, symbol, ordMsg.priceTicksLimit);
			// we have now handled whatever we can - check if we could fully fulfill the order.
			if (ordMsg.order.quantityLots * direction <= 0)
			{
				return;
			}
		}
		// 2) no immediate match or remainder left:
		// add the order to its requested level
		size_t currTickIdx = ordMsg.priceTicksLimit - symbol.tickOffset;
		auto &currOrdersLevel = symbol.orders[currTickIdx];
		currOrdersLevel.orders[currOrdersLevel.writeIndex] = std::move(ordMsg.order);

		// check if this is the new best price - update best idx to be that price level.
		if ((orderIsBuy && currOrdersLevel.priceInTicks > bestSameIdx) ||
			(!orderIsBuy && currOrdersLevel.priceInTicks < bestSameIdx))
		{
			bestSameIdx = currTickIdx;
		}
	}

	OrdersForTick::OrdersForTick(std::pmr::monotonic_buffer_resource &allocator) : orders(allocator) {
	}
}