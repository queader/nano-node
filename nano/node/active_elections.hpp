#pragma once

#include <nano/lib/enum_util.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/election_behavior.hpp>
#include <nano/node/election_container.hpp>
#include <nano/node/election_status.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/recently_cemented_cache.hpp>
#include <nano/node/recently_confirmed_cache.hpp>
#include <nano/node/vote_router.hpp>
#include <nano/node/vote_with_weight_info.hpp>
#include <nano/secure/common.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>

namespace nano
{
enum class election_state;
}

namespace nano
{
class active_elections_config final
{
public:
	explicit active_elections_config (nano::network_constants const &);

	nano::error deserialize (nano::tomlconfig & toml);
	nano::error serialize (nano::tomlconfig & toml) const;

public:
	// Maximum number of simultaneous active elections (AEC size)
	size_t size{ 5000 };
	// Limit of hinted elections as percentage of `active_elections_size`
	size_t hinted_limit_percentage{ 20 };
	// Limit of optimistic elections as percentage of `active_elections_size`
	size_t optimistic_limit_percentage{ 10 };
	// Maximum confirmation history size
	size_t confirmation_history_size{ 2048 };
	// Maximum cache size for recently_confirmed
	size_t confirmation_cache{ 65536 };

	size_t reserved_per_bucket{ 100 };
	size_t max_per_bucket{ 150 };
};

/**
 * Core class for determining consensus
 * Holds all active blocks i.e. recently added blocks that need confirmation
 */
class active_elections final
{
	friend class nano::election;

public:
	active_elections (nano::node &, nano::confirming_set &, nano::block_processor &);
	~active_elections ();

	void start ();
	void stop ();

	struct insert_result
	{
		std::shared_ptr<nano::election> election{ nullptr };
		bool inserted{ false };
	};

	insert_result insert (
	std::shared_ptr<nano::block> const &,
	nano::election_behavior = nano::election_behavior::priority,
	nano::bucket_t bucket = 0,
	nano::priority_t priority = 0);

	// Notify this container about a new block (potential fork)
	bool publish (std::shared_ptr<nano::block> const &);

	// Is the root of this block in the roots container
	bool active (nano::block const &) const;
	bool active (nano::qualified_root const &) const;
	std::shared_ptr<nano::election> election (nano::qualified_root const &) const;

	struct details_info
	{
		std::shared_ptr<nano::election> election;
		nano::election_behavior behavior;
		nano::bucket_t bucket;
		nano::priority_t priority;
	};
	std::vector<std::shared_ptr<nano::election>> list () const;
	std::vector<details_info> list_details () const;

	bool erase (nano::block const &);
	bool erase (nano::qualified_root const &);
	bool erase (std::shared_ptr<nano::election> const &);

	bool empty () const;
	size_t size () const;
	size_t size (nano::election_behavior) const;
	size_t size (nano::election_behavior, nano::bucket_t) const;

	// Returns election with the largerst "priority" number (highest timestamp). NOTE: Lower "priority" is better.
	using top_entry_t = std::pair<std::shared_ptr<nano::election>, priority_t>;
	top_entry_t top (nano::election_behavior, nano::bucket_t) const;

	struct info_result
	{
		std::shared_ptr<nano::election> top_election{ nullptr };
		nano::priority_t top_priority{ std::numeric_limits<nano::priority_t>::max () };
		size_t election_count{ 0 };
	};
	info_result info (nano::election_behavior, nano::bucket_t) const;

	// Maximum number of elections that should be present in this container
	// NOTE: This is only a soft limit, it is possible for this container to exceed this count
	int64_t limit (nano::election_behavior behavior) const;
	// How many election slots are available for specified election type
	int64_t vacancy (nano::election_behavior behavior) const;

	std::unique_ptr<container_info_component> collect_container_info (std::string const & name);

public: // TODO: Move to a separate class
	std::size_t election_winner_details_size ();
	void add_election_winner_details (nano::block_hash const &, std::shared_ptr<nano::election> const &);
	std::shared_ptr<nano::election> remove_election_winner_details (nano::block_hash const &);

public: // Events
	// TODO: Use observer_set
	std::function<void ()> vacancy_update{ [] () {} };

private:
	void request_loop ();
	void request_confirm (nano::unique_lock<nano::mutex> &);
	// Erase all blocks from active and, if not confirmed, clear digests from network filters
	void erase_impl (nano::unique_lock<nano::mutex> &, std::shared_ptr<nano::election>);
	void activate_successors (nano::secure::read_transaction const & transaction, std::shared_ptr<nano::block> const & block);
	void notify_observers (nano::secure::read_transaction const & transaction, nano::election_status const & status, std::vector<nano::vote_with_weight_info> const & votes);
	void block_cemented_callback (std::shared_ptr<nano::block> const &);
	void block_already_cemented_callback (nano::block_hash const &);

	void run_cleanup ();
	void trim (nano::unique_lock<nano::mutex> &);

	static nano::stat::type to_completion_type (nano::election_state);

private: // Dependencies
	active_elections_config const & config;
	nano::node & node;
	nano::confirming_set & confirming_set;
	nano::block_processor & block_processor;

public:
	nano::recently_confirmed_cache recently_confirmed;
	nano::recently_cemented_cache recently_cemented;

	// TODO: This mutex is currently public because many tests access it
	// TODO: This is bad. Remove the need to explicitly lock this from any code outside of this class
	mutable nano::mutex mutex{ mutex_identifier (mutexes::active) };

private:
	nano::election_container elections;

	nano::mutex election_winner_details_mutex{ mutex_identifier (mutexes::election_winner_details) };
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::election>> election_winner_details;

	// Maximum time an election can be kept active if it is extending the container
	std::chrono::seconds const election_time_to_live;

	nano::condition_variable condition;
	bool stopped{ false };
	std::thread thread;
	std::thread cleanup_thread;

public: // Tests
	void clear ();

	friend class node_fork_storm_Test;
	friend class system_block_sequence_Test;
	friend class node_mass_block_new_Test;
	friend class active_elections_vote_replays_Test;
	friend class frontiers_confirmation_prioritize_frontiers_Test;
	friend class frontiers_confirmation_prioritize_frontiers_max_optimistic_elections_Test;
	friend class confirmation_height_prioritize_frontiers_overwrite_Test;
	friend class active_elections_confirmation_consistency_Test;
	friend class node_deferred_dependent_elections_Test;
	friend class active_elections_pessimistic_elections_Test;
	friend class frontiers_confirmation_expired_optimistic_elections_removal_Test;
};
}
