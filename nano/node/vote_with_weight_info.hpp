#pragma once

#include <nano/lib/numbers.hpp>

#include <chrono>

namespace nano
{
struct vote_with_weight_info
{
	nano::account representative;
	std::chrono::steady_clock::time_point time;
	uint64_t timestamp;
	nano::block_hash hash;
	nano::uint128_t weight;
};
}
