#pragma once

#include <cstddef>
#include <memory_resource>

#include <sys/mman.h>

#include <cassert>
#include <print>

namespace pc
{

template <typename T, size_t Capacity>
	requires(Capacity > 0)
class pmr_array final
{
public:
	template <typename... Args>
	explicit pmr_array(std::pmr::monotonic_buffer_resource& allocator, Args&&... args)
	{
		// allocate bytes for the capacity
		// this will throw if allocation is unsuccessful.

		buf = static_cast<T*>(allocator.allocate(sizeof(T) * Capacity, alignof(T)));

		for (T* p = buf; p < buf + Capacity; p++)
		{
			// don't forward intentionally - otherwise the first constructor will consume the args.
			// So if value is needed, must copy.
			std::construct_at(p, args...);
		}
	}

	~pmr_array()
	{
		// destruct all Ts.
		std::destroy_n(buf, Capacity);
	}

	pmr_array(const pmr_array&) = delete;
	pmr_array& operator=(const pmr_array&) = delete;

	pmr_array(pmr_array&&) = delete;
	pmr_array& operator=(pmr_array&&) = delete;

	static constexpr size_t size() noexcept { return Capacity; }

	T& operator[](size_t i) noexcept { return buf[i]; }
	const T& operator[](size_t i) const noexcept { return buf[i]; }

	T* begin() noexcept { return buf; }
	T* end() noexcept { return buf + Capacity; }
	const T* begin() const noexcept { return buf; }
	const T* end() const noexcept { return buf + Capacity; }
	auto rbegin() noexcept { return std::reverse_iterator(end()); }
	auto rend() noexcept { return std::reverse_iterator(begin()); }

private:
	T* buf;
};
}