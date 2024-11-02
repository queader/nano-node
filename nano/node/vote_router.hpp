#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/fwd.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace mi = boost::multi_index;

namespace nano
{
enum class vote_code
{
	invalid, // Vote is not signed correctly
	replay, // Vote does not have the highest timestamp, it's a replay
	vote, // Vote has the highest timestamp
	indeterminate, // Unknown if replay or vote
	ignored, // Vote is valid, but got ingored (e.g. due to cooldown)
};

nano::stat::detail to_stat_detail (vote_code);

enum class vote_source
{
	live,
	rebroadcast,
	cache,
};

nano::stat::detail to_stat_detail (vote_source);

// This class routes votes to their associated election
// This class holds a weak_ptr as this container does not own the elections
// Routing entries are removed periodically if the weak_ptr has expired
class vote_router final
{
public:
	vote_router (nano::vote_cache &, nano::recently_confirmed_cache &, nano::stats &);
	~vote_router ();

	// Add a route for 'hash' to 'election'
	// Existing routes will be replaced
	// Election must hold the block for the hash being passed in
	void connect (nano::block_hash const & hash, std::shared_ptr<nano::election> const & election);

	// Remove the route for 'hash'
	void disconnect (nano::block_hash const & hash, std::shared_ptr<nano::election> const & election);

	// Remove all routes to this election
	void disconnect (std::shared_ptr<nano::election> const & election);

	// Route vote to associated elections
	// Distinguishes replay votes, cannot be determined if the block is not in any election
	// If 'filter' parameter is non-zero, only elections for the specified hash are notified.
	// This eliminates duplicate processing when triggering votes from the vote_cache as the result of a specific election being created.
	std::unordered_map<nano::block_hash, nano::vote_code> vote (std::shared_ptr<nano::vote> const &, nano::vote_source = nano::vote_source::live, nano::block_hash filter = { 0 });

	bool active (nano::block_hash const & hash) const;
	bool active (nano::qualified_root const & root) const;
	std::shared_ptr<nano::election> election (nano::block_hash const & hash) const;

	size_t size () const;

	void start ();
	void stop ();

	using vote_processed_event_t = nano::observer_set<std::shared_ptr<nano::vote> const &, nano::vote_source, std::unordered_map<nano::block_hash, nano::vote_code> const &>;
	vote_processed_event_t vote_processed;

	nano::container_info container_info () const;

private: // Dependencies
	nano::vote_cache & vote_cache;
	nano::recently_confirmed_cache & recently_confirmed;
	nano::stats & stats;

private:
	void run ();

private:
	struct entry
	{
		nano::block_hash hash;
		nano::qualified_root qualified_root;
		std::weak_ptr<nano::election> election;
	};

	// clang-format off
	class tag_hash {};
	class tag_election {};
	class tag_root {};

	// Mapping of block hashes to elections.
	using ordered_elections = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<entry, nano::block_hash, &entry::hash>>,
		mi::hashed_non_unique<mi::tag<tag_root>,
			mi::member<entry, nano::qualified_root, &entry::qualified_root>>
	>>;
	// clang-format on

	ordered_elections elections;

	bool stopped{ false };
	std::condition_variable_any condition;
	mutable std::shared_mutex mutex;
	std::thread thread;
};
}
