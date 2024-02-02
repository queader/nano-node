#include <nano/lib/blocks.hpp>
#include <nano/node/scheduler/bucket.hpp>
#include <nano/node/scheduler/election_set.hpp>

bool nano::scheduler::bucket::entry::operator< (entry const & other_a) const
{
	return time < other_a.time || (time == other_a.time && block->hash () < other_a.block->hash ());
}

bool nano::scheduler::bucket::entry::operator== (entry const & other_a) const
{
	return time == other_a.time && block->hash () == other_a.block->hash ();
}

/*
 * bucket
 */

nano::scheduler::bucket::bucket (nano::uint128_t minimum_balance, std::size_t reserved_elections, nano::active_transactions & active) :
	minimum_balance{ minimum_balance },
	reserved_elections{ reserved_elections },
	active{ active },
	elections{ active, reserved_elections }
{
}

nano::scheduler::bucket::~bucket ()
{
}

bool nano::scheduler::bucket::available () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	if (queue.empty ())
	{
		return false;
	}
	else
	{
		return elections.vacancy (queue.begin ()->time);
	}
}

bool nano::scheduler::bucket::activate ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	if (!queue.empty ())
	{
		entry top = *queue.begin ();
		queue.erase (queue.begin ());

		auto result = elections.activate (top.block, top.time);
		if (result.inserted)
		{
			return true; // Activated
		}
	}

	return false; // Not activated
}

void nano::scheduler::bucket::push (uint64_t time, std::shared_ptr<nano::block> block)
{
	queue.insert ({ time, block });
	if (queue.size () > max_entries)
	{
		release_assert (!queue.empty ());
		queue.erase (--queue.end ());
	}
}

std::shared_ptr<nano::block> nano::scheduler::bucket::top () const
{
	debug_assert (!queue.empty ());
	return queue.begin ()->block;
}

void nano::scheduler::bucket::pop ()
{
	debug_assert (!queue.empty ());
	queue.erase (queue.begin ());
}

size_t nano::scheduler::bucket::size () const
{
	return queue.size ();
}

bool nano::scheduler::bucket::empty () const
{
	return queue.empty ();
}

void nano::scheduler::bucket::dump () const
{
	for (auto const & item : queue)
	{
		std::cerr << item.time << ' ' << item.block->hash ().to_string () << '\n';
	}
}
