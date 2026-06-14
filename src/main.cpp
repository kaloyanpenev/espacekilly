#include <matcher/dro/spsc-queue.hpp>
#include <matcher/matcher.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <print>
#include <ranges>
#include <thread>

struct sadstr
{
	sadstr() noexcept { arr.fill(512UL); }

	std::array<size_t, 32> arr;
};

constexpr size_t ops = 2000UL;
using Queue = dro::SPSCQueue<sadstr, ops>;

void writerWorker(Queue& fif)
{
	for ([[maybe_unused]] const auto i : std::ranges::views::iota(0UL, ops))
	{
		fif.emplace();
	}
};

void readerWorker(Queue& fif)
{
	sadstr s{};
	size_t opcount = ops;
	while (opcount)
	{
		if (fif.try_pop(s))
		{
			opcount--;
		}
	}
};

int main()
{
	matcher::startMatch();
//	try
//	{
//		auto spscFifo = Queue();
//		const auto now = std::chrono::steady_clock::now();
//		std::jthread writer{writerWorker, std::ref(spscFifo)};
//		std::this_thread::sleep_for(std::chrono::microseconds(1));
//		std::jthread reader{readerWorker, std::ref(spscFifo)};
//		writer.join();
//		reader.join();
//		const auto done = std::chrono::steady_clock::now();
//
//		std::println("end! duration: {}", std::chrono::duration_cast<std::chrono::microseconds>(done - now).count());
//	}
//	catch (const std::exception& e)
//	{
//		std::cerr << "Fatal: " << e.what() << '\n';
//		return 1;
//	}
//	catch (...)
//	{
//		std::cerr << "Fatal: unknown exception\n";
//		return 1;
//	}

	return 0;
}