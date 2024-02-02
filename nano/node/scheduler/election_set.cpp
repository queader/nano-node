#include <nano/node/scheduler/election_set.hpp>

nano::election_set::election_set (nano::active_transactions & active, std::size_t reserved_size, nano::election_behavior behavior) :
	active{ active },
	reserved_size{ reserved_size },
	behavior{ behavior }
{
}

bool nano::election_set::vacancy () const
{
	if (active.vacancy (behavior) > 0)
	{
		return true;
	}

	nano::lock_guard<nano::mutex> lock{ mutex };

	if (elections.size () < reserved_size)
	{
		return true;
	}

	return false;
}

bool nano::election_set::vacancy (priority_t candidate) const
{
	if (active.vacancy (behavior) > 0)
	{
		return true;
	}

	nano::lock_guard<nano::mutex> lock{ mutex };

	if (elections.size () < reserved_size)
	{
		return true;
	}
	if (!elections.empty ())
	{
		auto lowest = elections.get<tag_priority> ().begin ()->priority;
		return candidate < lowest;
	}

	return false;
}

nano::election_insertion_result nano::election_set::activate (std::shared_ptr<nano::block> block, priority_t priority)
{
	if (!vacancy ())
	{
		erase_lowest ();
	}

	auto erase_callback = [this] (std::shared_ptr<nano::election> election) {
		nano::lock_guard<nano::mutex> lock{ mutex };
		elections.get<tag_root> ().erase (election->qualified_root);
	};

	auto result = active.insert (block, nano::election_behavior::normal, erase_callback);
	if (result.inserted)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		elections.get<tag_root> ().insert ({ result.election, result.election->qualified_root, priority });
	}

	return result;
}

void nano::election_set::erase_lowest ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	if (!elections.empty ())
	{
		elections.get<tag_priority> ().begin ()->election->cancel ();
	}
}
