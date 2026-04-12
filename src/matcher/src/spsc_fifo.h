#pragma once
#include <cstdint>
#include <vector>

template <typename T>
class spsc
{
public:
	void try_push(T element);
	void try_pop(T element);

private:
	alignas(64) uint64_t readIdx;
	alignas(64) uint64_t writeIdx;
	std::vector<T> data;
};

