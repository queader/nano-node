#pragma once

#include <nano/node/bandwidth_limiter.hpp>
#include <nano/node/bootstrap_ascending/account_sets.hpp>
#include <nano/node/bootstrap_ascending/common.hpp>
#include <nano/node/bootstrap_ascending/iterators.hpp>
#include <nano/node/bootstrap_ascending/throttle.hpp>

namespace nano
{
class block_processor;
class ledger;
class network;
}

namespace nano::bootstrap_ascending
{
class service;

class account_scan final
{
public:
	class tag : public nano::bootstrap_ascending::tag_base<tag, nano::asc_pull_ack::blocks_payload>
	{
	public:
		enum class query_type
		{
			blocks_by_hash,
			blocks_by_account,
		};

		const nano::account account{};
		nano::hash_or_account start{};
		query_type type{};

		enum class verify_result
		{
			ok,
			nothing_new,
			invalid,
		};

		verify_result verify (nano::asc_pull_ack::blocks_payload const & response) const;
	};

public:
	account_scan (bootstrap_ascending_config const &, service &, nano::ledger &, nano::network_constants &, nano::block_processor &, nano::stats &);
	~account_scan ();

	void start ();
	void stop ();

	nano::asc_pull_req::payload_variant prepare (account_scan::tag &);
	void process (nano::asc_pull_ack::blocks_payload const & response, account_scan::tag const &);
	void cleanup ();

	std::size_t blocked_size () const;
	std::size_t priority_size () const;

private: // Dependencies
	bootstrap_ascending_config const & config;
	service & service;
	nano::ledger & ledger;
	nano::network_constants & network_consts;
	nano::block_processor & block_processor;
	nano::stats & stats;

private:
	void run ();
	void run_one ();

	/* Inspects a block that has been processed by the block processor */
	void inspect (store::transaction const &, nano::process_return const & result, nano::block const & block);

	/* Waits until a suitable account outside of cool down period is available */
	nano::account available_account ();
	nano::account wait_available_account ();

	/* Throttles requesting new blocks, not to overwhelm blockprocessor */
	void wait_blockprocessor ();

	void throttle_if_needed (nano::unique_lock<nano::mutex> & lock);
	// Calculates a lookback size based on the size of the ledger where larger ledgers have a larger sample count
	std::size_t compute_throttle_size () const;

private:
	nano::bootstrap_ascending::account_sets accounts;
	nano::bootstrap_ascending::buffered_iterator iterator;
	nano::bootstrap_ascending::throttle throttle;

	// Requests for accounts from database have much lower hitrate and could introduce strain on the network
	// A separate (lower) limiter ensures that we always reserve resources for querying accounts from priority queue
	nano::bandwidth_limiter database_limiter;

	bool stopped{ false };
	mutable nano::mutex mutex;
	mutable nano::condition_variable condition;
	std::thread thread;
};
}