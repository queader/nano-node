#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/processing_queue.hpp>
#include <nano/store/component.hpp>

#include <unordered_set>
#include <vector>

namespace nano
{
class network;
class stats;
class ledger;
class vote;
namespace transport
{
	class channel;
}

class vote_storage final
{
	using vote_list_t = std::vector<std::shared_ptr<nano::vote>>;
	using broadcast_entry_t = std::pair<nano::block_hash, std::shared_ptr<nano::transport::channel>>;

public:
	vote_storage (nano::node &, nano::store::component & vote_store, nano::network &, nano::ledger &, nano::stats &);
	~vote_storage ();

	void start ();
	void stop ();

public:
	/**
	 * Store vote in database
	 */
	void vote (std::shared_ptr<nano::vote>);

	/**
	 * Trigger vote broadcast for hash
	 */
	void trigger (nano::block_hash const &, std::shared_ptr<nano::transport::channel> const &);

public: // Dependencies
	nano::node & node;
	nano::store::component & vote_store;
	nano::network & network;
	nano::ledger & ledger;
	nano::stats & stats;

private:
	nano::processing_queue<std::shared_ptr<nano::vote>> store_queue;
	nano::processing_queue<broadcast_entry_t> broadcast_queue;

private:
	std::unordered_set<nano::block_hash> recently_broadcasted;
	mutable nano::mutex mutex;

private:
	void process_batch (decltype (store_queue)::batch_t &);
	void process_batch (decltype (broadcast_queue)::batch_t &);

	uint128_t weight (vote_list_t const &) const;
	vote_list_t filter (vote_list_t const &) const;

	void reply (vote_list_t const &, std::shared_ptr<nano::transport::channel> const &);
	void broadcast (vote_list_t const &, nano::block_hash const &);
	void broadcast_impl (vote_list_t const &);

	vote_list_t query_hash (nano::store::transaction const & vote_transaction, nano::block_hash const &, std::size_t count_threshold = 0);
	/** @returns <votes, votes frontier> */
	std::pair<vote_list_t, nano::block_hash> query_frontier (nano::store::transaction const & ledger_transaction, nano::store::transaction const & vote_transaction, nano::block_hash const &);

private:
	// TODO: Use nodeconfig
	uint128_t const vote_weight_threshold{ 60000 * nano::Gxrb_ratio };
	//	uint128_t const vote_weight_threshold{ 120000 * nano::Gxrb_ratio }; // Disable

	uint128_t const rep_weight_threshold{ 100 * nano::Gxrb_ratio };
	std::size_t const rep_count_threshold{ 15 };

private: // Flags
	static bool constexpr enable_broadcast = true;
	static bool constexpr enable_pr_broadcast = true;
	static bool constexpr enable_random_broadcast = false;
	static bool constexpr enable_query_frontier = false;
};
}