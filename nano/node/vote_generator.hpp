#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/processing_queue.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_set>
#include <variant>

namespace mi = boost::multi_index;

namespace nano
{
class vote_generator final
{
private:
	using candidate_t = std::pair<nano::root, nano::block_hash>;
	using request_t = std::pair<std::vector<candidate_t>, std::shared_ptr<nano::transport::channel>>;

public:
	vote_generator (nano::node_config const &, nano::node &, nano::ledger &, nano::wallets &, nano::vote_processor &, nano::local_vote_history &, nano::network &, nano::stats &, nano::logger &, bool is_final);
	~vote_generator ();

	void start ();
	void stop ();

	/** Queue items for vote generation, or broadcast votes already in cache */
	void add (nano::root const &, nano::block_hash const &);

	/** Queue blocks for vote generation, returning the number of successful candidates.*/
	std::size_t generate (std::vector<std::shared_ptr<nano::block>> const & blocks, std::shared_ptr<nano::transport::channel> const &);

	// TODO: This is unnecessary
	void set_reply_action (std::function<void (std::shared_ptr<nano::vote> const &, std::shared_ptr<nano::transport::channel> const &)>);

	std::unique_ptr<container_info_component> collect_container_info (std::string const & name) const;

private:
	using transaction_variant_t = std::variant<nano::secure::read_transaction, nano::secure::write_transaction>;

	void run ();
	void run_verification ();
	void broadcast (nano::unique_lock<nano::mutex> &);
	void reply (nano::unique_lock<nano::mutex> &, request_t &&);
	void vote (std::vector<nano::block_hash> const &, std::vector<nano::root> const &, std::function<void (std::shared_ptr<nano::vote> const &)> const &);
	void broadcast_action (std::shared_ptr<nano::vote> const &) const;
	std::deque<candidate_t> next_batch (size_t max_count);
	void verify_batch (std::deque<candidate_t> & batch);
	bool should_vote (transaction_variant_t const &, nano::root const &, nano::block_hash const &) const;

private:
	std::function<void (std::shared_ptr<nano::vote> const &, std::shared_ptr<nano::transport::channel> &)> reply_action; // must be set only during initialization by using set_reply_action

private: // Dependencies
	nano::node_config const & config;
	nano::node & node;
	nano::ledger & ledger;
	nano::wallets & wallets;
	nano::vote_processor & vote_processor;
	nano::local_vote_history & history;
	nano::network & network;
	nano::stats & stats;
	nano::logger & logger;

private:
	std::unique_ptr<nano::vote_spacing> spacing_impl;
	nano::vote_spacing & spacing;

private:
	const bool is_final;
	static std::size_t constexpr max_requests{ 2048 };

	std::shared_ptr<nano::transport::channel> inproc_channel;

	std::unordered_set<candidate_t> queued;
	std::deque<request_t> requests;
	std::deque<candidate_t> candidates;

	mutable nano::mutex mutex;
	nano::condition_variable condition;
	std::atomic<bool> stopped{ false };
	std::thread thread;
	std::thread verification_thread;
};
}
