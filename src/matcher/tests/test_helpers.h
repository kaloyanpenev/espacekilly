#pragma once

#include <gtest/gtest.h>

#include <matcher/matcher.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Shared helpers for the matcher unit tests.
//
// The handlers operate on an OrderBook whose state is entirely public, so the
// tests drive HandleLimitOrder / HandleMarketOrder directly and assert the
// resulting book state. Conventions (see DESIGN.md / matcher.h):
//   - quantityLots > 0 == buy, < 0 == sell
//   - index 0 == ask/sell side, index 1 == bid/buy side
//   - bestIdx entry == SIZE_MAX means "unset"
//   - fulfilled[] is a monotonic ring; only [0, filledWriteIdx) is committed
namespace th
{

inline constexpr std::size_t kUnset = std::numeric_limits<std::size_t>::max();

// ---- message builders -----------------------------------------------------

inline matcher::OrderMessage limitOrder(std::size_t id, std::int64_t qty, std::size_t tick, bool buy)
{
	matcher::OrderMessage m{};
	m.instrumentId = matcher::Instrument::Time;
	m.order = {.id = id, .quantityLots = qty};
	m.priceTicksLimit = tick;
	m.orderType = buy ? matcher::OrderType::BuyLimit : matcher::OrderType::SellLimit;
	return m;
}

inline matcher::OrderMessage marketOrder(std::size_t id, std::int64_t qty, bool buy)
{
	matcher::OrderMessage m{};
	m.instrumentId = matcher::Instrument::Time;
	m.order = {.id = id, .quantityLots = qty};
	m.orderType = buy ? matcher::OrderType::Buy : matcher::OrderType::Sell;
	return m;
}

// ---- driving the handlers -------------------------------------------------

// The handlers mutate the message in place (consuming quantityLots), so the
// lvalue overloads let a caller inspect the leftover afterwards; the rvalue
// overloads exist purely so call sites can pass a freshly-built temporary.
inline bool restOrMatch(matcher::OrderBook& book, matcher::OrderMessage& msg)
{
	return matcher::HandleLimitOrder(msg, book);
}
inline bool restOrMatch(matcher::OrderBook& book, matcher::OrderMessage&& msg)
{
	return restOrMatch(book, msg);
}

// Mirrors startMatch: a market buy may walk up to SIZE_MAX, a market sell down to 0.
inline bool marketInto(matcher::OrderBook& book, matcher::OrderMessage& msg)
{
	const bool buy = msg.orderType == matcher::OrderType::Buy;
	return matcher::HandleMarketOrder(msg, book, buy ? kUnset : 0UL);
}
inline bool marketInto(matcher::OrderBook& book, matcher::OrderMessage&& msg)
{
	return marketInto(book, msg);
}

// ---- assertions -----------------------------------------------------------

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

// Exact committed fulfilled sequence: slots [0, filledWriteIdx).
inline void expectFulfilled(matcher::OrderBook& book, const std::vector<std::size_t>& ids)
{
	ASSERT_EQ(book.filledWriteIdx, ids.size()) << "number of committed fulfilled ids";
	const std::size_t mask = book.fulfilled.size() - 1;
	for (std::size_t i = 0; i < ids.size(); ++i)
	{
		EXPECT_EQ(book.fulfilled[i & mask], ids[i]) << "fulfilled[" << i << "]";
	}
}

// Verifies the active window of a price level: read/write indices and the
// orders sitting in [readIndex, writeIndex). Indices are taken modulo capacity.
inline void expectLevel(matcher::OrderBook& book, std::size_t tick, std::size_t readIdx, std::size_t writeIdx,
	const std::vector<matcher::Order>& active)
{
	auto& level = book.orders[tick];
	EXPECT_EQ(level.readIndex, readIdx) << "level " << tick << " readIndex";
	EXPECT_EQ(level.writeIndex, writeIdx) << "level " << tick << " writeIndex";
	ASSERT_EQ(writeIdx - readIdx, active.size()) << "level " << tick << " active size";
	const std::size_t mask = level.orders.size() - 1;
	for (std::size_t i = 0; i < active.size(); ++i)
	{
		auto& got = level.orders[(readIdx + i) & mask];
		EXPECT_EQ(got.id, active[i].id) << "level " << tick << " order[" << i << "].id";
		EXPECT_EQ(got.quantityLots, active[i].quantityLots) << "level " << tick << " order[" << i << "].qty";
	}
}

inline void expectEmptyLevel(matcher::OrderBook& book, std::size_t tick)
{
	auto& level = book.orders[tick];
	EXPECT_EQ(level.readIndex, level.writeIndex) << "level " << tick << " should be empty";
}

} // namespace th
