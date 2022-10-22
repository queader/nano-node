#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/processing_queue.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace mi = boost::multi_index;

namespace nano
{
class ledger;
class network;
class node_config;
class stat;
class vote_processor;
class wallets;
namespace transport
{
	class channel;
}

class vote_spacing final
{
	class entry
	{
	public:
		nano::root root;
		std::chrono::steady_clock::time_point time;
		nano::block_hash hash;
	};

	// clang-format off
	boost::multi_index_container<entry,
		mi::indexed_by<
			mi::hashed_non_unique<mi::tag<class tag_root>,
				mi::member<entry, nano::root, &entry::root>>,
			mi::ordered_non_unique<mi::tag<class tag_time>,
				mi::member<entry, std::chrono::steady_clock::time_point, &entry::time>>
	>>
	recent;
	// clang-format on

	std::chrono::milliseconds const delay;
	void trim ();

public:
	explicit vote_spacing (std::chrono::milliseconds const & delay) :
		delay{ delay }
	{
	}

	bool votable (nano::root const & root_a, nano::block_hash const & hash_a) const;
	void flag (nano::root const & root_a, nano::block_hash const & hash_a);
	std::size_t size () const;
};

class local_vote_history final
{
	class local_vote final
	{
	public:
		local_vote (nano::root const & root_a, nano::block_hash const & hash_a, std::shared_ptr<nano::vote> const & vote_a) :
			root (root_a),
			hash (hash_a),
			vote (vote_a)
		{
		}
		nano::root root;
		nano::block_hash hash;
		std::shared_ptr<nano::vote> vote;
	};

public:
	explicit local_vote_history (nano::voting_constants const & constants) :
		constants{ constants }
	{
	}

	void add (nano::root const & root_a, nano::block_hash const & hash_a, std::shared_ptr<nano::vote> const & vote_a);
	void erase (nano::root const & root_a);

	std::vector<std::shared_ptr<nano::vote>> votes (nano::root const & root_a, nano::block_hash const & hash_a, bool const is_final_a = false) const;
	bool exists (nano::root const &) const;
	std::size_t size () const;

private:
	// clang-format off
	boost::multi_index_container<local_vote,
	mi::indexed_by<
		mi::hashed_non_unique<mi::tag<class tag_root>,
			mi::member<local_vote, nano::root, &local_vote::root>>,
		mi::sequenced<mi::tag<class tag_sequence>>>>
	history;
	// clang-format on

	nano::voting_constants const & constants;
	void clean ();
	std::vector<std::shared_ptr<nano::vote>> votes (nano::root const & root_a) const;
	// Only used in Debug
	bool consistency_check (nano::root const &) const;
	mutable nano::mutex mutex;

	friend std::unique_ptr<container_info_component> collect_container_info (local_vote_history & history, std::string const & name);
	friend class local_vote_history_basic_Test;
};

std::unique_ptr<container_info_component> collect_container_info (local_vote_history & history, std::string const & name);

/*
 * There are two types of vote generation requests:
 * - broadcast requests: requested by active elections; vote contains a single hash - current election winner
 * - reply requests: requested by 'confirm_req' messages; vote can contain up to `nano::network::confirm_ack_hashes_max` hashes
 */

/**
 * Helper class to generate votes from local wallets
 */
class wallet_voter final
{
public:
	using candidate_t = std::pair<nano::root, nano::block_hash>;

public:
	wallet_voter (nano::wallets &, nano::vote_spacing &, nano::local_vote_history &, nano::stat &, bool is_final, nano::stat::type);

	/**
	 * Checks local vote history for already cached votes and uses those if possible
	 * Otherwise check if vote spacing allows vote generation (for live elections) and generates a vote for each local representative
	 */
	std::vector<std::shared_ptr<nano::vote>> vote (std::deque<candidate_t> & candidates);
	/**
	 * Iterates through all local representatives and generates signed votes
	 * Does not do any verifications
	 */
	std::vector<std::shared_ptr<nano::vote>> generate_votes (std::vector<candidate_t> const & candidates);

private: // Dependencies
	nano::wallets & wallets;
	nano::vote_spacing & spacing;
	nano::local_vote_history & history;
	nano::stat & stats;

private:
	const bool is_final;
	const nano::stat::type stat_type;

private:
	/**
	 * Maximum number of cached votes to return per request
	 * Using cached votes is faster but requires more bandwidth
	 */
	constexpr static std::size_t max_cached_candidates = 16;
};

/**
 * Helper class to generate and broadcast votes for submitted live vote candidates
 */
class broadcast_voter final
{
public:
	using candidate_t = std::pair<nano::root, nano::block_hash>;

public:
	broadcast_voter (nano::wallet_voter &, nano::network &, nano::vote_processor &, nano::stat &, std::chrono::milliseconds max_delay, nano::stat::type);
	~broadcast_voter ();

	void start ();
	void stop ();

	/**
	 * Queue candidate for vote generation
	 */
	void submit (nano::root root, nano::block_hash hash);

private:
	void run ();
	void run_broadcast (nano::unique_lock<nano::mutex> &);
	/**
	 * Floods network with vote for live election
	 */
	void send_broadcast (std::shared_ptr<nano::vote> const &) const;

private: // Dependencies
	nano::wallet_voter & voter;
	nano::network & network;
	nano::vote_processor & vote_processor;
	nano::stat & stats;

private:
	const std::chrono::milliseconds max_delay;
	const nano::stat::type stat_type;

	mutable nano::mutex mutex;
	nano::condition_variable condition;
	std::atomic<bool> stopped{ false };
	std::thread thread;

	/* Candidates for live vote broadcasting */
	std::deque<candidate_t> candidates_m;

	constexpr static std::size_t max_candidates = 1024 * 8;

public: // Container info
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name);
};

/**
 *
 */
class normal_vote_generator final
{
private:
	using candidate_t = std::pair<nano::root, nano::block_hash>;
	using broadcast_request_t = std::pair<nano::root, nano::block_hash>;

public:
	normal_vote_generator (nano::node_config const &, nano::ledger &, nano::store &, nano::wallets &, nano::vote_processor &, nano::local_vote_history &, nano::network &, nano::stat &);
	~normal_vote_generator ();

	void start ();
	void stop ();

	/**
	 * Queue items for vote generation and broadcasting, or broadcast votes already in cache
	 * Used by active elections to generate votes for current election winner
	 */
	void broadcast (nano::root const &, nano::block_hash const &);

private:
	/**
	 * Process batch of broadcast requests
	 */
	void process_batch (std::deque<broadcast_request_t> & batch);
	/**
	 * Check if block is eligible for vote generation, then generates and broadcasts a new vote or broadcasts votes already in cache
	 * @param transaction : needs `tables::final_votes` lock
	 */
	void process (nano::transaction const &, nano::root const &, nano::block_hash const &);
	/**
	 * Checks whether we should generate a vote for a block coming from live election
	 */
	bool should_vote (nano::transaction const &, nano::root const &, nano::block_hash const &);

private: // Dependencies
	nano::node_config const & config;
	nano::ledger & ledger;
	nano::store & store;
	nano::wallets & wallets;
	nano::vote_processor & vote_processor;
	nano::local_vote_history & history;
	nano::network & network;
	nano::stat & stats;

private:
	nano::vote_spacing spacing;
	nano::wallet_voter voter;
	nano::broadcast_voter broadcaster;

	nano::processing_queue<broadcast_request_t> broadcast_requests;

private:
	constexpr static bool is_final = false;

public: // Container info
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name);
};

/**
 *
 */
class final_vote_generator final
{
private:
	using candidate_t = std::pair<nano::root, nano::block_hash>;
	using broadcast_request_t = std::pair<nano::root, nano::block_hash>;

public:
	final_vote_generator (nano::node_config const &, nano::ledger &, nano::store &, nano::wallets &, nano::vote_processor &, nano::local_vote_history &, nano::network &, nano::stat &);
	~final_vote_generator ();

	void start ();
	void stop ();

	/**
	 * Queue items for vote generation and broadcasting, or broadcast votes already in cache
	 * Used by active elections to generate votes for current election winner
	 */
	void broadcast (nano::root const &, nano::block_hash const &);

private:
	/**
	 * Process batch of broadcast requests
	 */
	void process_batch (std::deque<broadcast_request_t> & batch);
	/**
	 * Check if block is eligible for vote generation, then generates and broadcasts a new vote or broadcasts votes already in cache
	 * @param transaction : needs `tables::final_votes` lock
	 */
	void process (nano::write_transaction const &, nano::root const &, nano::block_hash const &);
	/**
	 * Checks whether we should generate a vote for a block coming from live election
	 */
	bool should_vote (nano::write_transaction const &, nano::root const &, nano::block_hash const &);

private: // Dependencies
	nano::node_config const & config;
	nano::ledger & ledger;
	nano::store & store;
	nano::wallets & wallets;
	nano::vote_processor & vote_processor;
	nano::local_vote_history & history;
	nano::network & network;
	nano::stat & stats;

private:
	nano::vote_spacing spacing;
	nano::wallet_voter voter;
	nano::broadcast_voter broadcaster;

	nano::processing_queue<broadcast_request_t> broadcast_requests;

private:
	constexpr static bool is_final = true;

public: // Container info
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name);
};

/**
 *
 */
class reply_vote_generator final
{
private:
	using candidate_t = std::pair<nano::root, nano::block_hash>;
	using reply_request_t = std::pair<std::vector<candidate_t>, std::shared_ptr<nano::transport::channel>>;

public:
	reply_vote_generator (nano::node_config const &, nano::ledger &, nano::store &, nano::wallets &, nano::vote_processor &, nano::local_vote_history &, nano::network &, nano::stat &);
	~reply_vote_generator ();

	void start ();
	void stop ();

	/**
	 * Queue candidates (<block root, block hash> pairs) for vote generation in response to `confirm_req` requests
	 * Replies with up to `nano::network::confirm_ack_hashes_max` hashes in single vote
	 * NOTE: It is responsibility of client to batch multiple vote requests in a single `confirm_req` message
	 * We do not do batching server side to minimize latency and needed preprocessing
	 */
	void request (std::vector<std::pair<nano::root, nano::block_hash>> const & candidates, std::shared_ptr<nano::transport::channel> const &);

private:
	/**
	 * Process batch of broadcast requests
	 */
	void process_batch (std::deque<reply_request_t> & batch);
	/**
	 * Checks if candidates are eligible for vote generation and returns list of votes for those candidates
	 */
	std::vector<std::shared_ptr<nano::vote>> process (nano::transaction const &, std::vector<candidate_t> const & candidates);
	/**
	 * Only reply to `confirm_req`s for blocks that are already cemented
	 */
	bool should_vote (nano::transaction const &, nano::root const &, nano::block_hash const &) const;
	/**
	 * Wraps vote into `confirm_ack` message and sends it through the channel
	 */
	void send (std::shared_ptr<nano::vote> const &, std::shared_ptr<nano::transport::channel> &);

private: // Dependencies
	nano::node_config const & config;
	nano::ledger & ledger;
	nano::store & store;
	nano::wallets & wallets;
	nano::vote_processor & vote_processor;
	nano::local_vote_history & history;
	nano::network & network;
	nano::stat & stats;

private:
	nano::vote_spacing spacing;
	nano::wallet_voter voter;

	nano::processing_queue<reply_request_t> reply_requests;

private:
	constexpr static bool is_final = true;

public: // Container info
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name);
};
}