// market_generator: builds the entire market directly inside a hugepage-backed
// memfd, then blocks so ./exchange can attach and drain it.
//
// This is CreateMarket() from matcher.cpp with three changes:
//   1. std::list -> a flat array constructed in place in the mapping, so the
//      requests live in one contiguous run of bytes another process can map.
//   2. no SPSCQueue. There is no concurrency here: the generator finishes
//      completely before the consumer is started, so the handoff needs no
//      synchronisation at all.
//   3. the loop is bounded on slots rather than orders, so it emits exactly
//      arrSize requests. The order/cancel split therefore differs slightly
//      from matcher.cpp's, whose bound was on orders.
//
// In the list version the cancel pass inserted each CancelOrderRequest *after*
// the current iterator and set skip=true so the next advance stepped over the
// freshly inserted node without drawing. Every order therefore consumed exactly
// one cancelDistrib draw, which is what the single pass below does.
//
// The mapping is anonymous: it has no name in any filesystem. ./exchange reaches
// it by reopening /proc/<pid>/fd/<fd>, which is why this process must stay alive
// with the descriptor open.

#include <matcher/matcher.h>

#include <cstdio>
#include <print>
#include <random>
#include <sys/mman.h>
#include <unistd.h>

using namespace matcher;

namespace
{

std::bernoulli_distribution isBuyDistrib(0.5);

std::geometric_distribution<int> depthDistrib(0.5); // 0 half the time, 1 1/4 of the time...
std::bernoulli_distribution crossingOrderDistrib(0.03); // 3%
std::uniform_int_distribution<int> driftStepDistrib(-1, 1); // move spread
std::geometric_distribution<size_t> cancelAgeDistrib(0.05);

std::discrete_distribution<size_t> lotsDistrib({30, 25, 20, 10,  7,  5,  2,   1});
constexpr std::array<int64_t, 8>   lotSizes       { 1,  2,  5, 10, 20, 50, 80, 100};


size_t mid = 64;

size_t makePrice(bool isBuy, bool crossing, std::mt19937& gen)
{

	// distance from midpoint, geometric
	const int depth = depthDistrib(gen);

	// passive bid and crossing asks are below the mid: mid - 1 - d;
	// passive ask and crossing bids are above the mid: mid + 1 + d;

	const bool aboveMid = isBuy == crossing;
	const int price = aboveMid ? mid + 1 + depth : mid - 1 - depth;

	return static_cast<size_t>(std::clamp<int64_t>(price, 0, kPriceLevelCount - 1));
}

void driftMidpoint(std::mt19937& gen)
{
	mid += driftStepDistrib(gen);
	// revert towards mean if we start going off the wall
	if (mid > 90) { mid -= 1; };
	if (mid < 37) { mid += 1; };
}
}


int main(int argc, char* argv[])
{
	int seed = std::stoi(std::string(argv[1]));
	std::mt19937 gen(seed);

	int fd = memfd_create("market", MFD_HUGETLB | MFD_CLOEXEC);
	if (fd == -1)
	{
		std::perror("memfd_create");
		return 1;
	}

	// hugetlbfs requires this length to be a whole number of huge pages.
	if (ftruncate(fd, allocSize) == -1)
	{
		std::perror("ftruncate");
		return 1;
	}

	void* region = mmap(nullptr, allocSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
	if (region == MAP_FAILED)
	{
		std::perror("mmap"); // ENOMEM here means the hugepage pool is too small
		return 1;
	}
	NewRequest* orders_p = static_cast<NewRequest*>(region);

	std::println("attach with: ./exchange /proc/{}/fd/{}   ({} requests, {} bytes)", getpid(), fd, arrSize, allocSize);


	enum Action : int { AddLimit = 0, Cancel = 1, Market = 2 };
	// 50% limits, 44% cancels, 6% markets
	std::discrete_distribution<int> action({50, 44, 6});
	size_t msgId = 1;

	// what was emitted
	size_t limitBids = 0, limitAsks = 0, crossingLimits = 0;
	size_t marketBuys = 0, marketSells = 0;
	size_t cancels = 0, cancelsForcedToLimit = 0;
	int64_t limitLotsTotal = 0, marketLotsTotal = 0;

	// the generator's own picture of what rests: [side][price] -> order count,
	// side 0 = ask, 1 = bid (same layout as OrderBook::counts). fills are not
	// modelled here, so this is an upper bound on what the exchange really holds.
	std::array<std::array<size_t, kPriceLevelCount>, 2> restingModel{};

	struct LiveOrder
	{
		OrderID id;
		size_t price;
		bool isBuy;
	};
	std::vector<LiveOrder> live{};
	size_t maxLive = 0;

	for (size_t i = 0ul; i < arrSize; ++i)
	{
		static constexpr int driftN = 500;
		if (i % driftN == 0)
		{
			driftMidpoint(gen);
		}

		Action act = static_cast<Action>(action(gen));

		if (act == Cancel && live.empty())
		{
			act = AddLimit;
			++cancelsForcedToLimit;
		}

		if (act == Cancel)
		{
			const size_t k = std::min(live.size() - 1, cancelAgeDistrib(gen));
			const size_t idx = live.size() - 1 - k;
			const LiveOrder victim = live[idx];
			new (orders_p + i) NewRequest{CancelOrderRequest{
				.msgId = msgId++, .toCancel = victim.id, .instrumentId = Instrument::Time}};
			live[idx] = live.back(); //swap with back
			live.pop_back();
			--restingModel[victim.isBuy][victim.price];
			++cancels;
			continue;
		}

		const bool isBuy = isBuyDistrib(gen);
		const bool isLimit = act == AddLimit;
		const int64_t sign = isBuy ? 1 : -1; // -1 if selling

		const int64_t lots = lotSizes[lotsDistrib(gen)];

		const size_t orderId = msgId++;
		OrderType type = isBuy ? (isLimit ? OrderType::BuyLimit : OrderType::Buy) :
								(isLimit ? OrderType::SellLimit : OrderType::Sell);

		const bool crossing = isLimit && crossingOrderDistrib(gen);
		const size_t price = isLimit ? makePrice(isBuy, crossing, gen) : 0;

		new (orders_p + i) NewRequest{NewOrderRequest{.msgId = orderId,
			.order = {
				.id = orderId,
				.quantityLots = sign * lots
			},
			.orderType = type,
			.instrumentId = Instrument::Time,
			.priceTicksLimit = price}};

		if (isLimit)
		{
			live.push_back({orderId, price, isBuy});
			maxLive = std::max(maxLive, live.size());
			++restingModel[isBuy][price];
			(isBuy ? limitBids : limitAsks)++;
			crossingLimits += crossing;
			limitLotsTotal += lots;
		}
		else
		{
			(isBuy ? marketBuys : marketSells)++;
			marketLotsTotal += lots;
		}
	}

	const size_t limits = limitBids + limitAsks;
	const size_t markets = marketBuys + marketSells;
	std::println("generated {} requests:", arrSize);
	std::println("  limits:  {} ({:.1f}%)  bids {}  asks {}  crossing {}  avg lots {:.1f}",
		limits, 100.0 * limits / arrSize, limitBids, limitAsks, crossingLimits,
		static_cast<double>(limitLotsTotal) / limits);
	std::println("  markets: {} ({:.1f}%)  buys {}  sells {}  avg lots {:.1f}",
		markets, 100.0 * markets / arrSize, marketBuys, marketSells,
		static_cast<double>(marketLotsTotal) / markets);
	std::println("  cancels: {} ({:.1f}%)  {} cancel draws turned into limits because the pool was empty",
		cancels, 100.0 * cancels / arrSize, cancelsForcedToLimit);

	// ladder as the generator sees it: bids on the left, asks on the right, high price first.
	size_t modelBids = 0, modelAsks = 0;
	for (size_t p = 0; p < kPriceLevelCount; ++p)
	{
		modelBids += restingModel[1][p];
		modelAsks += restingModel[0][p];
	}
	std::println("generator's book at the end (fills not modelled): mid {}  live pool {} (peak {})  bids {}  asks {}",
		mid, live.size(), maxLive, modelBids, modelAsks);
	std::println("  {:>6} {:>5} {:>6}", "bids", "price", "asks");
	for (size_t p = kPriceLevelCount; p-- > 0;)
	{
		const size_t bids = restingModel[1][p];
		const size_t asks = restingModel[0][p];
		if (bids == 0 && asks == 0)
		{
			continue;
		}
		std::println("  {:>6} {:>5} {:>6}{}", bids, p, asks, p == mid ? "  <- mid" : "");
	}
	(void)std::fflush(stdout);

	::pause();

	return 0;
}