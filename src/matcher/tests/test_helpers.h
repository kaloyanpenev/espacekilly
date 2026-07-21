#pragma once

#include <gtest/gtest.h>

#include <matcher/matcher.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Shared helpers for the matcher unit tests.
//
// The handlers operate on an OrderBook whose state is entirely public, so the
// tests drive HandleLimitOrder / HandleMarketOrder / HandleCancellation
// directly and assert the resulting book state. Conventions (see matcher.h):
//   - quantityLots > 0 == buy, < 0 == sell
//   - index 0 == ask/sell side, index 1 == bid/buy side
//   - bestIdx entry == invalidBestIdx (SIZE_MAX) means "unset"
//   - each price level is an intrusive circular list; FIFO order is
//     sentinel.next (head, oldest) .. sentinel.prev (tail, newest)
//   - fulfilled[] is a monotonic ring holding one MessageResponse per RESTING
//     order touched during a match (Filled when fully consumed, qty 0, or
//     PartiallyFilled with the remaining qty). The incoming order's own
//     response is the handler's return value and never enters the ring.
//   - ticks must stay < kPriceLevelCount (128); the rest path indexes
//     orders[tick] unmasked.
namespace th
{

inline constexpr std::size_t kUnset = matcher::invalidBestIdx;
using Result = matcher::MessageResponse::Result;

// ---- message builders -----------------------------------------------------

inline matcher::NewOrderRequest limitOrder(std::size_t id, std::int64_t qty, std::size_t tick, bool buy)
{
	matcher::NewOrderRequest m{};
	m.instrumentId = matcher::Instrument::Time;
	m.order = {.id = id, .quantityLots = qty};
	m.priceTicksLimit = tick;
	m.orderType = buy ? matcher::OrderType::BuyLimit : matcher::OrderType::SellLimit;
	return m;
}

inline matcher::NewOrderRequest marketOrder(std::size_t id, std::int64_t qty, bool buy)
{
	matcher::NewOrderRequest m{};
	m.instrumentId = matcher::Instrument::Time;
	m.order = {.id = id, .quantityLots = qty};
	m.orderType = buy ? matcher::OrderType::Buy : matcher::OrderType::Sell;
	return m;
}

// ---- driving the handlers -------------------------------------------------

// The handlers mutate the message in place (consuming quantityLots), so the
// lvalue overloads let a caller inspect the leftover afterwards; the rvalue
// overloads exist purely so call sites can pass a freshly-built temporary.
inline matcher::MessageResponse restOrMatch(matcher::OrderBook& book, matcher::NewOrderRequest& msg)
{
	return matcher::HandleLimitOrder(msg, book);
}
inline matcher::MessageResponse restOrMatch(matcher::OrderBook& book, matcher::NewOrderRequest&& msg)
{
	return restOrMatch(book, msg);
}

// Mirrors matchAllOrders: a market buy may walk up to SIZE_MAX, a market sell down to 0.
inline matcher::MessageResponse marketInto(matcher::OrderBook& book, matcher::NewOrderRequest& msg)
{
	const bool buy = msg.orderType == matcher::OrderType::Buy;
	return matcher::HandleMarketOrder(msg, book, buy ? SIZE_MAX : 0UL);
}
inline matcher::MessageResponse marketInto(matcher::OrderBook& book, matcher::NewOrderRequest&& msg)
{
	return marketInto(book, msg);
}

inline matcher::MessageResponse cancelOrder(matcher::OrderBook& book, std::size_t id)
{
	matcher::CancelOrderRequest c{.msgId = 0, .toCancel = id, .instrumentId = matcher::Instrument::Time};
	return matcher::HandleCancellation(c, book);
}

// ---- assertions -----------------------------------------------------------

inline void expectResponse(const matcher::MessageResponse& res, Result result, std::size_t id, std::int64_t qty)
{
	EXPECT_EQ(res.result, result) << "response result";
	ASSERT_TRUE(res.oOrder.has_value()) << "response should carry the order";
	EXPECT_EQ(res.oOrder->id, id) << "response order id";
	EXPECT_EQ(res.oOrder->quantityLots, qty) << "response order qty";
}

// For responses that carry no order (Rejected-on-ingest / NotFound).
inline void expectEmptyResponse(const matcher::MessageResponse& res, Result result)
{
	EXPECT_EQ(res.result, result) << "response result";
	EXPECT_FALSE(res.oOrder.has_value()) << "response should carry no order";
}

inline void expectBest(matcher::OrderBook& book, std::size_t ask, std::size_t bid)
{
	EXPECT_EQ(book.bestIdx[0], ask) << "best ask";
	EXPECT_EQ(book.bestIdx[1], bid) << "best bid";
}

inline void expectCounts(matcher::OrderBook& book, std::size_t ask, std::size_t bid)
{
	EXPECT_EQ(book.counts[0], ask) << "ask count";
	EXPECT_EQ(book.counts[1], bid) << "bid count";
}

// Orders resting at a level in FIFO (time-priority) order.
inline std::vector<matcher::Order> levelOrders(matcher::OrderBook& book, std::size_t tick)
{
	std::vector<matcher::Order> out;
	const matcher::OrderNode& sentinel = book.orders[tick].listSentinel;
	for (const matcher::OrderNode* n = sentinel.next; n != &sentinel; n = n->next)
	{
		out.push_back(n->order);
		if (out.size() > matcher::totalOrderCount) // corrupted list: bail instead of spinning
		{
			ADD_FAILURE() << "level " << tick << " list does not terminate";
			break;
		}
	}
	return out;
}

inline void expectLevel(matcher::OrderBook& book, std::size_t tick, const std::vector<matcher::Order>& active)
{
	const auto got = levelOrders(book, tick);
	ASSERT_EQ(got.size(), active.size()) << "level " << tick << " active size";
	for (std::size_t i = 0; i < active.size(); ++i)
	{
		EXPECT_EQ(got[i].id, active[i].id) << "level " << tick << " order[" << i << "].id";
		EXPECT_EQ(got[i].quantityLots, active[i].quantityLots) << "level " << tick << " order[" << i << "].qty";
	}
}

inline void expectEmptyLevel(matcher::OrderBook& book, std::size_t tick)
{
	EXPECT_TRUE(matcher::intrusiveList::isEmpty(book.orders[tick].listSentinel))
		<< "level " << tick << " should be empty";
}

// One expected entry in the fulfilled ring.
struct Fill
{
	std::size_t id;
	std::int64_t qty;
	Result result;
};

// Exact committed fulfilled sequence: slots [0, filledWriteIdx). Only valid
// while filledWriteIdx <= ring capacity (no wrap); wrap tests index manually.
inline void expectFulfilled(matcher::OrderBook& book, const std::vector<Fill>& fills)
{
	ASSERT_EQ(book.filledWriteIdx, fills.size()) << "number of committed fulfilled entries";
	const std::size_t mask = book.fulfilled.size() - 1;
	for (std::size_t i = 0; i < fills.size(); ++i)
	{
		const matcher::MessageResponse& got = book.fulfilled[i & mask];
		EXPECT_EQ(got.result, fills[i].result) << "fulfilled[" << i << "].result";
		ASSERT_TRUE(got.oOrder.has_value()) << "fulfilled[" << i << "] order";
		EXPECT_EQ(got.oOrder->id, fills[i].id) << "fulfilled[" << i << "].id";
		EXPECT_EQ(got.oOrder->quantityLots, fills[i].qty) << "fulfilled[" << i << "].qty";
	}
}

// Cross-checks the redundant book state against itself:
//   - counts[] match a full walk of every price level, split by qty sign
//   - idToOrder holds exactly the resting nodes (same size, each id maps to
//     the node that carries it)
inline void expectBookConsistent(matcher::OrderBook& book)
{
	std::size_t asks = 0;
	std::size_t bids = 0;
	std::size_t resting = 0;
	for (std::size_t tick = 0; tick < matcher::kPriceLevelCount; ++tick)
	{
		const matcher::OrderNode& sentinel = book.orders[tick].listSentinel;
		for (const matcher::OrderNode* n = sentinel.next; n != &sentinel; n = n->next)
		{
			ASSERT_LE(++resting, matcher::totalOrderCount) << "level " << tick << " list does not terminate";
			EXPECT_NE(n->order.quantityLots, 0) << "resting order " << n->order.id << " has zero qty";
			(n->order.quantityLots > 0 ? bids : asks)++;
			const auto it = book.idToOrder.find(n->order.id);
			ASSERT_NE(it, book.idToOrder.end()) << "resting order " << n->order.id << " missing from idToOrder";
			EXPECT_EQ(it->second, n) << "idToOrder points at a different node for id " << n->order.id;
		}
	}
	EXPECT_EQ(book.counts[0], asks) << "ask count vs book walk";
	EXPECT_EQ(book.counts[1], bids) << "bid count vs book walk";
	EXPECT_EQ(book.idToOrder.size(), asks + bids) << "idToOrder size vs book walk";
}

} // namespace th
