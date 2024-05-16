#pragma once

#include <nano/lib/enum_util.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/election_behavior.hpp>
#include <nano/node/election_insertion_result.hpp>
#include <nano/node/election_status.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/recently_cemented_cache.hpp>
#include <nano/node/recently_confirmed_cache.hpp>
#include <nano/node/vote_router.hpp>
#include <nano/node/vote_with_weight_info.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>

namespace mi = boost::multi_index;

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
	std::size_t size{ 5000 };
	// Limit of hinted elections as percentage of `active_elections_size`
	std::size_t hinted_limit_percentage{ 20 };
	// Limit of optimistic elections as percentage of `active_elections_size`
	std::size_t optimistic_limit_percentage{ 10 };
	// Maximum confirmation history size
	std::size_t confirmation_history_size{ 2048 };
	// Maximum cache size for recently_confirmed
	std::size_t confirmation_cache{ 65536 };
};

using bucket_t = uint64_t;
using priority_t = uint64_t;

/**
 * Core class for determining consensus
 * Holds all active blocks i.e. recently added blocks that need confirmation
 */
class active_elections final
{
	friend class nano::election;

public:
	using erased_callback_t = std::function<void (std::shared_ptr<nano::election>)>;

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
	bucket_t bucket = 0,
	priority_t priority = 0);

	// Is the root of this block in the roots container
	bool active (nano::block const &) const;
	bool active (nano::qualified_root const &) const;
	std::shared_ptr<nano::election> election (nano::qualified_root const &) const;
	// Returns a list of elections sorted by difficulty
	std::vector<std::shared_ptr<nano::election>> list_active (std::size_t = std::numeric_limits<std::size_t>::max ());
	bool erase (nano::block const &);
	bool erase (nano::qualified_root const &);
	bool publish (std::shared_ptr<nano::block> const &);

	bool empty () const;
	size_t size () const;
	size_t size (nano::election_behavior) const;
	size_t size (nano::election_behavior, bucket_t) const;

	// Returns election with the largerst "priority" number (highest timestamp). NOTE: Lower "priority" is better.
	std::shared_ptr<nano::election> top (nano::election_behavior, bucket_t) const;

	// Maximum number of elections that should be present in this container
	// NOTE: This is only a soft limit, it is possible for this container to exceed this count
	int64_t limit (nano::election_behavior behavior) const;
	// How many election slots are available for specified election type
	int64_t vacancy (nano::election_behavior behavior) const;

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
	void cleanup_election (nano::unique_lock<nano::mutex> & lock_a, std::shared_ptr<nano::election>);
	// Returns a list of elections sorted by difficulty, mutex must be locked
	std::vector<std::shared_ptr<nano::election>> list_active_impl (std::size_t) const;
	void activate_successors (nano::secure::read_transaction const & transaction, std::shared_ptr<nano::block> const & block);
	void notify_observers (nano::secure::read_transaction const & transaction, nano::election_status const & status, std::vector<nano::vote_with_weight_info> const & votes);
	void block_cemented_callback (std::shared_ptr<nano::block> const &);
	void block_already_cemented_callback (nano::block_hash const &);

	static nano::stat::type to_completion_type (nano::election_state);

private: // Dependencies
	active_elections_config const & config;
	nano::node & node;
	nano::confirming_set & confirming_set;
	nano::block_processor & block_processor;

private: // Elections
	struct key final
	{
		nano::election_behavior behavior;
		bucket_t bucket;
		priority_t priority;

		auto operator<=> (key const &) const = default;
	};

	struct entry final
	{
		nano::qualified_root root;
		std::shared_ptr<nano::election> election;
		active_elections::key key;
	};

	// clang-format off
	class tag_account {};
	class tag_root {};
	class tag_sequenced {};
	class tag_uncemented {};
	class tag_arrival {};
	class tag_hash {};
	class tag_key {};

	using ordered_elections = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<entry, nano::qualified_root, &entry::root>>,
		mi::ordered_non_unique<mi::tag<tag_key>,
			mi::member<entry, active_elections::key, &entry::key>>
	>>;
	// clang-format on
	ordered_elections elections;

public:
	class container
	{
	private:
		struct entry
		{
			std::shared_ptr<nano::election> election;
			nano::qualified_root root;
			bucket_t bucket;
			priority_t priority;
		};

		using by_ptr_map = std::unordered_map<std::shared_ptr<nano::election>, entry>;
		by_ptr_map by_ptr;

		using by_root_map = std::unordered_map<nano::qualified_root, entry>;
		by_root_map by_root;

		using by_priority_map = std::map<priority_t, entry>;
		using by_bucket_map = std::map<bucket_t, by_priority_map>;
		using by_behavior_map = std::map<nano::election_behavior, by_bucket_map>;
		by_behavior_map by_behavior;

	public:
		void insert (std::shared_ptr<nano::election> const &, nano::election_behavior, bucket_t, priority_t);

		bool erase (nano::qualified_root const &);
		bool erase (std::shared_ptr<nano::election> const &);

		bool exists (nano::qualified_root const &) const;
		bool exists (std::shared_ptr<nano::election> const &) const;

		std::shared_ptr<nano::election> election (nano::qualified_root const &) const;
		std::shared_ptr<nano::election> election (std::shared_ptr<nano::block> const &) const;
	};

public:
	recently_confirmed_cache recently_confirmed;
	recently_cemented_cache recently_cemented;

	// TODO: This mutex is currently public because many tests access it
	// TODO: This is bad. Remove the need to explicitly lock this from any code outside of this class
	mutable nano::mutex mutex{ mutex_identifier (mutexes::active) };

private:
	nano::mutex election_winner_details_mutex{ mutex_identifier (mutexes::election_winner_details) };
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::election>> election_winner_details;

	// Maximum time an election can be kept active if it is extending the container
	std::chrono::seconds const election_time_to_live;

	/** Keeps track of number of elections by election behavior (normal, hinted, optimistic) */
	nano::enum_array<nano::election_behavior, int64_t> count_by_behavior;

	nano::condition_variable condition;
	bool stopped{ false };
	std::thread thread;

	friend class election;
	friend std::unique_ptr<container_info_component> collect_container_info (active_elections &, std::string const &);

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

std::unique_ptr<container_info_component> collect_container_info (active_elections & active_elections, std::string const & name);
}
