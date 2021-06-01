#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>

#include <boost/optional.hpp>

#include <condition_variable>
#include <deque>
#include <unordered_set>
#include <thread>
#include <mutex>

namespace nano
{
class node;
class transaction;
class vote;

class vote_replay_cache final
{
public:
	explicit vote_replay_cache (nano::node &);
	void stop ();
	void add (const nano::block_hash, const std::vector<std::shared_ptr<nano::vote>>);
	bool add_vote_to_db (nano::write_transaction const & transaction_a, std::shared_ptr<nano::vote> const & vote_a);

	using vote_cache_result = boost::optional<std::pair<nano::block_hash, std::vector<std::shared_ptr<nano::vote>>>>;

	vote_cache_result get_vote_replay_cached_votes_for_hash (nano::transaction const & transaction_vote_cache_a, nano::block_hash hash_a) const;
	vote_cache_result get_vote_replay_cached_votes_for_hash_or_conf_frontier (nano::transaction const & transaction_a, nano::transaction const & transaction_vote_cache_a, nano::block_hash hash_a) const;
	vote_cache_result get_vote_replay_cached_votes_for_conf_frontier (nano::transaction const & transaction_a, nano::transaction const & transaction_vote_cache_a, nano::block_hash hash_a) const;

	nano::block_store & store;

	const nano::uint128_t replay_vote_weight_minimum;
	const nano::uint128_t replay_unconfirmed_vote_weight_minimum;

private:
	void run_aec_vote_seeding () const;
	void run_rebroadcast ();
	void run_rebroadcast_random () const;
	void run_prunning () const;

	nano::node & node;
	nano::stat & stats;
	nano::ledger & ledger;
	nano::active_transactions & active;

	std::deque<std::vector<std::shared_ptr<nano::vote>>> replay_candidates;
	std::unordered_set<nano::block_hash> replay_candidates_hashes;
	bool stopped{ false };
	nano::condition_variable condition;
	nano::condition_variable condition_candidates;
	nano::mutex mutex{ mutex_identifier (mutexes::vote_replay_cache) };
	nano::mutex mutex_candidates{ mutex_identifier (mutexes::vote_replay_cache) };

	std::thread thread_seed_votes;
	std::thread thread_rebroadcast;
	std::thread thread_rebroadcast_2;
	std::thread thread_rebroadcast_random;
	std::thread thread_prune;
};
}