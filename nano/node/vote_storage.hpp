#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/processing_queue.hpp>

#include <vector>

namespace nano
{
class network;
class store;
class stat;
class ledger;
class vote;
namespace transport
{
	class channel;
}

class vote_storage final
{
public:
	vote_storage (nano::node &, nano::store &, nano::network &, nano::ledger &, nano::stat &);
	~vote_storage ();

	void start ();
	void stop ();

public:
	/**
	 * Store vote in database
	 */
	void vote (std::shared_ptr<nano::vote> const &);

	/**
	 * Trigger vote broadcast for hash
	 */
	void trigger (nano::block_hash const &, std::shared_ptr<nano::transport::channel> const &);

public: // Dependencies
	nano::node & node;
	nano::store & store;
	nano::network & network;
	nano::ledger & ledger;
	nano::stat & stats;

private:
	nano::processing_queue<std::shared_ptr<nano::vote>> store_queue;

	using vote_list_t = std::vector<std::shared_ptr<nano::vote>>;
	//	using broadcast_entry_t = std::tuple<vote_list_t, std::shared_ptr<nano::transport::channel>>;
	using broadcast_entry_t = std::pair<nano::block_hash, std::shared_ptr<nano::transport::channel>>;

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
	void broadcast (vote_list_t const &);

private:
	// TODO: Use nodeconfig
	uint128_t const vote_weight_threshold{ 60000 * nano::Gxrb_ratio };
	//	uint128_t const vote_weight_threshold{ 120000 * nano::Gxrb_ratio }; // Disable

	uint128_t const rep_weight_threshold{ 600 * nano::Gxrb_ratio };
};
}