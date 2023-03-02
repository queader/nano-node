#include <nano/lib/blocks.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/prioritization.hpp>

#include <string>

/*
 * value_type
 */

// TODO: Use spaceship
bool nano::prioritization::value_type::operator<(value_type const & other_a) const
{
	return priority < other_a.priority || (priority == other_a.priority && block->hash () < other_a.block->hash ());
}

bool nano::prioritization::value_type::operator== (value_type const & other_a) const
{
	return priority == other_a.priority && block->hash () == other_a.block->hash ();
}

/*
 * bucket
 */

nano::prioritization::bucket::bucket (std::size_t limit, const election_set_factory_t & factory) :
	limit_m{ limit }
{
	elections = factory ();
}

void nano::prioritization::bucket::insert (std::shared_ptr<nano::block> block, priority_t priority)
{
	queue.emplace (value_type{ priority, std::move (block) });
	if (queue.size () > limit_m)
	{
		pop ();
	}
}

nano::election_insertion_result nano::prioritization::bucket::activate ()
{
	debug_assert (!queue.empty ());
	auto top = queue.begin ();
	auto result = elections->activate (top->block, top->priority);
	pop ();
	return result;
}

bool nano::prioritization::bucket::vacancy () const
{
	return elections->vacancy (queue.begin ()->priority);
}

bool nano::prioritization::bucket::available () const
{
	return !empty () && vacancy ();
}

void nano::prioritization::bucket::pop ()
{
	debug_assert (!queue.empty ());
	queue.erase (--queue.end ());
}

bool nano::prioritization::bucket::empty () const
{
	return queue.empty ();
}

std::size_t nano::prioritization::bucket::size () const
{
	return queue.size ();
}

/*
 * prioritization
 */

/**
 * Prioritization constructor, construct a container containing approximately 'maximum' number of blocks.
 * @param maximum number of blocks that this container can hold, this is a soft and approximate limit.
 */
nano::prioritization::prioritization (std::size_t max_size_a) :
	max_size{ max_size_a }
{
	auto build_region = [this] (uint128_t const & begin, uint128_t const & end, size_t count) {
		auto width = (end - begin) / count;
		for (auto i = 0; i < count; ++i)
		{
			minimums.push_back (begin + i * width);
		}
	};
	minimums.push_back (uint128_t{ 0 });
	build_region (uint128_t{ 1 } << 88, uint128_t{ 1 } << 92, 2);
	build_region (uint128_t{ 1 } << 92, uint128_t{ 1 } << 96, 4);
	build_region (uint128_t{ 1 } << 96, uint128_t{ 1 } << 100, 8);
	build_region (uint128_t{ 1 } << 100, uint128_t{ 1 } << 104, 16);
	build_region (uint128_t{ 1 } << 104, uint128_t{ 1 } << 108, 16);
	build_region (uint128_t{ 1 } << 108, uint128_t{ 1 } << 112, 8);
	build_region (uint128_t{ 1 } << 112, uint128_t{ 1 } << 116, 4);
	build_region (uint128_t{ 1 } << 116, uint128_t{ 1 } << 120, 2);
	minimums.push_back (uint128_t{ 1 } << 120);
}

void nano::prioritization::setup (election_set_factory_t const & factory)
{
	auto const bucket_size = std::max (1lu, max_size / buckets.size ());
	for (auto i = 0; i < minimums.size (); ++i)
	{
		buckets.emplace_back (bucket_size, factory);
	}
	populate_schedule ();
	current = schedule.begin ();
}

bool nano::prioritization::available () const
{
	if (empty ())
	{
		return false;
	}
	else
	{
		return std::any_of (buckets.begin (), buckets.end (), [] (bucket const & bucket) {
			return bucket.available ();
		});
	}
}

/** Moves the bucket pointer to the next bucket */
void nano::prioritization::next ()
{
	++current;
	if (current == schedule.end ())
	{
		current = schedule.begin ();
	}
}

nano::prioritization::bucket & nano::prioritization::current_bucket ()
{
	return buckets[*current];
}

/** Seek to the next non-empty bucket, if one exists */
void nano::prioritization::seek (bool skip_first)
{
	if (skip_first)
	{
		next ();
	}
	for (std::size_t i = 0, n = schedule.size (); i < n; ++i, next ())
	{
		if (current_bucket ().available ())
		{
			return;
		}
	}
}

/** Initialise the schedule vector */
void nano::prioritization::populate_schedule ()
{
	for (auto i = 0; i < buckets.size (); ++i)
	{
		schedule.push_back (i);
	}
}

std::size_t nano::prioritization::index (nano::uint128_t const & balance) const
{
	auto index = std::upper_bound (minimums.begin (), minimums.end (), balance) - minimums.begin () - 1;
	return index;
}

/**
 * Push a block and its associated time into the prioritization container.
 * The time is given here because sideband might not exist in the case of state blocks.
 */
void nano::prioritization::insert (uint64_t time, std::shared_ptr<nano::block> block, nano::amount const & priority)
{
	auto was_empty = empty ();
	auto & bucket = buckets[index (priority.number ())];
	bucket.insert (std::move (block), time);
	if (was_empty)
	{
		seek ();
	}
}

nano::election_insertion_result nano::prioritization::activate ()
{
	debug_assert (!empty ());

	seek (false); // Find first available bucket, including current

	if (!current_bucket ().empty ())
	{
		auto result = current_bucket ().activate ();
		seek (); // Skip to the next available bucket, excluding current
		return result;
	}
	else
	{
		return {}; // Failed to find available bucket
	}
}

///** Return the highest priority block of the current bucket */
// std::shared_ptr<nano::block> nano::prioritization::top () const
//{
//	debug_assert (!empty ());
//	debug_assert (!buckets[*current].empty ());
//	auto result = buckets[*current].begin ()->block;
//	return result;
// }
//
///** Pop the current block from the container and seek to the next block, if it exists */
// void nano::prioritization::pop ()
//{
//	debug_assert (!empty ());
//	debug_assert (!buckets[*current].empty ());
//	auto & bucket = buckets[*current];
//	bucket.erase (bucket.begin ());
//	seek ();
// }

/** Returns the total number of blocks in buckets */
std::size_t nano::prioritization::size () const
{
	std::size_t result{ 0 };
	for (auto const & queue : buckets)
	{
		result += queue.size ();
	}
	return result;
}

/** Returns number of buckets, 129 by default */
std::size_t nano::prioritization::bucket_count () const
{
	return buckets.size ();
}

/** Returns number of items in bucket with index 'index' */
std::size_t nano::prioritization::bucket_size (std::size_t index) const
{
	return buckets[index].size ();
}

/** Returns true if all buckets are empty */
bool nano::prioritization::empty () const
{
	return std::all_of (buckets.begin (), buckets.end (), [] (bucket const & bucket_a) {
		return bucket_a.empty ();
	});
}

///** Print the state of the class in stderr */
// void nano::prioritization::dump () const
//{
//	for (auto const & i : buckets)
//	{
//		for (auto const & j : i)
//		{
//			std::cerr << j.time << ' ' << j.block->hash ().to_string () << '\n';
//		}
//	}
//	std::cerr << "current: " << std::to_string (*current) << '\n';
// }

std::unique_ptr<nano::container_info_component> nano::prioritization::collect_container_info (std::string const & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	for (auto i = 0; i < buckets.size (); ++i)
	{
		auto const & bucket = buckets[i];
		composite->add_component (std::make_unique<container_info_leaf> (container_info{ std::to_string (i), bucket.size (), 0 }));
	}
	return composite;
}
