#include <gtest/gtest.h>

#include <matcher/matcher.h>

#include "test_helpers.h"

using namespace th;
using matcher::Order;
using matcher::OrderBook;

namespace
{
struct EdgeTest : ::testing::Test
{
	OrderBook book;
};
} // namespace

// ===========================================================================
// Suite 5 — Underflow / boundary walking
// ===========================================================================

TEST_F(EdgeTest, MarketSellExceedsAllBidLiquidityTerminatesCleanly)
{
	restOrMatch(book, limitOrder(2001, 75, 50, /*buy=*/true));

	auto msg = marketOrder(3001, -1000, /*buy=*/false);
	expectResponse(marketInto(book, msg), Result::PartiallyFilled, 3001, -925); // only 75 available

	expectBest(book, kUnset, /*bid=*/kUnset); // bid side emptied -> sentinel reset
	expectCounts(book, 0, 0);
	expectEmptyLevel(book, 50);
	expectFulfilled(book, {{2001, 0, Result::Filled}});
	EXPECT_EQ(msg.order.quantityLots, -925); // unfilled remainder preserved
	expectBookConsistent(book);
}

TEST_F(EdgeTest, MarketSellWalksDownAcrossEmptyLevelsWithoutUnderflow)
{
	restOrMatch(book, limitOrder(2001, 10, 50, true)); // bid 50
	restOrMatch(book, limitOrder(2002, 10, 48, true)); // bid 48 (49 empty)

	auto msg = marketOrder(3001, -1000, /*buy=*/false);
	expectResponse(marketInto(book, msg), Result::PartiallyFilled, 3001, -980); // sweeps both, runs out

	// best bid walked 50 -> 49 (empty) -> 48, consumed both, reset to unset
	// (must NOT underflow past 0 / wrap while liquidity remained)
	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
	expectFulfilled(book, {{2001, 0, Result::Filled}, {2002, 0, Result::Filled}});
	EXPECT_EQ(msg.order.quantityLots, -980);
	expectBookConsistent(book);
}

TEST_F(EdgeTest, MarketSellAtTickZeroFillsWithoutWrapping)
{
	restOrMatch(book, limitOrder(2001, 10, 0, true)); // bid at the lowest possible tick

	expectResponse(marketInto(book, marketOrder(3001, -10, false)), Result::Filled, 3001, 0);

	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
	expectFulfilled(book, {{2001, 0, Result::Filled}});
	expectBookConsistent(book);
}

TEST_F(EdgeTest, LimitCrossStopsAtItsOwnPrice)
{
	restOrMatch(book, limitOrder(1001, -10, 52, false));
	restOrMatch(book, limitOrder(1002, -10, 55, false)); // above the incoming limit

	// buy limited to 53: fills the 52 ask, must NOT reach the 55 ask; the
	// 10-lot remainder rests at 53
	expectResponse(restOrMatch(book, limitOrder(2001, 20, 53, true)), Result::Resting, 2001, 10);

	expectCounts(book, 1, 1);
	expectLevel(book, 55, {{1002, -10}});
	expectLevel(book, 53, {{2001, 10}});
	expectFulfilled(book, {{1001, 0, Result::Filled}});
	expectBookConsistent(book);
}

// Disabled TODO-driver / bug report. The limit-rest path indexes
// orders[priceTicksLimit] unmasked (HandleLimitOrder) while the market path
// masks bestIdx, so a tick >= kPriceLevelCount writes out of bounds today.
// Enable once large ticks are masked or rejected on ingest.
TEST_F(EdgeTest, DISABLED_BigTickRejected)
{
	EXPECT_ANY_THROW(restOrMatch(book, limitOrder(7001, -10, matcher::kPriceLevelCount, false)));
}

// ===========================================================================
// Suite 6 — Unlikely cases
// ===========================================================================

// QUIRK: a zero-quantity market order never enters the fill loop
// (quantityLots * direction == 0) so it consumes nothing, but the handler
// classifies it as PartiallyFilled (unfilled=0, rejected=1 indexes the middle
// of the result array) rather than Rejected. matchAllOrders guards against
// this by rejecting zero-qty requests before they reach the handler; this
// test documents the raw handler behaviour.
TEST_F(EdgeTest, ZeroQuantityMarketOrderConsumesNothing)
{
	restOrMatch(book, limitOrder(1001, -50, 52, false));

	expectResponse(marketInto(book, marketOrder(3000, 0, /*buy=*/true)), Result::PartiallyFilled, 3000, 0);

	// resting ask untouched, nothing recorded
	expectBest(book, /*ask=*/52, /*bid=*/kUnset);
	expectCounts(book, 1, 0);
	expectLevel(book, 52, {{1001, -50}});
	expectFulfilled(book, {});
	expectBookConsistent(book);
}

// A price level flips sell -> resting-buy -> sell again, all sharing one
// intrusive list. Exercises unlink/append correctness across side flips.
TEST_F(EdgeTest, AlternatingSidesAtSameLevel)
{
	restOrMatch(book, limitOrder(1001, -100, 50, false)); // sell rests
	restOrMatch(book, limitOrder(2002, 150, 50, true));   // buys 100, 50 buy rests at 50
	// sell hits the resting buy
	expectResponse(restOrMatch(book, limitOrder(3003, -30, 50, false)), Result::Filled, 3003, 0);

	expectBest(book, /*ask=*/kUnset, /*bid=*/50);
	expectCounts(book, 0, 1);
	expectLevel(book, 50, {{2002, 20}});
	expectFulfilled(book, {{1001, 0, Result::Filled}, {2002, 20, Result::PartiallyFilled}});
	expectBookConsistent(book);
}

// ===========================================================================
// Suite 7 — Ring wrap and node-pool churn at scale
// ===========================================================================

// Drive more than kFulfilledOrdersCount commits to verify the fulfilled ring
// keeps a monotonic write index and wraps via the mask without corruption,
// and that rest/fill node recycling holds up under churn at a single level.
TEST_F(EdgeTest, FulfilledRingWrapsAcrossRepeatedCrosses)
{
	const std::size_t crosses = 2 * matcher::kFulfilledOrdersCount + 5; // wraps the ring twice
	const std::size_t tick = 60;
	std::size_t lastResting = 0;
	for (std::size_t i = 0; i < crosses; ++i)
	{
		lastResting = 40'000 + i;
		restOrMatch(book, limitOrder(lastResting, -10, tick, false)); // sell rests
		// buy fully consumes it
		ASSERT_EQ(restOrMatch(book, limitOrder(80'000 + i, 10, tick, true)).result, Result::Filled)
			<< "cross " << i << " should fully fill";
	}

	EXPECT_EQ(book.filledWriteIdx, crosses); // one ring entry per consumed resting; monotonic, not wrapped
	const std::size_t mask = book.fulfilled.size() - 1;
	const matcher::MessageResponse& last = book.fulfilled[(book.filledWriteIdx - 1) & mask];
	ASSERT_TRUE(last.oOrder.has_value());
	EXPECT_EQ(last.oOrder->id, lastResting);
	EXPECT_EQ(last.result, Result::Filled);

	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
	EXPECT_TRUE(book.idToOrder.empty());
	expectBookConsistent(book);
}

// A single level holds far more simultaneous orders than the old per-level
// ring capacity ever allowed; one market order then sweeps them all. Also
// pushes filledWriteIdx past the ring size within ONE handler call — see the
// fulfilled-ring overrun flag in the review notes: entries written here
// overwrite undrained ones, but the book itself must stay correct.
TEST_F(EdgeTest, MarketSweepOfDeepLevelClearsBook)
{
	const std::size_t depth = matcher::kFulfilledOrdersCount + 100;
	const std::size_t tick = 55;
	for (std::size_t i = 0; i < depth; ++i)
	{
		restOrMatch(book, limitOrder(10'000 + i, -1, tick, false));
	}
	expectCounts(book, depth, 0);

	expectResponse(
		marketInto(book, marketOrder(90'000, static_cast<std::int64_t>(depth), /*buy=*/true)),
		Result::Filled, 90'000, 0);

	EXPECT_EQ(book.filledWriteIdx, depth);
	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
	expectEmptyLevel(book, tick);
	EXPECT_TRUE(book.idToOrder.empty());
	expectBookConsistent(book);
}
