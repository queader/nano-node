#include <nano/node/election.hpp>
#include <nano/node/election_container.hpp>

#include <ranges>

void nano::election_container::insert (std::shared_ptr<nano::election> const & election, nano::election_behavior behavior, nano::bucket_t bucket, nano::priority_t priority)
{
	debug_assert (election->behavior () == behavior);
	debug_assert (!exists (election)); // Should be checked before inserting
	debug_assert (!entries.get<tag_ptr> ().contains (election));

	auto [it, inserted] = entries.emplace_back (entry{ election, election->qualified_root, behavior, bucket, priority });
	debug_assert (inserted);

	// Update cached size
	size_by_behavior[{ behavior }]++;
	size_by_bucket[{ behavior, bucket }]++;
}

bool nano::election_container::erase (std::shared_ptr<nano::election> const & election)
{
	auto maybe_entry = info (election);
	if (!maybe_entry)
	{
		return false; // Not found
	}
	auto entry = *maybe_entry;

	auto erased = entries.get<tag_ptr> ().erase (election);
	release_assert (erased == 1);

	// Update cached size
	size_by_behavior[{ entry.behavior }]--;
	size_by_bucket[{ entry.behavior, entry.bucket }]--;

	return true; // Erased
}

bool nano::election_container::exists (nano::qualified_root const & root) const
{
	return entries.get<tag_root> ().contains (root);
}

bool nano::election_container::exists (std::shared_ptr<nano::election> const & election) const
{
	return entries.get<tag_ptr> ().contains (election);
}

std::shared_ptr<nano::election> nano::election_container::election (nano::qualified_root const & root) const
{
	if (auto existing = entries.get<tag_root> ().find (root); existing != entries.get<tag_root> ().end ())
	{
		return existing->election;
	}
	return nullptr;
}

auto nano::election_container::info (std::shared_ptr<nano::election> const & election) const -> std::optional<entry>
{
	if (auto existing = entries.get<tag_ptr> ().find (election); existing != entries.get<tag_ptr> ().end ())
	{
		return *existing;
	}
	return std::nullopt;
}

auto nano::election_container::list () const -> std::vector<entry>
{
	auto r = entries.get<tag_sequenced> () | std::views::transform ([] (auto const & entry) { return entry; });
	return { r.begin (), r.end () };
}

size_t nano::election_container::size () const
{
	return entries.size ();
}

size_t nano::election_container::size (nano::election_behavior behavior) const
{
	if (auto existing = size_by_behavior.find ({ behavior }); existing != size_by_behavior.end ())
	{
		return existing->second;
	}
	return 0;
}

size_t nano::election_container::size (nano::election_behavior behavior, nano::bucket_t bucket) const
{
	if (auto existing = size_by_bucket.find ({ behavior, bucket }); existing != size_by_bucket.end ())
	{
		return existing->second;
	}
	return 0;
}

auto nano::election_container::top (nano::election_behavior behavior, nano::bucket_t bucket) const -> top_entry_t
{
	auto & index = entries.get<tag_key> ();

	// Returns an iterator pointing to the first element with key greater than x
	auto existing = index.upper_bound (key{ behavior, bucket, std::numeric_limits<nano::priority_t>::max () });
	if (existing != index.begin () && existing != index.end ())
	{
		--existing;
		if (existing->behavior == behavior && existing->bucket == bucket)
		{
			return { existing->election, existing->priority };
		}
	}

	return { nullptr, std::numeric_limits<nano::priority_t>::max () };
}

void nano::election_container::clear ()
{
	entries.clear ();
	size_by_behavior.clear ();
	size_by_bucket.clear ();
}
