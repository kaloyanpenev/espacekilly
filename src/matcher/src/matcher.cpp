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

	// for each symbol
	for (auto& book : symbols)
	{
		// allocate the "hot orders" which are close to the spread
		// TODO: Set best ask and bid from input data
		constexpr size_t spread = 5000; // TODO set from input data
		size_t startOffset = spread - (kOrdersPerTick / 2);

		// initialize all small price level arrays
		for (auto& level : book.orders)
		{
			level.priceInTicks = startOffset++;
		}
	}
	return symbols;
}

int startMatch(std::shared_ptr<dro::SPSCQueue<OrderMessage>> messageQueue)
{
	auto orderBooks = initOrderBooks();
	OrderMessage order1{.order = {.id = 198259182, .quantityLots = 200},
		.instrumentId = Instrument::Time,
		.priceTicksLimit = 500,
		.orderType = OrderType::Sell};

	dro::SPSCQueue<OrderMessage> q{15000};
	q.emplace(std::move(order1));
	// handle cancel if it is partially fulfilled (with message to owner)

	OrderMessage ordMsg{};
	while (q.try_pop(ordMsg))
	{
		auto& symbol = orderBooks[static_cast<size_t>(ordMsg.instrumentId)];
		if (ordMsg.orderType == OrderType::Buy)
		{
			while (ordMsg.order.quantityLots > 0)
			{
				auto& asksVec =
					symbol.orders[symbol.bestAskIdx & (symbol.orders.size() - 1)]; // asks are all negative lots
				if (asksVec.readIndex == asksVec.writeIndex) [[unlikely]]
				{
					// level is empty; look for more expensive sells
					symbol.bestAskIdx++;
					continue;
				}

				auto& readIdx = asksVec.readIndex;
				auto& resting = asksVec.orders[readIdx];

				ordMsg.order.quantityLots += resting.quantityLots; // subtract exist from inc
				const int64_t remainderLeftInResting = ordMsg.order.quantityLots < 0;

				// if we filled the incoming - existing could be partially fulfilled now, so give back what we took.
				resting.quantityLots = remainderLeftInResting * ordMsg.order.quantityLots;

				// always stage existing id
				// it will get overwritten by the inc if there is remainder left.
				symbol.fulfilled[symbol.filledWriteIdx & (symbol.fulfilled.size() - 1)] = resting.id;
				// only leave inside fulfilled if there is no remainder.
				symbol.filledWriteIdx += !remainderLeftInResting;
				resting.id *= remainderLeftInResting; // clear the id if there is no remainder.

				readIdx += !remainderLeftInResting; // increment if inc was NOT overfilled.
			}
			// mark incoming filled - it will overwrite the last resting order which has remainder.
			symbol.fulfilled[symbol.filledWriteIdx++ & (symbol.fulfilled.size() - 1)] = ordMsg.order.id;
		}
	}
	return 0;
}

OrdersForTick::OrdersForTick(std::pmr::monotonic_buffer_resource& allocator) : orders(allocator) {}
}