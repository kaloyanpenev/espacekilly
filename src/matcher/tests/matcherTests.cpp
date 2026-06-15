#include <gtest/gtest.h>

#include <matcher/matcher.h>

#include "test_helpers.h"

using namespace th;
using matcher::Order;
using matcher::OrderBook;

namespace
{
// Fresh order book per test. Each construction allocates the book's arena, so we
// keep it on the heap inside a fixture rather than copying it around.
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
	EXPECT_FALSE(restOrMatch(book, limitOrder(1001, -100, 502, /*buy=*/false)));

	expectBest(book, /*ask=*/502, /*bid=*/kUnset);
	expectCounts(book, /*ask=*/1, /*bid=*/0);
	expectLevel(book, 502, /*read=*/0, /*write=*/1, {{1001, -100}});
	expectFulfilled(book, {});
}

TEST_F(MatcherTest, SingleBuyRests)
{
	EXPECT_FALSE(restOrMatch(book, limitOrder(2001, 75, 500, /*buy=*/true)));

	expectBest(book, /*ask=*/kUnset, /*bid=*/500);
	expectCounts(book, /*ask=*/0, /*bid=*/1);
	expectLevel(book, 500, 0, 1, {{2001, 75}});
	expectFulfilled(book, {});
}

TEST_F(MatcherTest, BestAskTracksLowestSell)
{
	restOrMatch(book, limitOrder(1001, -100, 502, false));
	restOrMatch(book, limitOrder(1002, -50, 503, false)); // higher tick: best ask stays 502
	expectBest(book, 502, kUnset);

	restOrMatch(book, limitOrder(1003, -25, 501, false)); // lower tick: best ask moves down
	expectBest(book, 501, kUnset);
	expectCounts(book, 3, 0);
}

TEST_F(MatcherTest, BestBidTracksHighestBuy)
{
	restOrMatch(book, limitOrder(2001, 75, 500, true));
	restOrMatch(book, limitOrder(2002, 75, 499, true)); // lower tick: best bid stays 500
	expectBest(book, kUnset, 500);

	restOrMatch(book, limitOrder(2003, 75, 501, true)); // higher tick: best bid moves up
	expectBest(book, kUnset, 501);
	expectCounts(book, 0, 3);
}

TEST_F(MatcherTest, NonCrossingBookKeepsBothSides)
{
	restOrMatch(book, limitOrder(2001, 75, 500, true));   // bid 500
	restOrMatch(book, limitOrder(1001, -100, 502, false)); // ask 502, does not cross

	expectBest(book, /*ask=*/502, /*bid=*/500);
	expectCounts(book, 1, 1);
	expectLevel(book, 500, 0, 1, {{2001, 75}});
	expectLevel(book, 502, 0, 1, {{1001, -100}});
	expectFulfilled(book, {});
}

// ===========================================================================
// Suite 2 — Crossing orders, exact / fully consumed
// ===========================================================================

TEST_F(MatcherTest, BuyFullyConsumesRestingSell)
{
	restOrMatch(book, limitOrder(1001, -100, 502, false));   // ask 502
	EXPECT_TRUE(restOrMatch(book, limitOrder(2002, 100, 502, true))); // exact cross

	expectBest(book, /*ask=*/kUnset, /*bid=*/kUnset);
	expectCounts(book, 0, 0);            // incoming did not rest
	expectEmptyLevel(book, 502);
	expectFulfilled(book, {1001, 2002}); // resting then incoming
}

TEST_F(MatcherTest, SmallBuyLeavesRemainderInRestingSell)
{
	restOrMatch(book, limitOrder(1001, -100, 502, false));
	EXPECT_TRUE(restOrMatch(book, limitOrder(2002, 30, 502, true))); // smaller than resting

	expectBest(book, /*ask=*/502, /*bid=*/kUnset); // resting still on the book
	expectCounts(book, 1, 0);
	expectLevel(book, 502, 0, 1, {{1001, -70}});   // resting partially filled
	expectFulfilled(book, {2002});                 // only the fully-filled incoming
}

TEST_F(MatcherTest, SellFullyConsumesRestingBuy)
{
	restOrMatch(book, limitOrder(2001, 100, 500, true));         // bid 500
	EXPECT_TRUE(restOrMatch(book, limitOrder(1001, -100, 500, false))); // exact cross

	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
	expectEmptyLevel(book, 500);
	expectFulfilled(book, {2001, 1001});
}

TEST_F(MatcherTest, SmallSellLeavesRemainderInRestingBuy)
{
	restOrMatch(book, limitOrder(2001, 100, 500, true));
	EXPECT_TRUE(restOrMatch(book, limitOrder(1001, -30, 500, false)));

	expectBest(book, kUnset, 500);
	expectCounts(book, 0, 1);
	expectLevel(book, 500, 0, 1, {{2001, 70}});
	expectFulfilled(book, {1001});
}

TEST_F(MatcherTest, CrossWalksEmptyGapBetweenLevels)
{
	restOrMatch(book, limitOrder(1001, -100, 502, false)); // ask 502
	restOrMatch(book, limitOrder(1002, -50, 504, false));  // ask 504 (503 empty)

	EXPECT_TRUE(restOrMatch(book, limitOrder(2002, 120, 504, true))); // sweeps 502, gap 503, into 504

	expectBest(book, /*ask=*/504, /*bid=*/kUnset);
	expectCounts(book, 1, 0);
	expectEmptyLevel(book, 502);
	expectLevel(book, 504, 0, 1, {{1002, -30}}); // 120 - 100 - 30 fills, 20 taken from 504
	expectFulfilled(book, {1001, 2002});
}

// ===========================================================================
// Suite 3 — Crossing with remainder that rests
// ===========================================================================

TEST_F(MatcherTest, BuyRemainderRestsInSameLevel)
{
	restOrMatch(book, limitOrder(1001, -100, 502, false));
	// crosses, fills 100, 50 remainder rests at 502 (the same level the sell was in)
	EXPECT_FALSE(restOrMatch(book, limitOrder(2002, 150, 502, true)));

	expectBest(book, /*ask=*/kUnset, /*bid=*/502);
	expectCounts(book, 0, 1);
	// consumed sell at slot 0, resting buy at slot 1; read advanced past the sell
	expectLevel(book, 502, /*read=*/1, /*write=*/2, {{2002, 50}});
	expectFulfilled(book, {1001}); // incoming not yet fully filled -> not committed
}

TEST_F(MatcherTest, BuyRemainderRestsAtFreshLevel)
{
	restOrMatch(book, limitOrder(1001, -50, 502, false)); // ask 502
	EXPECT_FALSE(restOrMatch(book, limitOrder(2002, 90, 505, true))); // fills 50, 40 rests at 505

	expectBest(book, /*ask=*/kUnset, /*bid=*/505);
	expectCounts(book, 0, 1);
	expectEmptyLevel(book, 502);
	expectLevel(book, 505, 0, 1, {{2002, 40}});
	expectFulfilled(book, {1001});
}

TEST_F(MatcherTest, SellRemainderRestsInSameLevel)
{
	restOrMatch(book, limitOrder(2001, 100, 500, true));
	EXPECT_FALSE(restOrMatch(book, limitOrder(1001, -150, 500, false)));

	expectBest(book, /*ask=*/500, /*bid=*/kUnset);
	expectCounts(book, 1, 0);
	expectLevel(book, 500, 1, 2, {{1001, -50}});
	expectFulfilled(book, {2001});
}

// ===========================================================================
// Suite 4 — Market orders
// ===========================================================================

TEST_F(MatcherTest, MarketSellFillsRestingBid)
{
	restOrMatch(book, limitOrder(2001, 75, 500, true)); // bid 500
	EXPECT_TRUE(marketInto(book, marketOrder(3001, -40, /*buy=*/false)));

	expectBest(book, kUnset, 500);
	expectCounts(book, 0, 1);
	expectLevel(book, 500, 0, 1, {{2001, 35}}); // 75 - 40
	expectFulfilled(book, {3001});
}

TEST_F(MatcherTest, MarketBuyOnEmptyBookFails)
{
	auto msg = marketOrder(3002, 30, /*buy=*/true);
	EXPECT_FALSE(marketInto(book, msg));

	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
	expectFulfilled(book, {});
	EXPECT_EQ(msg.order.quantityLots, 30); // unfilled quantity preserved
}

TEST_F(MatcherTest, MarketBuyPartialThenExhaustsBook)
{
	restOrMatch(book, limitOrder(1001, -50, 502, false)); // only 50 available
	auto msg = marketOrder(3002, 80, /*buy=*/true);
	EXPECT_FALSE(marketInto(book, msg)); // can only fill 50

	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
	expectEmptyLevel(book, 502);
	expectFulfilled(book, {1001});         // resting consumed; incoming not committed
	EXPECT_EQ(msg.order.quantityLots, 30); // 30 left unfilled
}

TEST_F(MatcherTest, MarketBuySweepsMultipleLevels)
{
	restOrMatch(book, limitOrder(1001, -40, 502, false));
	restOrMatch(book, limitOrder(1002, -40, 503, false));
	restOrMatch(book, limitOrder(1003, -40, 504, false));

	// 100 buys: 40 @502, 40 @503, 20 @504 (20 remainder left resting at 504)
	EXPECT_TRUE(marketInto(book, marketOrder(3003, 100, /*buy=*/true)));

	expectBest(book, /*ask=*/504, /*bid=*/kUnset);
	expectCounts(book, 1, 0);
	expectEmptyLevel(book, 502);
	expectEmptyLevel(book, 503);
	expectLevel(book, 504, 0, 1, {{1003, -20}});
	expectFulfilled(book, {1001, 1002, 3003});
}
