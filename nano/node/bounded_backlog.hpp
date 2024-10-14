#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/observer_set.hpp>
#include <nano/lib/rate_limiting.hpp>
#include <nano/node/bucketing.hpp>
#include <nano/node/fwd.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

namespace mi = boost::multi_index;

namespace nano
{
class backlog_index
{
public:
	struct key
	{
		nano::bucket_index bucket;
		nano::priority_timestamp priority;

		auto operator<=> (key const &) const = default;
	};

	struct entry
	{
		nano::account account;
		nano::bucket_index bucket;
		nano::priority_timestamp priority;
		nano::block_hash head;
		uint64_t unconfirmed;

		backlog_index::key key () const
		{
			return { bucket, priority };
		}
	};

	using value_type = entry;

public:
	backlog_index () = default;

	void update (nano::account const & account, nano::block_hash const & head, nano::bucket_index, nano::priority_timestamp, uint64_t unconfirmed);
	bool erase (nano::account const & account);
	nano::block_hash head (nano::account const & account) const;

	using filter_t = std::function<bool (nano::block_hash const &)>;
	std::deque<value_type> top (nano::bucket_index, size_t count, filter_t const &) const;

	std::deque<value_type> next (nano::account const & start, size_t count) const;

	uint64_t unconfirmed (nano::bucket_index) const;
	uint64_t backlog_size () const;

	nano::container_info container_info () const;

private:
	// clang-format off
	class tag_account {};
	class tag_key {};

	using ordered_accounts = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::ordered_unique<mi::tag<tag_account>,
			mi::member<entry, nano::account, &entry::account>>,
		mi::ordered_non_unique<mi::tag<tag_key>,
			mi::const_mem_fun<entry, key, &entry::key>, std::greater<>> // DESC order
	>>;
	// clang-format on

	ordered_accounts accounts;

	// Keep track of the size of the backlog in number of unconfirmed blocks per bucket
	std::map<nano::bucket_index, int64_t> unconfirmed_by_bucket;
	std::map<nano::bucket_index, size_t> size_by_bucket;

	// Keep track of the backlog size in number of unconfirmed blocks
	int64_t backlog_counter{ 0 };
};

class bounded_backlog_config
{
public:
	size_t max_backlog{ 100000 };
	size_t bucket_threshold{ 1000 };
	double overfill_factor{ 1.5 };
	size_t batch_size{ 128 };
};

class bounded_backlog
{
public:
	bounded_backlog (bounded_backlog_config const &, nano::node &, nano::ledger &, nano::bucketing &, nano::backlog_scan &, nano::block_processor &, nano::confirming_set &, nano::stats &, nano::logger &);
	~bounded_backlog ();

	void start ();
	void stop ();

	uint64_t backlog_size () const;

	nano::container_info container_info () const;

public: // Events
	nano::observer_set<std::deque<std::shared_ptr<nano::block>> const &> rolled_back;

private: // Dependencies
	bounded_backlog_config const & config;
	nano::node & node;
	nano::ledger & ledger;
	nano::bucketing & bucketing;
	nano::backlog_scan & backlog_scan;
	nano::block_processor & block_processor;
	nano::confirming_set & confirming_set;
	nano::stats & stats;
	nano::logger & logger;

private:
	using rollback_target = std::pair<nano::account, nano::block_hash>;

	bool update (nano::secure::transaction const &, nano::account const &);
	bool activate (nano::secure::transaction const &, nano::account const &, nano::account_info const &, nano::confirmation_height_info const &);
	bool erase (nano::secure::transaction const &, nano::account const &);

	bool predicate () const;
	void run ();
	void perform_rollbacks (std::deque<rollback_target> const & targets);
	std::deque<rollback_target> gather_targets (size_t max_count) const;
	bool should_rollback (nano::block_hash const &) const;

	void run_scan ();

	nano::amount block_priority_balance (nano::secure::transaction const &, nano::block const &) const;
	nano::priority_timestamp block_priority_timestamp (nano::secure::transaction const &, nano::block const &) const;

private:
	nano::backlog_index index;

	nano::rate_limiter scan_limiter;

	std::atomic<bool> stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;
	std::thread scan_thread;
};
}