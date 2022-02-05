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

class vote_storage final
{
public:
	explicit vote_storage (nano::node &);
	void stop ();
	bool add_vote (nano::write_transaction const & transaction_a, std::shared_ptr<nano::vote> const & vote_a);

	using vote_storage_result = boost::optional<std::pair<nano::block_hash, std::vector<std::shared_ptr<nano::vote>>>>;

	vote_storage_result get_votes_for_hash (nano::transaction const & transaction_a, nano::block_hash hash_a) const;
	vote_storage_result get_votes_for_hash_or_conf_frontier (nano::transaction const & transaction_a, nano::transaction const & transaction_vote_cache_a, nano::block_hash hash_a) const;
	vote_storage_result get_votes_for_conf_frontier (nano::transaction const & transaction_a, nano::transaction const & transaction_vote_cache_a, nano::block_hash hash_a) const;

	nano::store & store;

	const nano::uint128_t replay_vote_weight_minimum;

private:
	void run_pruning ();

	nano::node & node;
	nano::stat & stats;
	nano::ledger & ledger;

	bool stopped{ false };
	nano::condition_variable condition;
	nano::mutex mutex{ mutex_identifier (mutexes::vote_storage) };

	std::thread thread_prune;
};
}