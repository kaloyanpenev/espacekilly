#include <gtest/gtest.h>

#include <matcher/matcher.h>

#include "test_helpers.h"

using namespace th;
using matcher::Order;
using matcher::OrderBook;

namespace
{
struct CancelTest : ::testing::Test
{
	OrderBook book;
};
} // namespace

// ===========================================================================
// Suite 8 — Cancellation basics
// ===========================================================================

TEST_F(CancelTest, CancelRestingBuyRemovesIt)
{
	restOrMatch(book, limitOrder(2001, 75, 50, /*buy=*/true));

	expectResponse(cancelOrder(book, 2001), Result::Cancelled, 2001, 75);

	expectCounts(book, 0, 0);
	expectEmptyLevel(book, 50);
	EXPECT_TRUE(book.idToOrder.empty());
	expectFulfilled(book, {}); // cancels never enter the fulfilled ring
	// QUIRK: cancellation does not maintain bestIdx, so the bid best stays
	// pointing at the now-empty level 50. The match paths self-heal (walk /
	// reset on orderCount == 0), see the tests below.
	expectBest(book, kUnset, /*bid=*/50);
	expectBookConsistent(book);
}

TEST_F(CancelTest, CancelRestingSellRemovesIt)
{
	restOrMatch(book, limitOrder(1001, -100, 52, /*buy=*/false));

	expectResponse(cancelOrder(book, 1001), Result::Cancelled, 1001, -100);

	expectCounts(book, 0, 0);
	expectEmptyLevel(book, 52);
	EXPECT_TRUE(book.idToOrder.empty());
	expectBookConsistent(book);
}

TEST_F(CancelTest, CancelUnknownIdNotFound)
{
	expectEmptyResponse(cancelOrder(book, 424242), Result::NotFound);
	expectCounts(book, 0, 0);
	expectBookConsistent(book);
}

TEST_F(CancelTest, CancelTwiceSecondNotFound)
{
	restOrMatch(book, limitOrder(2001, 75, 50, true));

	expectResponse(cancelOrder(book, 2001), Result::Cancelled, 2001, 75);
	expectEmptyResponse(cancelOrder(book, 2001), Result::NotFound);

	expectCounts(book, 0, 0);
	expectBookConsistent(book);
}

TEST_F(CancelTest, CancelDecrementsOnlyItsOwnSide)
{
	restOrMatch(book, limitOrder(1001, -100, 52, false)); // ask
	restOrMatch(book, limitOrder(2001, 75, 50, true));    // bid

	expectResponse(cancelOrder(book, 2001), Result::Cancelled, 2001, 75);

	expectCounts(book, /*ask=*/1, /*bid=*/0);
	expectLevel(book, 52, {{1001, -100}});
	expectBookConsistent(book);
}

// ===========================================================================
// Suite 9 — Cancellation interacting with fills
// ===========================================================================

TEST_F(CancelTest, CancelFullyFilledOrderNotFound)
{
	restOrMatch(book, limitOrder(1001, -100, 52, false));
	restOrMatch(book, limitOrder(2002, 100, 52, true)); // consumes 1001 entirely

	// the fill erased 1001 from idToOrder, so a late cancel misses
	expectEmptyResponse(cancelOrder(book, 1001), Result::NotFound);
	expectCounts(book, 0, 0);
	expectBookConsistent(book);
}

TEST_F(CancelTest, CancelPartiallyFilledOrderReturnsRemainder)
{
	restOrMatch(book, limitOrder(1001, -100, 52, false));
	restOrMatch(book, limitOrder(2002, 30, 52, true)); // resting is now -70

	expectResponse(cancelOrder(book, 1001), Result::Cancelled, 1001, -70);

	expectCounts(book, 0, 0);
	expectEmptyLevel(book, 52);
	expectBookConsistent(book);
}

TEST_F(CancelTest, CancelMiddleOfLevelPreservesFifo)
{
	restOrMatch(book, limitOrder(1001, -10, 52, false));
	restOrMatch(book, limitOrder(1002, -10, 52, false));
	restOrMatch(book, limitOrder(1003, -10, 52, false));

	expectResponse(cancelOrder(book, 1002), Result::Cancelled, 1002, -10);
	expectCounts(book, 2, 0);
	expectLevel(book, 52, {{1001, -10}, {1003, -10}});

	// a sweep now fills 1001 then 1003, in time priority, skipping the hole
	expectResponse(marketInto(book, marketOrder(3001, 20, /*buy=*/true)), Result::Filled, 3001, 0);
	expectFulfilled(book, {{1001, 0, Result::Filled}, {1003, 0, Result::Filled}});
	expectCounts(book, 0, 0);
	expectBookConsistent(book);
}

// ===========================================================================
// Suite 10 — Stale bestIdx after cancellation self-heals
// ===========================================================================

TEST_F(CancelTest, MarketAfterCancelEmptiedSideIsRejectedAndResetsBest)
{
	restOrMatch(book, limitOrder(2001, 75, 50, true));
	cancelOrder(book, 2001); // bid side empty, but bestIdx[1] still 50

	auto msg = marketOrder(3001, -40, /*buy=*/false);
	expectResponse(marketInto(book, msg), Result::Rejected, 3001, -40);

	// the market path noticed orderCount == 0 and reset the stale best
	expectBest(book, kUnset, kUnset);
	expectFulfilled(book, {});
	expectBookConsistent(book);
}

TEST_F(CancelTest, MarketWalksDownFromStaleBestToRealOrder)
{
	restOrMatch(book, limitOrder(2001, 10, 55, true)); // best bid 55
	cancelOrder(book, 2001);                           // bestIdx[1] stays 55 (stale)
	restOrMatch(book, limitOrder(2002, 10, 50, true)); // real best is 50, bestIdx still 55

	expectBest(book, kUnset, 55); // stale, but >= the real best: walking finds 50

	expectResponse(marketInto(book, marketOrder(3001, -5, /*buy=*/false)), Result::Filled, 3001, 0);

	expectBest(book, kUnset, 50); // walk landed on the real level
	expectCounts(book, 0, 1);
	expectLevel(book, 50, {{2002, 5}});
	expectFulfilled(book, {{2002, 5, Result::PartiallyFilled}});
	expectBookConsistent(book);
}

TEST_F(CancelTest, LimitCrossAgainstStaleBestFallsThroughToResting)
{
	restOrMatch(book, limitOrder(2001, 10, 55, true)); // best bid 55
	cancelOrder(book, 2001);                           // bid side empty, bestIdx[1] stale at 55

	// sell at 53 "crosses" the stale best; the match finds nothing and the
	// order rests as a normal ask
	expectResponse(restOrMatch(book, limitOrder(1001, -10, 53, false)), Result::Resting, 1001, -10);

	expectBest(book, /*ask=*/53, /*bid=*/kUnset); // match path reset the stale bid best
	expectCounts(book, 1, 0);
	expectLevel(book, 53, {{1001, -10}});
	expectFulfilled(book, {});
	expectBookConsistent(book);
}

// ===========================================================================
// Suite 11 — Node pool recycling through cancels
// ===========================================================================

TEST_F(CancelTest, CancelledNodeIsReusedByNextRestingOrder)
{
	restOrMatch(book, limitOrder(2001, 75, 50, true));
	matcher::OrderNode* node = book.idToOrder.at(2001);

	cancelOrder(book, 2001);
	restOrMatch(book, limitOrder(2002, 33, 51, true));

	// the free list is LIFO: the freshly released node comes back first
	EXPECT_EQ(book.idToOrder.at(2002), node);
	expectLevel(book, 51, {{2002, 33}});
	expectBookConsistent(book);
}

TEST_F(CancelTest, RepeatedRestCancelChurnKeepsBookConsistent)
{
	for (std::size_t i = 0; i < 1000; ++i)
	{
		const std::size_t id = 10'000 + i;
		const bool buy = (i % 2) == 0;
		const std::size_t tick = 45 + (i % 10);
		restOrMatch(book, limitOrder(id, buy ? 10 : -10, tick, buy));
		expectResponse(cancelOrder(book, id), Result::Cancelled, id, buy ? 10 : -10);
	}
	expectCounts(book, 0, 0);
	EXPECT_TRUE(book.idToOrder.empty());
	expectBookConsistent(book);
}
