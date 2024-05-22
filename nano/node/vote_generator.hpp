#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/processing_queue.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/fair_queue.hpp>
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

namespace mi = boost::multi_index;

namespace nano
{
class vote_broadcaster;

class vote_generator_config final
{
public:
};

enum class vote_type
{
	normal,
	final,
};

class vote_verifier
{
public:
	virtual bool should_vote (nano::root, nano::block_hash) = 0;
};

class vote_generator final
{
private:
	using root_hash_t = std::pair<nano::root, nano::block_hash>;
	// using request_t = std::pair<std::vector<candidate_t>, std::shared_ptr<nano::transport::channel>>;
	// using queue_entry_t = std::pair<nano::root, nano::block_hash>;

public:
	explicit vote_generator (nano::node &);
	~vote_generator ();

	void start ();
	void stop ();

	/** Request vote to be generated and broadcasted. Called from active elections. */
	void vote (std::shared_ptr<nano::block> const &, nano::vote_type);

	/** Request vote reply to be generated and sent. Called from request aggregator. */
	void reply (std::vector<std::shared_ptr<nano::block>> const &, std::shared_ptr<nano::transport::channel> const &, nano::vote_type);

	/** Queue items for vote generation, or broadcast votes already in cache */
	// void add (nano::root const &, nano::block_hash const &);
	/** Queue blocks for vote generation, returning the number of successful candidates.*/
	// std::size_t generate (std::vector<std::shared_ptr<nano::block>> const & blocks_a, std::shared_ptr<nano::transport::channel> const & channel_a);

	// void set_reply_action (std::function<void (std::shared_ptr<nano::vote> const &, std::shared_ptr<nano::transport::channel> const &)>);

	std::unique_ptr<container_info_component> collect_container_info (std::string const & name) const;

private:
	void run_voting ();
	void run_voting_batch (nano::unique_lock<nano::mutex> &);
	void run_requests ();
	void run_requests_batch (nano::unique_lock<nano::mutex> &);

	std::unique_ptr<vote_verifier> make_verifier ();
	std::unordered_set<root_hash_t> verify (std::deque<std::shared_ptr<nano::block>> const & candidates);
	std::deque<std::shared_ptr<nano::vote>> generate (std::unordered_set<root_hash_t> const & verified);

	void broadcast (nano::unique_lock<nano::mutex> &);
	void reply (nano::unique_lock<nano::mutex> &, request_t &&);
	void vote (std::vector<nano::block_hash> const &, std::vector<nano::root> const &, std::function<void (std::shared_ptr<nano::vote> const &)> const &);
	void broadcast_action (std::shared_ptr<nano::vote> const &) const;
	void process_batch (std::deque<queue_entry_t> & batch);
	/**
	 * Check if block is eligible for vote generation
	 * @param transaction : needs `tables::final_votes` lock
	 * @return: Should vote
	 */
	bool should_vote (secure::write_transaction const &, nano::root const &, nano::block_hash const &);

private:
	std::function<void (std::shared_ptr<nano::vote> const &, std::shared_ptr<nano::transport::channel> &)> reply_action; // must be set only during initialization by using set_reply_action

private: // Dependencies
	vote_generator_config const & config;
	nano::node & node;
	nano::ledger & ledger;
	nano::wallets & wallets;
	nano::vote_processor & vote_processor;
	nano::vote_broadcaster & vote_broadcaster;
	nano::local_vote_history & history;
	nano::network & network;
	nano::stats & stats;
	nano::logger & logger;

private:
	// nano::processing_queue<queue_entry_t> vote_generation_queue;

	static std::size_t constexpr max_requests{ 2048 };
	// std::deque<request_t> requests;
	// std::deque<candidate_t> candidates;

private:
	std::unique_ptr<nano::vote_spacing> spacing_impl;
	nano::vote_spacing & spacing;

	std::shared_ptr<nano::transport::channel> inproc_channel;

	using vote_request_t = std::shared_ptr<nano::block>;
	nano::fair_queue<vote_request_t, nano::no_value> vote_requests;

	using reply_request_t = std::vector<std::shared_ptr<nano::block>>;
	nano::fair_queue<reply_request_t, nano::no_value> reply_requests;

private:
	mutable nano::mutex mutex;
	nano::condition_variable condition;
	std::atomic<bool> stopped{ false };
	std::thread voting_thread;
	std::thread reply_thread; // TODO: There could be multiple reply threads
};

class vote_broadcaster final
{
public:
	void broadcast (std::deque<std::shared_ptr<nano::vote>> const & batch);
};
}
