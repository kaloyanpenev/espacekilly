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



int main()
{
	std::mt19937 gen(75); // same seed as matcher.cpp, so the stream is comparable

	// MFD_HUGETLB puts the file in hugetlbfs, drawing from the vm.nr_hugepages
	// pool rather than from transparent hugepages. Reservation happens at mmap
	// time, so an undersized pool fails loudly instead of SIGBUS-ing later.
	// MFD_CLOEXEC only affects exec(), which we never call; it costs nothing and
	// keeps the descriptor from leaking if that ever changes.
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

	// MAP_HUGETLB is not repeated here: the hugeness comes from the file being in
	// hugetlbfs. MAP_POPULATE prefaults every page now, so the consumer's timed
	// loop never pays a fault. The returned address is guaranteed huge-page
	// aligned, since a huge page is mapped by a single PMD entry.
	void* region = mmap(nullptr, allocSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
	if (region == MAP_FAILED)
	{
		std::perror("mmap"); // ENOMEM here means the hugepage pool is too small
		return 1;
	}
	NewRequest* orders = static_cast<NewRequest*>(region);

	std::println("attach with: ./exchange /proc/{}/fd/{}   ({} requests, {} bytes)", getpid(), fd, arrSize, allocSize);
	(void)std::fflush(stdout); // the reader is a human in another terminal; do not sit in the buffer

	size_t msgId = 1;

	std::uniform_int_distribution<int64_t> distribLots(500, 20000);
	std::uniform_int_distribution<size_t> distribTicks(40, 60);
	std::uniform_int_distribution<int32_t> distribType(0, 3);
	std::uniform_int_distribution<int32_t> cancelDistrib(0, 1);

	size_t orderCount = 0;

	for (size_t i = 0ul; i < arrSize; ++i)
	{
		int8_t ordTypeNum = static_cast<int8_t>(distribType(gen));
		OrderType ordType = static_cast<OrderType>(ordTypeNum);

		int64_t sign = (ordTypeNum < 2) ? 1 : -1; // -1 if selling
		int64_t lots = distribLots(gen) * sign;

		size_t ticks = distribTicks(gen);
		size_t ordIsLimit = (ordType == OrderType::BuyLimit || ordType == OrderType::SellLimit);
		size_t orderId = msgId++;

		// Construct the *variant*, not the alternative. Placement-newing a bare
		// NewOrderRequest here would leave the discriminant at whatever the page
		// held (zero), so every cancel would silently read back as an order.
		new (orders + i) NewRequest{NewOrderRequest{.msgId = orderId,
			.order = {.id = orderId, .quantityLots = lots},
			.orderType = ordType,
			.instrumentId = Instrument::Time,
			.priceTicksLimit = ordIsLimit * ticks}};
		++orderCount;

		if (static_cast<bool>(cancelDistrib(gen)))
		{
			// a cancel drawn on the final slot is dropped, so the array stays
			// exactly arrSize long with no gaps and needs no sentinel.
			if (++i < arrSize)
			{
				new (orders + i) NewRequest{CancelOrderRequest{
					.msgId = msgId++, .toCancel = orderId, .instrumentId = Instrument::Time}};
			}
		}
	}

	std::println("generated {} requests: {} orders, {} cancels", arrSize, orderCount, arrSize - orderCount);
	(void)std::fflush(stdout);

	// The memfd dies with its last reference, and /proc/<pid>/fd/<fd> only exists
	// while this descriptor is open, so block here rather than exiting.
	::pause();

	return 0;
}