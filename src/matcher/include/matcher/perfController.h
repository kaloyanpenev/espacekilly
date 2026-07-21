#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <cstdint>

class PerfCounter
{
public:

	// Which hardware event to count — extend as needed
	enum class Event : uint32_t
	{
		Instructions    = PERF_COUNT_HW_INSTRUCTIONS,
		Cycles          = PERF_COUNT_HW_CPU_CYCLES,
		CacheMisses     = PERF_COUNT_HW_CACHE_MISSES,
		BranchMisses    = PERF_COUNT_HW_BRANCH_MISSES,
		Branches        = PERF_COUNT_HW_BRANCH_INSTRUCTIONS,
	};

	explicit PerfCounter(Event event = Event::Instructions)
	{
		perf_event_attr attr;
		memset(&attr, 0, sizeof(attr));

		attr.type           = PERF_TYPE_HARDWARE;
		attr.size           = sizeof(attr);
		attr.config         = static_cast<uint64_t>(event);
		attr.disabled       = 1;        // do not start counting on open
		attr.exclude_kernel = 1;        // user-space only
		attr.exclude_hv     = 1;        // exclude hypervisor

		// pid=0  → this process
		// cpu=-1 → any cpu
		// group_fd=-1 → standalone counter
		m_fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));

		if (m_fd < 0)
			throw std::runtime_error("perf_event_open failed — are you running as root or is perf_event_paranoid set low enough?");
	}

	// Non-copyable — file descriptor ownership is singular
	PerfCounter(const PerfCounter&)            = delete;
	PerfCounter& operator=(const PerfCounter&) = delete;

	~PerfCounter() { close(m_fd); }

	// Reset the hardware counter to zero and start counting
	void start()
	{
		ioctl(m_fd, PERF_EVENT_IOC_RESET,  0);
		ioctl(m_fd, PERF_EVENT_IOC_ENABLE, 0);
	}

	// Stop counting — counter value is preserved for read()
	void stop()
	{
		ioctl(m_fd, PERF_EVENT_IOC_DISABLE, 0);
	}

	// Read the counter value — safe to call after stop()
	int64_t read() const
	{
		int64_t count = 0;
		if (::read(m_fd, &count, sizeof(count)) != sizeof(count))
			throw std::runtime_error("failed to read perf counter");
		return count;
	}

private:
	int m_fd = -1;
};

