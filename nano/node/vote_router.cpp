#include <nano/lib/enum_util.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/election.hpp>
#include <nano/node/vote_cache.hpp>
#include <nano/node/vote_router.hpp>
#include <nano/secure/vote.hpp>

#include <chrono>

using namespace std::chrono_literals;

nano::vote_router::vote_router (nano::vote_cache & vote_cache_a, nano::recently_confirmed_cache & recently_confirmed_a, nano::stats & stats_a) :
	vote_cache{ vote_cache_a },
	recently_confirmed{ recently_confirmed_a },
	stats{ stats_a }
{
}

nano::vote_router::~vote_router ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::vote_router::connect (nano::block_hash const & hash, std::shared_ptr<nano::election> const & election)
{
	debug_assert (election->blocks ().contains (hash));

	std::lock_guard lock{ mutex };
	auto [existing, inserted] = elections.emplace (hash, election->qualified_root, election);
	debug_assert (inserted || existing->election.lock () == election);
}

void nano::vote_router::disconnect (nano::block_hash const & hash, std::shared_ptr<nano::election> const & election)
{
	std::lock_guard lock{ mutex };

	// Ensure hash belongs to target election
	debug_assert (std::any_of (elections.get<tag_root> ().equal_range (election->qualified_root).first, elections.get<tag_root> ().equal_range (election->qualified_root).second, [&] (auto const & entry) {
		return entry.hash == hash && entry.election.lock () == election;
	}));

	auto erased = elections.erase (hash);
	debug_assert (erased == 1);
}

void nano::vote_router::disconnect (std::shared_ptr<nano::election> const & election)
{
	std::lock_guard lock{ mutex };

	// Ensure all hashes belong to target election
	debug_assert (std::all_of (elections.get<tag_root> ().equal_range (election->qualified_root).first, elections.get<tag_root> ().equal_range (election->qualified_root).second, [&] (auto const & entry) {
		return entry.election.lock () == election;
	}));

	auto erased = elections.get<tag_root> ().erase (election->qualified_root);
	debug_assert (erased > 0);
}

// Validate a vote and apply it to the current election if one exists
std::unordered_map<nano::block_hash, nano::vote_code> nano::vote_router::vote (std::shared_ptr<nano::vote> const & vote, nano::vote_source source, nano::block_hash filter)
{
	debug_assert (!vote->validate ()); // false => valid vote
	// If present, filter should be set to one of the hashes in the vote
	debug_assert (filter.is_zero () || std::any_of (vote->hashes.begin (), vote->hashes.end (), [&filter] (auto const & hash) {
		return hash == filter;
	}));

	std::unordered_map<nano::block_hash, nano::vote_code> results;
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::election>> process;
	{
		std::shared_lock lock{ mutex };
		for (auto const & hash : vote->hashes)
		{
			// Ignore votes for other hashes if a filter is set
			if (!filter.is_zero () && hash != filter)
			{
				continue;
			}

			// Ignore duplicate hashes (should not happen with a well-behaved voting node)
			if (results.find (hash) != results.end ())
			{
				continue;
			}

			auto find_election = [this] (auto const & hash) -> std::shared_ptr<nano::election> {
				if (auto existing = elections.find (hash); existing != elections.end ())
				{
					return existing->election.lock ();
				}
				return {};
			};

			if (auto election = find_election (hash))
			{
				process[hash] = election;
			}
			else
			{
				if (!recently_confirmed.exists (hash))
				{
					results[hash] = nano::vote_code::indeterminate;
				}
				else
				{
					results[hash] = nano::vote_code::replay;
				}
			}
		}
	}

	for (auto const & [block_hash, election] : process)
	{
		auto const vote_result = election->vote (vote->account, vote->timestamp (), block_hash, source);
		results[block_hash] = vote_result;
	}

	// All hashes should have their result set
	debug_assert (!filter.is_zero () || std::all_of (vote->hashes.begin (), vote->hashes.end (), [&results] (auto const & hash) {
		return results.find (hash) != results.end ();
	}));

	// Cache the votes that didn't match any election
	if (source != nano::vote_source::cache)
	{
		vote_cache.insert (vote, results);
	}

	vote_processed.notify (vote, source, results);

	return results;
}

bool nano::vote_router::active (nano::block_hash const & hash) const
{
	std::shared_lock lock{ mutex };
	if (auto existing = elections.find (hash); existing != elections.end ())
	{
		if (auto election = existing->election.lock ())
		{
			return true;
		}
	}
	return false;
}

bool nano::vote_router::active (nano::qualified_root const & root) const
{
	std::shared_lock lock{ mutex };
	if (auto existing = elections.get<tag_root> ().find (root); existing != elections.get<tag_root> ().end ())
	{
		if (auto election = existing->election.lock ())
		{
			return true;
		}
	}
	return false;
}

std::shared_ptr<nano::election> nano::vote_router::election (nano::block_hash const & hash) const
{
	std::shared_lock lock{ mutex };
	if (auto existing = elections.find (hash); existing != elections.end ())
	{
		if (auto election = existing->election.lock (); election != nullptr)
		{
			return election;
		}
	}
	return nullptr;
}

size_t nano::vote_router::size () const
{
	std::shared_lock lock{ mutex };
	return elections.size ();
}

void nano::vote_router::start ()
{
	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::vote_router);
		run ();
	} };
}

void nano::vote_router::stop ()
{
	std::unique_lock lock{ mutex };
	stopped = true;
	lock.unlock ();
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::vote_router::run ()
{
	std::unique_lock lock{ mutex };
	while (!stopped)
	{
		stats.inc (nano::stat::type::vote_router, nano::stat::detail::cleanup);

		erase_if (elections, [] (entry const & entry) {
			return entry.election.expired ();
		});

		condition.wait_for (lock, 15s, [&] () { return stopped; });
	}
}

nano::container_info nano::vote_router::container_info () const
{
	std::shared_lock lock{ mutex };

	nano::container_info info;
	info.put ("elections", elections);
	return info;
}

/*
 *
 */

nano::stat::detail nano::to_stat_detail (nano::vote_code code)
{
	return nano::enum_util::cast<nano::stat::detail> (code);
}

nano::stat::detail nano::to_stat_detail (nano::vote_source source)
{
	return nano::enum_util::cast<nano::stat::detail> (source);
}