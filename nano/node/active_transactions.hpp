#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/node/election.hpp>
#include <nano/node/inactive_cache_information.hpp>
#include <nano/node/inactive_cache_status.hpp>
#include <nano/node/voting.hpp>
#include <nano/secure/common.hpp>

#include <boost/circular_buffer.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/optional.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace mi = boost::multi_index;

namespace nano
{
class node;
class block;
class block_sideband;
class election;
class election_scheduler;
class vote;
class transaction;
class confirmation_height_processor;
class stat;

/*
 * Container to match block hashes that won elections with their associated elections
 */
class election_winners final
{
public:
	/*
	 * Atomically checks if winner for hash already exists and if not adds one
	 * @return true if winner added, false otherwise
	 */
	bool put (nano::block_hash const & hash, std::shared_ptr<nano::election> election);
	/*
	 * Atomically checks if winner for hash exists and erases it
	 * @return if winner was present, returns an associated election
	 */
	std::optional<std::shared_ptr<nano::election>> erase (nano::block_hash const & hash);

	bool exists (nano::block_hash const & hash) const;
	std::size_t size () const;

private:
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::election>> winners;

	mutable nano::mutex mutex{ mutex_identifier (mutexes::election_winner_details) };
};

class election_insertion_result final
{
public:
	std::shared_ptr<nano::election> election;
	bool inserted{ false };
};

// Core class for determining consensus
// Holds all active blocks i.e. recently added blocks that need confirmation
class active_transactions final
{
	class conflict_info final
	{
	public:
		nano::qualified_root root;
		std::shared_ptr<nano::election> election;
	};

public:
	explicit active_transactions (nano::node &, nano::confirmation_height_processor &);
	~active_transactions ();

	/*
	 * Inserts election, right now a simple wrapper for `insert_impl`
	 */
	nano::election_insertion_result insert (std::shared_ptr<nano::block> const &, nano::election_behavior = nano::election_behavior::normal, std::function<void (std::shared_ptr<nano::block> const &)> const & confirmation_callback = nullptr);
	/*
	 * Validate a vote and apply it to the current election if one exists
	 * Distinguishes replay votes, cannot be determined if the block is not in any election
	 */
	nano::vote_code vote (std::shared_ptr<nano::vote> const &);
	// Is the root of this block in the roots container
	bool active (nano::block const &) const;
	bool active (nano::qualified_root const &) const;
	std::shared_ptr<nano::election> election (nano::qualified_root const &) const;
	std::shared_ptr<nano::block> winner (nano::block_hash const &) const;
	// Returns a list of elections sorted by difficulty
	std::vector<std::shared_ptr<nano::election>> list_active (std::size_t = std::numeric_limits<std::size_t>::max ());
	void erase (nano::block const &);
	void erase_hash (nano::block_hash const &);
	void erase_oldest ();
	bool empty () const;
	std::size_t size () const;
	void stop ();
	bool publish (std::shared_ptr<nano::block> const &);

	int64_t vacancy () const;
	std::function<void ()> vacancy_update{ [] () {} };

	std::deque<nano::election_status> list_recently_cemented ();
	void add_recently_cemented (nano::election_status const &);

	void add_recently_confirmed (nano::qualified_root const &, nano::block_hash const &);
	void erase_recently_confirmed (nano::block_hash const &);

	void add_inactive_votes_cache (nano::unique_lock<nano::mutex> &, nano::block_hash const &, nano::account const &, uint64_t const);
	// Inserts an election if conditions are met
	void trigger_inactive_votes_cache_election (std::shared_ptr<nano::block> const &);
	nano::inactive_cache_information find_inactive_votes_cache (nano::block_hash const &);
	void erase_inactive_votes_cache (nano::block_hash const &);
	std::size_t inactive_votes_cache_size ();

	nano::vote_generator generator;
	nano::vote_generator final_generator;

	election_winners winners;

#ifdef MEMORY_POOL_DISABLED
	using allocator = std::allocator<nano::inactive_cache_information>;
#else
	using allocator = boost::fast_pool_allocator<nano::inactive_cache_information>;
#endif

private: // Dependencies
	nano::node & node;
	nano::election_scheduler & scheduler;
	nano::confirmation_height_processor & confirmation_height_processor;

private:
	// Call action with confirmed block, may be different than what we started with
	nano::election_insertion_result insert_impl (nano::unique_lock<nano::shared_mutex> &, std::shared_ptr<nano::block> const &, nano::election_behavior = nano::election_behavior::normal, std::function<void (std::shared_ptr<nano::block> const &)> const & = nullptr);
	nano::election_insertion_result insert_hinted (nano::unique_lock<nano::shared_mutex> & lock_a, std::shared_ptr<nano::block> const & block_a);
	void request_loop ();
	void request_confirm (nano::unique_lock<nano::shared_mutex> &);
	void erase (nano::qualified_root const &);
	// Erase all blocks from active and, if not confirmed, clear digests from network filters
	void cleanup_election (nano::unique_lock<nano::shared_mutex> & lock_a, std::shared_ptr<nano::election>);
	// Returns a list of elections sorted by difficulty, mutex must be locked
	std::vector<std::shared_ptr<nano::election>> list_active_impl (std::size_t) const;

	nano::inactive_cache_status inactive_votes_bootstrap_check (nano::unique_lock<nano::mutex> &, std::vector<std::pair<nano::account, uint64_t>> const &, nano::block_hash const &, nano::inactive_cache_status const &);
	nano::inactive_cache_status inactive_votes_bootstrap_check (nano::unique_lock<nano::mutex> &, nano::account const &, nano::block_hash const &, nano::inactive_cache_status const &);
	nano::inactive_cache_status inactive_votes_bootstrap_check_impl (nano::unique_lock<nano::mutex> &, nano::uint128_t const &, std::size_t, nano::block_hash const &, nano::inactive_cache_status const &);
	nano::inactive_cache_information find_inactive_votes_cache_impl (nano::block_hash const &);

	void block_cemented_callback (std::shared_ptr<nano::block> const &);
	void block_already_cemented_callback (nano::block_hash const &);

	boost::optional<nano::election_status_type> confirm_by_confirmation_height (std::shared_ptr<nano::block> const &);

private: // Internal containers
	// clang-format off
	class tag_account {};
	class tag_random_access {};
	class tag_root {};
	class tag_sequence {};
	class tag_uncemented {};
	class tag_arrival {};
	class tag_hash {};
	// clang-format on

	// clang-format off
	using ordered_roots = boost::multi_index_container<conflict_info,
	mi::indexed_by<
		mi::random_access<mi::tag<tag_random_access>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<conflict_info, nano::qualified_root, &conflict_info::root>>
	>>;
	// clang-format on
	ordered_roots roots;

	std::unordered_map<nano::block_hash, std::shared_ptr<nano::election>> blocks;
	std::deque<nano::election_status> recently_cemented;

	// clang-format off
	using recent_confirmation = std::pair<nano::qualified_root, nano::block_hash>;
	using ordered_recent_confirmations = boost::multi_index_container<recent_confirmation,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequence>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<recent_confirmation, nano::qualified_root, &recent_confirmation::first>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<recent_confirmation, nano::block_hash, &recent_confirmation::second>>>>;
	// clang-format on
	ordered_recent_confirmations recently_confirmed;

	// clang-format off
	using ordered_cache = boost::multi_index_container<nano::inactive_cache_information,
	mi::indexed_by<
		mi::ordered_non_unique<mi::tag<tag_arrival>,
			mi::member<nano::inactive_cache_information, std::chrono::steady_clock::time_point, &nano::inactive_cache_information::arrival>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<nano::inactive_cache_information, nano::block_hash, &nano::inactive_cache_information::hash>>>, allocator>;
	// clang-format on
	ordered_cache inactive_votes_cache;

private:
	// Maximum time an election can be kept active if it is extending the container
	std::chrono::seconds const election_time_to_live;

	static std::size_t constexpr recently_confirmed_size{ 65536 };

	// Number of currently active elections with election behavior `hinted`
	int active_hinted_elections_count{ 0 };

	nano::condition_variable condition;
	bool started{ false };
	std::atomic<bool> stopped{ false };

	// TODO: { mutex_identifier (mutexes::active) }
	mutable nano::shared_mutex mutex;

	std::thread thread;

	friend std::unique_ptr<container_info_component> collect_container_info (active_transactions &, std::string const &);
	friend bool purge_singleton_inactive_votes_cache_pool_memory ();

private: // Tests
	// TODO: Instead of making so many tests friends, expose get methods for data those tests need to access
	friend class active_transactions_vote_replays_Test;
	friend class frontiers_confirmation_prioritize_frontiers_Test;
	friend class frontiers_confirmation_prioritize_frontiers_max_optimistic_elections_Test;
	friend class confirmation_height_prioritize_frontiers_overwrite_Test;
	friend class active_transactions_confirmation_consistency_Test;
	friend class node_deferred_dependent_elections_Test;
	friend class active_transactions_pessimistic_elections_Test;
	friend class frontiers_confirmation_expired_optimistic_elections_removal_Test;
	friend class active_transactions_inactive_votes_cache_existing_vote_Test;
	friend class confirmation_solicitor_batches_Test;
	friend class confirmation_height_callback_confirmed_history_Test;
	friend class votes_add_one_Test;
	friend class node_search_receivable_confirmed_Test;
	friend class node_epoch_conflict_confirm_Test;
	friend class node_dependency_graph_Test;
	friend class active_transactions_dropped_cleanup_Test;
	friend class memory_pool_validate_cleanup_Test;
};

bool purge_singleton_inactive_votes_cache_pool_memory ();
std::unique_ptr<container_info_component> collect_container_info (active_transactions & active_transactions, std::string const & name);
}
