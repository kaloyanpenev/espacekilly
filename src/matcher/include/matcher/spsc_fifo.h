#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <new>

// Require the type used with this to be exception-safe ctr-able and dtr-able
template <typename T>
concept ExceptionSafe = std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T>
						&& std::is_nothrow_destructible_v<T>;

template <ExceptionSafe T, typename AllocatorT = std::allocator<T>>
class spsc_fifo
{
public:
	explicit spsc_fifo(size_t capacity, const AllocatorT& allocator = AllocatorT()) : allocator_(allocator)
	{
		// make sure that capacity is at least 1
		capacity = std::max(capacity, 1UL);

		// Add one slack element so we don't confuse empty and full state
		// Full is when writeIdx + 1 == readIdx
		// Empty is when writeIdx == readIdx
		capacity++;

		// Make sure there is no overflow
		capacity_ = std::min(capacity, SIZE_MAX - 2 * pad);

		// need hidden padding at the front of the allocation block to prevent false sharing
		// so allocation = capacity + 2 * pad
		data_ = allocator_.allocate(capacity + 2 * pad);
	}
	~spsc_fifo()
	{
		while (front())
		{
			pop();
		}
		allocator_.deallocate(data_, capacity_ + 2 * pad);
	}

	spsc_fifo(const spsc_fifo&) = delete;
	spsc_fifo& operator=(const spsc_fifo&) = delete;

	// blocks if the queue is full
	template <typename... Args>
	void emplace(Args&&... args) noexcept
	{
		static_assert(std::is_nothrow_constructible_v<T, Args&&...>);

		const size_t widx = widx_.load(std::memory_order::relaxed);
		size_t nextWidx = widx + 1;

		if (nextWidx == capacity_)
		{
			nextWidx = 0;
		}

		// block and wait until there is a slot (while this is true, queue is full)
		while (nextWidx == ridxCache_)
		{
			// make sure we definitely see the order of operation to ensure we see the destruction of any elements
			ridxCache_ = ridx_.load(std::memory_order::acquire);
		}
		new (&data_[widx + pad]) T(std::forward<Args>(args)...);

		// make sure new element is visible
		widx_.store(nextWidx, std::memory_order::release);
	}

	// returns false if the queue is full
	template <typename... Args>
	bool try_emplace(Args&&... args) noexcept
	{
		static_assert(std::is_nothrow_constructible_v<T, Args&&...>);

		const size_t widx = widx_.load(std::memory_order::relaxed);
		size_t nextWidx = widx + 1;
		// handle wrap-around
		if (nextWidx == capacity_)
		{
			nextWidx = 0;
		}
		// check cached ridx
		if (nextWidx == ridxCache_)
		{
			ridxCache_ = ridx_.load(std::memory_order::acquire);
			// we've loaded the ridx and queue is still full
			if (nextWidx == ridxCache_) [[unlikely]] // unlikely to be full
			{
				// TODO: benchmark with and without unlikely
				return false;
			}
		}
		new (&data_[widx + pad]) T(std::forward<Args>(args)...);

		// make writes visible
		widx_.store(nextWidx, std::memory_order::release);
		return true;
	}

	[[nodiscard]] T* front() noexcept
	{
		const size_t ridx = ridx_.load(std::memory_order::relaxed);
		// check for empty queue
		if (ridx == widxCache_)
		{
			widxCache_ = widx_.load(std::memory_order::acquire);
			// queue is empty
			if (ridx == widxCache_)
			{
				return nullptr;
			}
		}
		return &data_[ridx + pad];
	}

	// assumes the user is 100% sure that the queue is _not_ empty, i.e. front() does not return nullptr.
	// assert-guarded in debug mode
	void pop() noexcept
	{
		const size_t ridx = ridx_.load(std::memory_order::relaxed);
		// debug assert t make sure that queue is not empty
		assert(ridx != widx_.load(std::memory_order::acquire) && "Only call fast_pop if front() returns non-nullptr");

		data_[ridx + pad].~T();

		// handle wraparound
		auto nextRidx = ridx + 1;
		if (nextRidx == capacity_) [[unlikely]]
		{
			nextRidx = 0;
		}

		// memory_order::release in order to make data modification visible
		ridx_.store(nextRidx, std::memory_order::release);
	}

	bool try_pop() noexcept
	{
		const size_t ridx = ridx_.load(std::memory_order::relaxed);
		if (ridx == widxCache_)
		{
			widxCache_ = widx_.load(std::memory_order::acquire);
			// queue is empty
			if (ridx == widxCache_)
			{
				return false;
			}
		}
		data_[ridx + pad].~T();

		// handle wraparound
		auto nextRidx = ridx + 1;
		if (nextRidx == capacity_)
		{
			nextRidx = 0;
		}

		// memory_order::release in order to make data modification visible
		ridx_.store(nextRidx, std::memory_order::release);
		return true;
	}

	// can be called from both threads
	[[nodiscard]] bool is_empty() const noexcept
	{
		return ridx_.load(std::memory_order::acquire) == widx_.load(std::memory_order::acquire);
	}

	// can be called from both threads
	[[nodiscard]] bool is_full() const noexcept
	{
		auto nextWidx = widx_.load(std::memory_order::acquire) + 1;
		if (nextWidx == capacity_) [[unlikely]]
		{
			nextWidx = 0;
		}
		return nextWidx == ridx_.load(std::memory_order::acquire);
	}

	// can be called from both threads
	[[nodiscard]] size_t size() const noexcept
	{
		ptrdiff_t diff = widx_.load(std::memory_order::acquire) - ridx_.load(std::memory_order::acquire);
		if (diff < 0)
		{
			diff += capacity_;
		}
		return static_cast<size_t>(diff);
	}

	[[nodiscard]] size_t capacity() const noexcept { return capacity_ - 1; }

private:
	// TODO: test on different arch, performance when CPU prefetches 2 cache lines
	static constexpr size_t cacheLineSize = std::hardware_destructive_interference_size;
	static constexpr size_t pad = (cacheLineSize - 1) / sizeof(T) + 1;

	T* data_;
	size_t capacity_;

	// use no_unique_address to avoid using empty base optimization and make sure zero-state allocator object takes no memory
	AllocatorT allocator_ [[no_unique_address]];

	alignas(cacheLineSize) std::atomic<size_t> widx_{0}; // write index
	alignas(cacheLineSize) size_t ridxCache_{0};         // read index cache

	alignas(cacheLineSize) std::atomic<size_t> ridx_{0}; //read index
	alignas(cacheLineSize) size_t widxCache_{0};         // write index cache
};
