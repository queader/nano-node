#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>

namespace nano
{
class block;
}

namespace nano::scheduler
{
/**
 * A class which holds an ordered set of blocks to be scheduled, ordered by their block arrival time
 */
class bucket final
{
public:
	using priority_t = uint64_t;
	using bucket_t = uint64_t;

public:
	bucket (size_t max_blocks, nano::uint128_t min_balance, bucket_t index);
	~bucket ();

	bucket_t const index;
	size_t const max_blocks;
	nano::uint128_t const min_balance;

	void push (std::shared_ptr<nano::block> block, priority_t time);
	std::pair<std::shared_ptr<nano::block>, priority_t> top () const;
	void pop ();
	size_t size () const;
	bool empty () const;

private:
	class value_type
	{
	public:
		priority_t time;
		std::shared_ptr<nano::block> block;
		bool operator< (value_type const & other_a) const;
		bool operator== (value_type const & other_a) const;
	};
	std::set<value_type> queue;
};
} // namespace nano::scheduler
