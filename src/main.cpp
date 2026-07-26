#include <matcher/dro/spsc-queue.hpp>
#include <matcher/matcher.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <print>

// Pin the calling thread to a single logical CPU.
// Passing pid 0 means "the thread making this call".
void pin_to_cpu(int cpu)
{
	cpu_set_t set;                       // a fixed-size bitmask, one bit per CPU
	CPU_ZERO(&set);                      // clear every bit
	CPU_SET(cpu, &set);                  // set the bit for our target CPU

	// args: pid (0 = self), size of the mask, pointer to the mask
	if (sched_setaffinity(0, sizeof(set), &set) != 0) {
		std::perror("sched_setaffinity");   // prints the errno reason
		std::exit(EXIT_FAILURE);
	}
}

int main(int argc, char** argv)
{
	// market_generator's memfd has no name in any filesystem, so the only handle
	// on it is a descriptor. It prints the /proc/<pid>/fd/<fd> path to reopen:
	// that is a procfs symlink to the underlying object, so opening it yields a
	// fresh descriptor to the same memfd, not a copy of the bytes.
	// It only resolves while the generator is alive with that descriptor open.
	if (argc != 2)
	{
		std::println(stderr, "usage: {} /proc/<pid>/fd/<fd>   (path printed by market_generator)", argv[0]);
		return EXIT_FAILURE;
	}

	int marketFd = ::open(argv[1], O_RDONLY);
	if (marketFd == -1)
	{
		std::perror("open market");
		return EXIT_FAILURE;
	}

	constexpr int target_cpu = 8; // isolating cpu8

	pin_to_cpu(target_cpu);

	std::println("running on CPU {}", sched_getcpu());

	matcher::startMatch(marketFd);

	::close(marketFd);

	return 0;
}