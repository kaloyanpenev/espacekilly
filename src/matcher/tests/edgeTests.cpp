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

// Rest `n` non-crossing sells at `tick` (no bids exist, so none of them match).
void fillLevelWithSells(OrderBook& book, std::size_t tick, std::size_t n)
{
	for (std::size_t i = 0; i < n; ++i)
	{
		restOrMatch(book, limitOrder(/*id=*/10'000 + i, /*qty=*/-1, tick, /*buy=*/false));
	}
}
} // namespace

// ===========================================================================
// Suite 5 — Full ring buffer (TODO: should throw; currently warns + wraps)
// ===========================================================================

// Disabled TODO-driver. The per-level write currently masks with
// & (kOrdersPerTick-1) and only println()s when the level is full
// (matcher.cpp:218), silently overwriting slot 0 on the (capacity)th order.
// Enable once HandleLimitOrder throws instead of wrapping.
TEST_F(EdgeTest, FullLevelThrows)
{
	fillLevelWithSells(book, /*tick=*/600, matcher::kOrdersPerTick - 1);
	EXPECT_THROW(restOrMatch(book, limitOrder(99'999, -1, 600, false)), std::exception);
}

// ===========================================================================
// Suite 6 — Underflow / overflow
// ===========================================================================

TEST_F(EdgeTest, MarketSellExceedsAllBidLiquidityTerminatesCleanly)
{
	restOrMatch(book, limitOrder(2001, 75, 500, /*buy=*/true));

	auto msg = marketOrder(3001, -1000, /*buy=*/false);
	EXPECT_FALSE(marketInto(book, msg)); // only 75 available

	expectBest(book, kUnset, /*bid=*/kUnset); // bid side emptied -> sentinel reset
	expectCounts(book, 0, 0);
	expectEmptyLevel(book, 500);
	expectFulfilled(book, {2001});             // resting bid consumed, incoming not committed
	EXPECT_EQ(msg.order.quantityLots, -925);   // unfilled remainder preserved
}

TEST_F(EdgeTest, MarketSellWalksDownAcrossEmptyLevelsWithoutUnderflow)
{
	restOrMatch(book, limitOrder(2001, 10, 500, true)); // bid 500
	restOrMatch(book, limitOrder(2002, 10, 498, true)); // bid 498 (499 empty)

	auto msg = marketOrder(3001, -1000, /*buy=*/false);
	EXPECT_FALSE(marketInto(book, msg)); // sweeps both bids, then runs out

	// best bid walked 500 -> 499 (empty) -> 498, consumed both, reset to sentinel
	// (must NOT underflow past 0 / wrap while liquidity remained)
	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
	expectFulfilled(book, {2001, 2002});
	EXPECT_EQ(msg.order.quantityLots, -980);
}

// Disabled TODO-driver. The limit-rest path indexes orders[priceTicksLimit]
// without masking (matcher.cpp:216) while the market path masks
// (matcher.cpp:123), so a tick >= kPriceLevelCount is out of bounds today.
// Enable once large ticks are masked or rejected.
TEST_F(EdgeTest, DISABLED_BigTickRejected)
{
	EXPECT_ANY_THROW(restOrMatch(book, limitOrder(7001, -10, matcher::kPriceLevelCount, false)));
}

// ===========================================================================
// Suite 7 — Unlikely cases
// ===========================================================================

// A zero-quantity market order never enters the fill loop (quantityLots*dir == 0),
// so it consumes nothing but is currently reported as "filled" and recorded.
TEST_F(EdgeTest, ZeroQuantityMarketOrderConsumesNothing)
{
	restOrMatch(book, limitOrder(1001, -50, 502, false));

	EXPECT_TRUE(marketInto(book, marketOrder(3000, 0, /*buy=*/true)));

	// resting ask untouched
	expectBest(book, /*ask=*/502, /*bid=*/kUnset);
	expectCounts(book, 1, 0);
	expectLevel(book, 502, 0, 1, {{1001, -50}});
	expectFulfilled(book, {3000}); // zero-qty order recorded as filled (quirk)
}

// A price level flips sell -> resting-buy -> sell again, all sharing one queue.
// Exercises the readIndex advance invariant across side flips.
TEST_F(EdgeTest, AlternatingSidesAtSameLevel)
{
	restOrMatch(book, limitOrder(1001, -100, 500, false)); // sell rests
	restOrMatch(book, limitOrder(2002, 150, 500, true));   // buys 100, 50 buy rests at 500
	EXPECT_TRUE(restOrMatch(book, limitOrder(3003, -30, 500, false))); // sell hits resting buy

	expectBest(book, /*ask=*/kUnset, /*bid=*/500);
	expectCounts(book, 0, 1);
	// queue: [0]=consumed sell, [1]=buy now 20 left; read still past the sell
	expectLevel(book, 500, /*read=*/1, /*write=*/2, {{2002, 20}});
	expectFulfilled(book, {1001, 3003});
}

// Drive more than kFulfilledOrdersCount commits to verify the fulfilled ring
// keeps a monotonic write index and wraps via the mask without corruption.
// Each cross uses its own tick so no single price level accumulates >2 orders
// (which would hit the unmasked-read bug exercised by the DISABLED test below).
TEST_F(EdgeTest, FulfilledRingWrapsAround)
{
	const std::size_t crosses = matcher::kFulfilledOrdersCount; // 2 commits each -> 2x capacity
	std::size_t lastResting = 0, lastIncoming = 0;
	for (std::size_t i = 0; i < crosses; ++i)
	{
		const std::size_t tick = 700 + i; // distinct level per cross, stays < kPriceLevelCount
		lastResting = 40'000 + i;
		lastIncoming = 80'000 + i;
		restOrMatch(book, limitOrder(lastResting, -10, tick, false)); // sell rests
		ASSERT_TRUE(restOrMatch(book, limitOrder(lastIncoming, 10, tick, true))); // buy fully consumes it
	}

	const std::size_t mask = book.fulfilled.size() - 1;
	EXPECT_EQ(book.filledWriteIdx, 2 * crosses); // monotonic, not wrapped
	// last two committed entries: resting then incoming of the final cross
	EXPECT_EQ(book.fulfilled[(book.filledWriteIdx - 2) & mask], lastResting);
	EXPECT_EQ(book.fulfilled[(book.filledWriteIdx - 1) & mask], lastIncoming);
	// every cross flattened its own level
	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
}

// Disabled TODO-driver / bug report. A single price level cycled past
// kOrdersPerTick cumulative orders breaks: HandleLimitOrder masks the WRITE
// index (matcher.cpp:217) but HandleMarketOrder reads orders[readIndex]
// UNMASKED (matcher.cpp:139). Once readIndex reaches kOrdersPerTick the match
// reads/writes out of bounds, so the (capacity+1)-th cross fails to fill and the
// incoming buy wrongly rests. Repeatedly crossing one level should keep working.
// Enable once the read index is masked the same way as the write index.
TEST_F(EdgeTest, LevelCyclesPastCapacity)
{
	const std::size_t tick = 700;
	for (std::size_t i = 0; i < matcher::kOrdersPerTick + 4; ++i)
	{
		restOrMatch(book, limitOrder(40'000 + i, -10, tick, false));
		EXPECT_TRUE(restOrMatch(book, limitOrder(80'000 + i, 10, tick, true)))
			<< "cross " << i << " should fully fill";
	}
	expectBest(book, kUnset, kUnset);
	expectCounts(book, 0, 0);
}
