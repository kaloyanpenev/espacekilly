#include <gtest/gtest.h>

#include <matcher/matcher.h>

#include "test_helpers.h"

using namespace th;
using matcher::Order;
using matcher::OrderBook;

namespace
{
// Fresh order book per test. Each construction allocates the book's arena, so
// we keep it inside a fixture rather than copying it around (OrderBook is
// neither copyable nor movable).
struct MatcherTest : ::testing::Test
{
	OrderBook book;
};
} // namespace

// ===========================================================================
// Suite 1 — Resting orders (no cross)
// ===========================================================================

TEST_F(MatcherTest, SingleSellRests)
{
	expectResponse(restOrMatch(book, limitOrder(1001, -100, 52, /*buy=*/false)), Result::Resting, 1001, -100);

	expectBest(book, /*ask=*/52, /*bid=*/kUnset);
	expectCounts(book, /*ask=*/1, /*bid=*/0);
	expectLevel(book, 52, {{1001, -100}});
	expectFulfilled(book, {});
	expectBookConsistent(book);
}

TEST_F(MatcherTest, SingleBuyRests)
{
	expectResponse(restOrMatch(book, limitOrder(2001, 75, 50, /*buy=*/true)), Result::Resting, 2001, 75);

	expectBest(book, /*ask=*/kUnset, /*bid=*/50);
	expectCounts(book, /*ask=*/0, /*bid=*/1);
	expectLevel(book, 50, {{2001, 75}});
	expectFulfilled(book, {});
	expectBookConsistent(book);
}

TEST_F(MatcherTest, BestAskTracksLowestSell)
{
	restOrMatch(book, limitOrder(1001, -100, 52, false));
	restOrMatch(book, limitOrder(1002, -50, 53, false)); // higher tick: best ask stays 52
	expectBest(book, 52, kUnset);

	restOrMatch(book, limitOrder(1003, -25, 51, false)); // lower tick: best ask moves down
	expectBest(book, 51, kUnset);
	expectCounts(book, 3, 0);
	expectBookConsistent(book);
}

TEST_F(MatcherTest, BestBidTracksHighestBuy)
{
	restOrMatch(book, limitOrder(2001, 75, 50, true));
	restOrMatch(book, limitOrder(2002, 75, 49, true)); // lower tick: best bid stays 50
	expectBest(book, kUnset, 50);

	restOrMatch(book, limitOrder(2003, 75, 51, true)); // higher tick: best bid moves up
	expectBest(book, kUnset, 51);
	expectCounts(book, 0, 3);
	expectBookConsistent(book);
}

TEST_F(MatcherTest, NonCrossingBookKeepsBothSides)
{
	restOrMatch(book, limitOrder(2001, 75, 50, true));    // bid 50
	restOrMatch(book, limitOrder(1001, -100, 52, false)); // ask 52, does not cross

	expectBest(book, /*ask=*/52, /*bid=*/50);
	expectCounts(book, 1, 1);
	expectLevel(book, 50, {{2001, 75}});
	expectLevel(book, 52, {{1001, -100}});
	expectFulfilled(book, {});
	expectBookConsistent(book);
}

TEST_F(MatcherTest, FifoWithinLevelPreservesTimePriority)
{
	restOrMatch(book, limitOrder(1001, -40, 52, false)); // first in
	restOrMatch(book, limitOrder(1002, -60, 52, false)); // second in, same tick
	expectLevel(book, 52, {{1001, -40}, {1002, -60}});

	// buy exactly the size of the first order: only 1001 is consumed
	expectResponse(marketInto(book, marketOrder(3001, 40, /*buy=*/true)), Result::Filled, 3001, 0);

	expectCounts(book, 1, 0);
	expectLevel(book, 52, {{1002, -60}});
	expectFulfilled(book, {{1001, 0, Result::Filled}});
	expectBookConsistent(book);
}

// ===========================================================================
// Suite 2 — Crossing orders, exact / fully consumed
// ===========================================================================

TEST_F(MatcherTest, BuyFullyConsumesRestingSell)
{
	restOrMatch(book, limitOrder(1001, -100, 52, false)); // ask 52
	// exact cross: incoming fully filled, qty consumed to zero
	expectResponse(restOrMatch(book, limitOrder(2002, 100, 52, true)), Result::Filled, 2002, 0);

	expectBest(book, /*ask=*/kUnset, /*bid=*/kUnset); // ask side emptied -> reset
	expectCounts(book, 0, 0);                         // incoming did not rest
	expectEmptyLevel(book, 52);
	expectFulfilled(book, {{1001, 0, Result::Filled}});
	expectBookConsistent(book);
}

TEST_F(MatcherTest, SmallBuyLeavesRemainderInRestingSell)
{
	restOrMatch(book, limitOrder(1001, -100, 52, false));
	// smaller than resting: incoming fully filled, resting keeps -70
	expectResponse(restOrMatch(book, limitOrder(2002, 30, 52, true)), Result::Filled, 2002, 0);

	expectBest(book, /*ask=*/52, /*bid=*/kUnset); // resting still on the book
	expectCounts(book, 1, 0);
	expectLevel(book, 52, {{1001, -70}});
	expectFulfilled(book, {{1001, -70, Result::PartiallyFilled}});
	expectBookConsistent(book);
}

TEST_F(MatcherTest, SellFullyConsumesRestingBuy)
{
	restOrMatch(book, limitOrder(2001, 100, 50, true)); // bid 50
	expectResponse(restOrMatch(book, limitOrder(1001, -100, 50, false)), Result::Filled, 1001, 0);

	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
	expectEmptyLevel(book, 50);
	expectFulfilled(book, {{2001, 0, Result::Filled}});
	expectBookConsistent(book);
}

TEST_F(MatcherTest, SmallSellLeavesRemainderInRestingBuy)
{
	restOrMatch(book, limitOrder(2001, 100, 50, true));
	expectResponse(restOrMatch(book, limitOrder(1001, -30, 50, false)), Result::Filled, 1001, 0);

	expectBest(book, kUnset, 50);
	expectCounts(book, 0, 1);
	expectLevel(book, 50, {{2001, 70}});
	expectFulfilled(book, {{2001, 70, Result::PartiallyFilled}});
	expectBookConsistent(book);
}

TEST_F(MatcherTest, CrossWalksEmptyGapBetweenLevels)
{
	restOrMatch(book, limitOrder(1001, -100, 52, false)); // ask 52
	restOrMatch(book, limitOrder(1002, -50, 54, false));  // ask 54 (53 empty)

	// sweeps 52, walks the empty 53, takes 20 from 54
	expectResponse(restOrMatch(book, limitOrder(2002, 120, 54, true)), Result::Filled, 2002, 0);

	expectBest(book, /*ask=*/54, /*bid=*/kUnset);
	expectCounts(book, 1, 0);
	expectEmptyLevel(book, 52);
	expectLevel(book, 54, {{1002, -30}});
	expectFulfilled(book, {{1001, 0, Result::Filled}, {1002, -30, Result::PartiallyFilled}});
	expectBookConsistent(book);
}

// ===========================================================================
// Suite 3 — Crossing with remainder that rests
// ===========================================================================

TEST_F(MatcherTest, BuyRemainderRestsInSameLevel)
{
	restOrMatch(book, limitOrder(1001, -100, 52, false));
	// crosses, fills 100, the 50 remainder rests at 52 (the level the sell was in)
	expectResponse(restOrMatch(book, limitOrder(2002, 150, 52, true)), Result::Resting, 2002, 50);

	expectBest(book, /*ask=*/kUnset, /*bid=*/52);
	expectCounts(book, 0, 1);
	expectLevel(book, 52, {{2002, 50}});
	expectFulfilled(book, {{1001, 0, Result::Filled}});
	expectBookConsistent(book);
}

TEST_F(MatcherTest, BuyRemainderRestsAtFreshLevel)
{
	restOrMatch(book, limitOrder(1001, -50, 52, false)); // ask 52
	// fills 50, the 40 remainder rests at 55
	expectResponse(restOrMatch(book, limitOrder(2002, 90, 55, true)), Result::Resting, 2002, 40);

	expectBest(book, /*ask=*/kUnset, /*bid=*/55);
	expectCounts(book, 0, 1);
	expectEmptyLevel(book, 52);
	expectLevel(book, 55, {{2002, 40}});
	expectFulfilled(book, {{1001, 0, Result::Filled}});
	expectBookConsistent(book);
}

TEST_F(MatcherTest, SellRemainderRestsInSameLevel)
{
	restOrMatch(book, limitOrder(2001, 100, 50, true));
	expectResponse(restOrMatch(book, limitOrder(1001, -150, 50, false)), Result::Resting, 1001, -50);

	expectBest(book, /*ask=*/50, /*bid=*/kUnset);
	expectCounts(book, 1, 0);
	expectLevel(book, 50, {{1001, -50}});
	expectFulfilled(book, {{2001, 0, Result::Filled}});
	expectBookConsistent(book);
}

// ===========================================================================
// Suite 4 — Market orders
// ===========================================================================

TEST_F(MatcherTest, MarketSellFillsRestingBid)
{
	restOrMatch(book, limitOrder(2001, 75, 50, true)); // bid 50
	expectResponse(marketInto(book, marketOrder(3001, -40, /*buy=*/false)), Result::Filled, 3001, 0);

	expectBest(book, kUnset, 50);
	expectCounts(book, 0, 1);
	expectLevel(book, 50, {{2001, 35}}); // 75 - 40
	expectFulfilled(book, {{2001, 35, Result::PartiallyFilled}});
	expectBookConsistent(book);
}

TEST_F(MatcherTest, MarketBuyOnEmptyBookRejected)
{
	auto msg = marketOrder(3002, 30, /*buy=*/true);
	expectResponse(marketInto(book, msg), Result::Rejected, 3002, 30);

	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
	expectFulfilled(book, {});
	EXPECT_EQ(msg.order.quantityLots, 30); // unfilled quantity preserved
	expectBookConsistent(book);
}

TEST_F(MatcherTest, MarketBuyPartialThenExhaustsBook)
{
	restOrMatch(book, limitOrder(1001, -50, 52, false)); // only 50 available
	auto msg = marketOrder(3002, 80, /*buy=*/true);
	expectResponse(marketInto(book, msg), Result::PartiallyFilled, 3002, 30); // fills 50, 30 left

	expectBest(book, kUnset, kUnset); // ask side emptied -> reset
	expectCounts(book, 0, 0);
	expectEmptyLevel(book, 52);
	expectFulfilled(book, {{1001, 0, Result::Filled}});
	EXPECT_EQ(msg.order.quantityLots, 30); // unfilled remainder preserved in the message
	expectBookConsistent(book);
}

TEST_F(MatcherTest, MarketBuySweepsMultipleLevels)
{
	restOrMatch(book, limitOrder(1001, -40, 52, false));
	restOrMatch(book, limitOrder(1002, -40, 53, false));
	restOrMatch(book, limitOrder(1003, -40, 54, false));

	// 100 buys: 40 @52, 40 @53, 20 @54 (20 stays resting at 54)
	expectResponse(marketInto(book, marketOrder(3003, 100, /*buy=*/true)), Result::Filled, 3003, 0);

	expectBest(book, /*ask=*/54, /*bid=*/kUnset);
	expectCounts(book, 1, 0);
	expectEmptyLevel(book, 52);
	expectEmptyLevel(book, 53);
	expectLevel(book, 54, {{1003, -20}});
	expectFulfilled(book,
		{{1001, 0, Result::Filled}, {1002, 0, Result::Filled}, {1003, -20, Result::PartiallyFilled}});
	expectBookConsistent(book);
}
