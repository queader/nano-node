#include <nano/node/election_set.hpp>

nano::election_set::election_set (nano::active_transactions & active_a, std::size_t limit_a, nano::election_behavior behavior_a) :
	active{ active_a },
	limit_m{ limit_a },
	behavior_m{ behavior_a }
{
	debug_assert (limit_a > 0);
}

bool nano::election_set::vacancy (priority_t candidate) const
{
	if (active.vacancy (behavior_m) <= 0)
	{
		return false;
	}

	nano::lock_guard<nano::mutex> lock{ mutex };

	if (elections.size () < limit_m)
	{
		return true;
	}
	else
	{
		debug_assert (!elections.empty ());
		auto lowest = elections.get<tag_priority> ().begin ()->priority;
		return candidate > lowest;
	}
}

nano::election_insertion_result nano::election_set::activate (std::shared_ptr<nano::block> block, priority_t priority)
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	if (elections.size () >= limit_m)
	{
		erase_lowest ();
	}

	auto erase_callback = [this] (nano::qualified_root const & root) {
		nano::lock_guard<nano::mutex> lock{ mutex };
		elections.get<tag_root> ().erase (root);
	};

	auto result = active.insert (block, nano::election_behavior::normal, erase_callback);
	if (result.inserted)
	{
		elections.get<tag_root> ().insert ({ result.election, result.election->qualified_root, priority });
	}
	return result;
}

void nano::election_set::erase_lowest ()
{
	debug_assert (!mutex.try_lock ());
	debug_assert (!elections.empty ());

	elections.get<tag_priority> ().begin ()->election->cancel ();
}