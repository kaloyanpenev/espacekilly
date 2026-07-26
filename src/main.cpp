#include <matcher/dro/spsc-queue.hpp>
#include <matcher/matcher.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sched.h>
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

int main()
{
	constexpr int target_cpu = 8; // isolating cpu8

	pin_to_cpu(target_cpu);

	std::println("running on CPU {}", sched_getcpu());

	matcher::startMatch();

	return 0;
}