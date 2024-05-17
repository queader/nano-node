#include <nano/lib/blocks.hpp>
#include <nano/node/scheduler/bucket.hpp>

bool nano::scheduler::bucket::value_type::operator< (value_type const & other_a) const
{
	return time < other_a.time || (time == other_a.time && block->hash () < other_a.block->hash ());
}

bool nano::scheduler::bucket::value_type::operator== (value_type const & other_a) const
{
	return time == other_a.time && block->hash () == other_a.block->hash ();
}

/*
 * bucket
 */

nano::scheduler::bucket::bucket (size_t max_blocks, nano::uint128_t min_balance, bucket_t index) :
	max_blocks{ max_blocks },
	min_balance{ min_balance },
	index{ index }
{
	debug_assert (max_blocks > 0);
}

nano::scheduler::bucket::~bucket ()
{
}

auto nano::scheduler::bucket::top () const -> std::pair<std::shared_ptr<nano::block>, priority_t>
{
	debug_assert (!queue.empty ());
	auto const & top = *queue.begin ();
	return { top.block, top.time };
}

void nano::scheduler::bucket::pop ()
{
	debug_assert (!queue.empty ());
	queue.erase (queue.begin ());
}

void nano::scheduler::bucket::push (std::shared_ptr<nano::block> block, priority_t time)
{
	queue.insert ({ time, block });
	if (queue.size () > max_blocks)
	{
		debug_assert (!queue.empty ());
		queue.erase (--queue.end ());
	}
}

size_t nano::scheduler::bucket::size () const
{
	return queue.size ();
}

bool nano::scheduler::bucket::empty () const
{
	return queue.empty ();
}
