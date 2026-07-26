#include "hugepage_allocation.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/mman.h>

HugepageAllocation::HugepageAllocation(size_t size) : size_(roundUp(size,  kHugePage))
{
	data_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

	if (data_ == MAP_FAILED)
	{
		int e = errno;                     // capture before anything else touches it
		throw std::runtime_error(std::string("mmap hugetlb failed: ") + std::strerror(e));

	}
}

HugepageAllocation::~HugepageAllocation()
{
	munmap(data_, size_);
}
