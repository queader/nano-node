#pragma once

#include <nano/node/common.hpp>
#include <nano/node/transport/transport.hpp>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/optional.hpp>

#include <chrono>
#include <memory>
#include <unordered_set>

namespace mi = boost::multi_index;

namespace nano
{
class node;

/**
 * A representative picked up during repcrawl.
 */
class representative
{
public:
	representative () = default;
	representative (nano::account account_a, nano::amount weight_a, nano::account node_id_a) :
		account (account_a),
		weight (weight_a),
		node_id{ node_id_a }
	{
	}
	bool operator== (nano::representative const & other_a) const
	{
		return account == other_a.account;
	}
	nano::account account{};
	nano::amount weight{ 0 };
	nano::account node_id{};
	std::chrono::steady_clock::time_point last_request{ std::chrono::steady_clock::time_point () };
	std::chrono::steady_clock::time_point last_response{ std::chrono::steady_clock::time_point () };
};

/**
 * Crawls the network for representatives. Queries are performed by requesting confirmation of a
 * random block and observing the corresponding vote.
 */
class rep_crawler final
{
	friend std::unique_ptr<container_info_component> collect_container_info (rep_crawler & rep_crawler, std::string const & name);

	// clang-format off
	class tag_index {};
	class tag_last_request {};
	class tag_weight {};
	class tag_node_id {};

	using probably_rep_t = boost::multi_index_container<representative,
	mi::indexed_by<
		mi::random_access<>,
		/*
		 * It is possible for single node id to host multiple representative accounts
		 * and it is possible for single representative to be visible at multiple node ids (man in the middle)
		 * Therefore keep track of that with a unique account-node_id index composite key
		 */
		mi::hashed_unique<mi::tag<tag_index>,
			mi::composite_key<representative,
				mi::member<representative, nano::account, &representative::account>,
				mi::member<representative, nano::account, &representative::node_id>
			>>,
		mi::ordered_non_unique<mi::tag<tag_last_request>,
			mi::member<representative, std::chrono::steady_clock::time_point, &representative::last_request>>,
		mi::ordered_non_unique<mi::tag<tag_weight>,
			mi::member<representative, nano::amount, &representative::weight>, std::greater<nano::amount>>,
		mi::hashed_non_unique<mi::tag<tag_node_id>,
			mi::member<representative, nano::account, &representative::node_id>>
	>>;
	// clang-format on

public:
	rep_crawler (nano::node & node_a);

	/** Start crawling */
	void start ();

	/** Remove block hash from list of active rep queries */
	void remove (nano::block_hash const &);

	/** Remove block hash from with delay depending on vote processor size */
	void throttled_remove (nano::block_hash const &, uint64_t const);

	/** Attempt to determine if the peer manages one or more representative accounts */
	void query (std::vector<std::shared_ptr<nano::transport::channel>> const & channels_a);

	/** Attempt to determine if the peer manages one or more representative accounts */
	void query (std::shared_ptr<nano::transport::channel> const & channel_a);

	/** Query if a peer manages a principle representative */
	bool is_pr (nano::transport::channel const &) const;

	/**
	 * Called when a non-replay vote on a block previously sent by query() is received. This indicates
	 * with high probability that the endpoint is a representative node.
	 * @return false if the vote corresponded to any active hash.
	 */
	bool response (std::shared_ptr<nano::transport::channel> const &, std::shared_ptr<nano::vote> const &);

	/** Get total available weight from representatives */
	nano::uint128_t total_weight () const;

	/** Request a list of the top \p count_a known representatives in descending order of weight, with at least \p weight_a voting weight, and optionally with a minimum version \p opt_version_min_a */
	std::vector<representative> representatives (std::size_t count_a = std::numeric_limits<std::size_t>::max (), nano::uint128_t const weight_a = 0, boost::optional<decltype (nano::network_constants::protocol_version)> const & opt_version_min_a = boost::none);

	/** Request a list of the top \p count_a known principal representatives in descending order of weight, optionally with a minimum version \p opt_version_min_a */
	std::vector<representative> principal_representatives (std::size_t count_a = std::numeric_limits<std::size_t>::max (), boost::optional<decltype (nano::network_constants::protocol_version)> const & opt_version_min_a = boost::none);

	/** Total number of representatives */
	std::size_t representative_count ();

private:
	nano::node & node;

	/** Protects the active-hash container */
	nano::mutex active_mutex;

	/** We have solicted votes for these random blocks */
	std::unordered_set<nano::block_hash> active;

	// Validate responses to see if they're reps
	void process_responses ();
	void process_response (std::shared_ptr<nano::transport::channel> const & channel, std::shared_ptr<nano::vote> const & vote);
	bool insert_rep (nano::account rep_account, nano::amount rep_weight, nano::account rep_node_id);

	/** Called continuously to crawl for representatives */
	void ongoing_crawl ();

	/** Returns a list of endpoints to crawl. The total weight is passed in to avoid computing it twice. */
	std::vector<std::shared_ptr<nano::transport::channel>> get_crawl_targets (nano::uint128_t total_weight_a);

	/** When a rep request is made, this is called to update the last-request timestamp. */
	void on_rep_request (std::shared_ptr<nano::transport::channel> const & channel_a);

	/** Clean representatives with inactive channels */
	void cleanup_reps ();
	void cleanup_reps_expired_channel ();
	void cleanup_reps_expired_reponse ();

	/** Update representatives weights from ledger */
	void update_weights ();

	/** Protects the probable_reps container */
	mutable nano::mutex probable_reps_mutex;

	/** Probable representatives */
	probably_rep_t probable_reps;

	friend class active_transactions_confirm_election_by_request_Test;
	friend class active_transactions_confirm_frontier_Test;
	friend class rep_crawler_local_Test;
	friend class node_online_reps_rep_crawler_Test;

	std::deque<std::pair<std::shared_ptr<nano::transport::channel>, std::shared_ptr<nano::vote>>> responses;
};
}
