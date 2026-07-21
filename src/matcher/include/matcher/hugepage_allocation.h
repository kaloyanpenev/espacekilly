#pragma once

#include <new>
#include <cstdint>
#include <sys/mman.h>

class HugepageAllocation
{
public:


	HugepageAllocation() = delete;
	HugepageAllocation(size_t size);

	~HugepageAllocation();

	HugepageAllocation(const HugepageAllocation&) = delete;
	HugepageAllocation& operator=(const HugepageAllocation&) = delete;
	HugepageAllocation(HugepageAllocation&& other) = delete;
	HugepageAllocation& operator=(HugepageAllocation&& other) = delete;

	void* data() const { return data_; }
	size_t size() const { return size_; }

private:
	static inline constexpr std::size_t kHugePage = 2 * 1024 * 1024; // 2 MiB
	static inline constexpr size_t roundUp(size_t n, size_t page)
	{
		return (n + page - 1) & ~(page - 1);
	}

	void* data_;
	size_t size_;
};