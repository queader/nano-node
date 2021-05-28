#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>

#include <boost/optional.hpp>

#include <condition_variable>
#include <deque>
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
	void add (const std::vector<std::shared_ptr<nano::vote>>);

	boost::optional<std::vector<std::shared_ptr<nano::vote>>> get_vote_replay_cached_votes_for_hash (nano::transaction const & transaction_a, nano::block_hash hash_a, nano::uint128_t minimum_weight) const;
	boost::optional<std::vector<std::shared_ptr<nano::vote>>> get_vote_replay_cached_votes_for_hash_or_conf_frontier (nano::transaction const & transaction_a, nano::block_hash hash_a) const;
	boost::optional<std::vector<std::shared_ptr<nano::vote>>> get_vote_replay_cached_votes_for_conf_frontier (nano::transaction const & transaction_a, nano::block_hash hash_a) const;

	const nano::uint128_t replay_vote_weight_minimum;
	const nano::uint128_t replay_unconfirmed_vote_weight_minimum;

private:
	void run_aec_vote_seeding () const;
	void run_rebroadcast ();
	void run_rebroadcast_random () const;

	nano::node & node;
	nano::stat & stats;
	nano::ledger & ledger;
	nano::active_transactions & active;

	std::deque<std::vector<std::shared_ptr<nano::vote>>> replay_candidates;
	bool stopped{ false };
	nano::condition_variable condition;
	nano::condition_variable condition_candidates;
	nano::mutex mutex{ mutex_identifier (mutexes::vote_replay_cache) };
	nano::mutex mutex_candidates{ mutex_identifier (mutexes::vote_replay_cache) };

	std::thread thread_seed_votes;
	std::thread thread_rebroadcast;
	std::thread thread_rebroadcast_random;
};
}