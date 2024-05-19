#include <nano/lib/blocks.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/election.hpp>
#include <nano/node/node.hpp>
#include <nano/node/scheduler/bucket.hpp>
#include <nano/node/scheduler/priority.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_confirmed.hpp>

nano::scheduler::priority::priority (nano::node & node_a, nano::stats & stats_a) :
	config{ node_a.config.priority_scheduler },
	node{ node_a },
	stats{ stats_a }
{
	std::vector<nano::uint128_t> minimums;

	auto build_region = [&minimums] (uint128_t const & begin, uint128_t const & end, size_t count) {
		auto width = (end - begin) / count;
		for (auto i = 0; i < count; ++i)
		{
			minimums.push_back (begin + i * width);
		}
	};

	minimums.push_back (uint128_t{ 0 });
	build_region (uint128_t{ 1 } << 88, uint128_t{ 1 } << 92, 2);
	build_region (uint128_t{ 1 } << 92, uint128_t{ 1 } << 96, 4);
	build_region (uint128_t{ 1 } << 96, uint128_t{ 1 } << 100, 8);
	build_region (uint128_t{ 1 } << 100, uint128_t{ 1 } << 104, 16);
	build_region (uint128_t{ 1 } << 104, uint128_t{ 1 } << 108, 16);
	build_region (uint128_t{ 1 } << 108, uint128_t{ 1 } << 112, 8);
	build_region (uint128_t{ 1 } << 112, uint128_t{ 1 } << 116, 4);
	build_region (uint128_t{ 1 } << 116, uint128_t{ 1 } << 120, 2);
	minimums.push_back (uint128_t{ 1 } << 120);

	auto const bucket_max = std::max<size_t> (1u, config.max_blocks / minimums.size ());

	node.logger.info (nano::log::type::priority_scheduler, "Number of buckets: {} (blocks per bucket: {})", minimums.size (), bucket_max);

	for (size_t i = 0u, n = minimums.size (); i < n; ++i)
	{
		auto bucket = std::make_unique<nano::scheduler::bucket> (bucket_max, minimums[i], i);
		buckets.emplace_back (std::move (bucket));
	}
}

nano::scheduler::priority::~priority ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::scheduler::priority::start ()
{
	debug_assert (!thread.joinable ());

	if (!config.enabled)
	{
		return;
	}

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::scheduler_priority);
		run ();
	} };
}

void nano::scheduler::priority::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	notify ();
	nano::join_or_pass (thread);
}

bool nano::scheduler::priority::activate (secure::transaction const & transaction, nano::account const & account)
{
	debug_assert (!account.is_zero ());
	auto head = node.ledger.confirmed.account_head (transaction, account);
	if (node.ledger.any.account_head (transaction, account) == head)
	{
		return false;
	}
	auto block = node.ledger.any.block_get (transaction, node.ledger.any.block_successor (transaction, { head.is_zero () ? static_cast<nano::uint256_union> (account) : head, head }).value ());
	if (!node.ledger.dependents_confirmed (transaction, *block))
	{
		return false;
	}
	auto const balance_priority = std::max (block->balance ().number (), node.ledger.confirmed.block_balance (transaction, head).value_or (0).number ());
	auto const time_priority = !head.is_zero () ? node.ledger.confirmed.block_get (transaction, head)->sideband ().timestamp : nano::seconds_since_epoch (); // New accounts get current timestamp i.e. lowest priority

	node.stats.inc (nano::stat::type::priority_scheduler, nano::stat::detail::activated);
	node.logger.trace (nano::log::type::priority_scheduler, nano::log::detail::block_activated,
	nano::log::arg{ "account", account.to_account () }, // TODO: Convert to lazy eval
	nano::log::arg{ "block", block },
	nano::log::arg{ "time", time_priority },
	nano::log::arg{ "priority", balance_priority });

	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		bucket (balance_priority).push (block, time_priority);
	}
	condition.notify_all ();

	return true; // Activated
}

void nano::scheduler::priority::notify ()
{
	condition.notify_all ();
}

std::size_t nano::scheduler::priority::size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	return std::accumulate (buckets.begin (), buckets.end (), std::size_t{ 0 }, [] (auto const & sum, auto const & bucket) {
		return sum + bucket->size ();
	});
}

bool nano::scheduler::priority::empty () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	return std::all_of (buckets.begin (), buckets.end (), [] (auto const & bucket) {
		return bucket->empty ();
	});
}

bool nano::scheduler::priority::predicate () const
{
	debug_assert (!mutex.try_lock ());

	return std::any_of (buckets.begin (), buckets.end (), [this] (auto const & bucket) {
		return available (*bucket);
	});
}

bool nano::scheduler::priority::available (nano::scheduler::bucket const & bucket) const
{
	debug_assert (!mutex.try_lock ());

	if (bucket.empty ())
	{
		return false;
	}

	auto const bucket_info = node.active.info (nano::election_behavior::priority, bucket.index);

	if (bucket_info.election_count < config.elections_reserved)
	{
		return true;
	}
	if (bucket_info.election_count < config.elections_max)
	{
		return node.active.vacancy (nano::election_behavior::priority) > 0;
	}
	// Check if the top election in the bucket should be reprioritized
	if (bucket_info.top_election)
	{
		auto [candidate_block, candidate_time] = bucket.top ();

		if (bucket_info.top_election->qualified_root == candidate_block->qualified_root ())
		{
			return true; // Drain duplicates
		}
		if (candidate_time < bucket_info.top_priority)
		{
			// Bound number of reprioritizations
			return bucket_info.election_count < config.elections_max * 2;
		};
	}
	return false;
}

void nano::scheduler::priority::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		stats.inc (nano::stat::type::priority_scheduler, nano::stat::detail::loop);
		debug_assert ((std::this_thread::yield (), true)); // Introduce some random delay in debug builds

		if (predicate ())
		{
			for (auto & bucket : buckets)
			{
				if (available (*bucket))
				{
					auto [block, time] = bucket->top ();
					bucket->pop ();

					lock.unlock ();

					auto result = node.active.insert (block);
					stats.inc (nano::stat::type::priority_scheduler, result.inserted ? nano::stat::detail::insert_success : nano::stat::detail::insert_failed);

					lock.lock ();
				}
			}
		}
		else
		{
			condition.wait (lock, [this] () { return stopped || predicate (); });
		}
	}
}

nano::scheduler::bucket & nano::scheduler::priority::bucket (nano::uint128_t balance) const
{
	debug_assert (!mutex.try_lock ());

	auto it = std::upper_bound (buckets.begin (), buckets.end (), balance, [] (nano::uint128_t const & priority, std::unique_ptr<nano::scheduler::bucket> const & bucket) {
		return priority < bucket->min_balance;
	});
	release_assert (it != buckets.begin ()); // There should always be a bucket with a minimum_balance of 0
	--it;
	return **it;
}

std::unique_ptr<nano::container_info_component> nano::scheduler::priority::collect_container_info (std::string const & name)
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	auto collect_blocks = [&] () {
		auto composite = std::make_unique<container_info_composite> ("blocks");
		for (auto i = 0; i < buckets.size (); ++i)
		{
			auto const & bucket = buckets[i];
			composite->add_component (std::make_unique<container_info_leaf> (container_info{ std::to_string (i), bucket->size (), 0 }));
		}
		return composite;
	};

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (collect_blocks ());
	return composite;
}
