#include <nano/node/election.hpp>
#include <nano/node/election_container.hpp>

#include <ranges>

void nano::election_container::insert (std::shared_ptr<nano::election> const & election, nano::election_behavior behavior, nano::bucket_t bucket, nano::priority_t priority)
{
	debug_assert (!exists (election)); // Should be checked before calling insert

	entry entry{ election, election->qualified_root, behavior, bucket, priority };

	debug_assert (!by_ptr.contains (election));
	debug_assert (!by_root.contains (election->qualified_root));
	debug_assert (!by_behavior[behavior].map[bucket].map.contains (entry));

	by_ptr.insert ({ election, entry });
	by_root.insert ({ election->qualified_root, entry });

	auto & by_bucket = by_behavior[behavior];
	auto & by_priority = by_bucket.map[bucket];

	by_priority.map.insert (entry);

	// Update cached size
	++by_bucket.total;
	++by_priority.total;
}

bool nano::election_container::erase (std::shared_ptr<nano::election> const & election)
{
	if (auto existing = by_ptr.find (election); existing != by_ptr.end ())
	{
		erase_impl (existing->second);
		return true; // Erased
	}
	return false;
}

void nano::election_container::erase_impl (entry entry)
{
	debug_assert (by_ptr.contains (entry.election));
	debug_assert (by_root.contains (entry.root));
	debug_assert (by_behavior[entry.behavior].map[entry.bucket].map.contains (entry));

	auto & by_bucket = by_behavior[entry.behavior];
	auto & by_priority = by_bucket.map[entry.bucket];

	by_priority.map.erase (entry);
	by_root.erase (entry.root);
	by_ptr.erase (entry.election);

	// Update cached size
	--by_bucket.total;
	--by_priority.total;
}

bool nano::election_container::exists (nano::qualified_root const & root) const
{
	return by_root.contains (root);
}

bool nano::election_container::exists (std::shared_ptr<nano::election> const & election) const
{
	return by_ptr.contains (election);
}

std::shared_ptr<nano::election> nano::election_container::election (nano::qualified_root const & root) const
{
	if (auto existing = by_root.find (root); existing != by_root.end ())
	{
		return existing->second.election;
	}
	return nullptr;
}

nano::election_container::entry nano::election_container::info (std::shared_ptr<nano::election> const & election) const
{
	if (auto existing = by_ptr.find (election); existing != by_ptr.end ())
	{
		return existing->second;
	}
	return {};
}

auto nano::election_container::list () const -> std::vector<entry>
{
	auto r = by_ptr | std::views::values;
	return { r.begin (), r.end () };
}

size_t nano::election_container::size () const
{
	return by_ptr.size ();
}

size_t nano::election_container::size (nano::election_behavior behavior) const
{
	if (auto existing = by_behavior.find (behavior); existing != by_behavior.end ())
	{
		return existing->second.total;
	}
	return 0;
}

size_t nano::election_container::size (nano::election_behavior behavior, nano::bucket_t bucket) const
{
	if (auto existing = by_behavior.find (behavior); existing != by_behavior.end ())
	{
		if (auto existing_bucket = existing->second.map.find (bucket); existing_bucket != existing->second.map.end ())
		{
			return existing_bucket->second.total;
		}
	}
	return 0;
}

auto nano::election_container::top (nano::election_behavior behavior, nano::bucket_t bucket) const -> top_entry_t
{
	if (auto existing = by_behavior.find (behavior); existing != by_behavior.end ())
	{
		if (auto existing_bucket = existing->second.map.find (bucket); existing_bucket != existing->second.map.end ())
		{
			if (!existing_bucket->second.map.empty ())
			{
				auto top = existing_bucket->second.map.begin ();
				return { top->election, top->priority };
			}
		}
	}
	return { nullptr, std::numeric_limits<nano::priority_t>::max () };
}

void nano::election_container::clear ()
{
	by_ptr.clear ();
	by_root.clear ();
	by_behavior.clear ();
}
