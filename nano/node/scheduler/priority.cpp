#include <nano/node/node.hpp>
#include <nano/node/scheduler/bucket.hpp>
#include <nano/node/scheduler/buckets.hpp>
#include <nano/node/scheduler/priority.hpp>

nano::scheduler::priority::priority (nano::node & node_a, nano::stats & stats_a) :
	node{ node_a },
	stats{ stats_a },
	buckets{}
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

	const int reserved_elections = node.config.active_elections_size / minimums.size ();

	node.logger.info (nano::log::type::election_scheduler, "Number of buckets: {}", minimums.size ());
	node.logger.info (nano::log::type::election_scheduler, "Reserved elections per bucket: {}", reserved_elections);

	for (size_t i = 0u, n = minimums.size (); i < n; ++i)
	{
		auto bucket = std::make_unique<scheduler::bucket> (minimums[i], reserved_elections, node.active);
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

bool nano::scheduler::priority::activate (nano::account const & account_a, store::transaction const & transaction)
{
	debug_assert (!account_a.is_zero ());
	auto info = node.ledger.account_info (transaction, account_a);
	if (info)
	{
		nano::confirmation_height_info conf_info;
		node.store.confirmation_height.get (transaction, account_a, conf_info);
		if (conf_info.height < info->block_count)
		{
			debug_assert (conf_info.frontier != info->head);
			auto hash = conf_info.height == 0 ? info->open_block : node.store.block.successor (transaction, conf_info.frontier);
			auto block = node.store.block.get (transaction, hash);
			debug_assert (block != nullptr);
			if (node.ledger.dependents_confirmed (transaction, *block))
			{
				auto const balance = node.ledger.balance (transaction, hash);
				auto const previous_balance = node.ledger.balance (transaction, conf_info.frontier);
				auto const balance_priority = std::max (balance, previous_balance);

				node.stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::activated);
				node.logger.trace (nano::log::type::election_scheduler, nano::log::detail::block_activated,
				nano::log::arg{ "account", account_a.to_account () }, // TODO: Convert to lazy eval
				nano::log::arg{ "block", block },
				nano::log::arg{ "time", info->modified },
				nano::log::arg{ "priority", balance_priority });

				{
					nano::lock_guard<nano::mutex> lock{ mutex };
					auto & bucket = find_bucket (balance_priority);
					bucket.push (info->modified, block);
				}

				notify ();

				return true; // Activated
			}
		}
	}
	return false; // Not activated
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
	return size () == 0;
}

bool nano::scheduler::priority::predicate () const
{
	return std::any_of (buckets.begin (), buckets.end (), [] (auto const & bucket) {
		return bucket->available ();
	});
}

void nano::scheduler::priority::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait (lock, [this] () {
			return stopped || predicate ();
		});
		debug_assert ((std::this_thread::yield (), true)); // Introduce some random delay in debug builds
		if (!stopped)
		{
			stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::loop);

			for (auto & bucket : buckets)
			{
				if (bucket->available ())
				{
					bucket->activate ();
				}
			}

			//				auto block = buckets.top ();
			//				buckets.pop ();
			//				lock.unlock ();
			//				stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::insert_priority);
			//				auto result = node.active.insert (block);
			//				if (result.inserted)
			//				{
			//					stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::insert_priority_success);
			//				}
			//				if (result.election != nullptr)
			//				{
			//					result.election->transition_active ();
			//				}
			//			}
			//			else
			//			{
			//				lock.unlock ();
			//			}
			//			notify ();
			//			lock.lock ();
		}
	}
}

auto nano::scheduler::priority::find_bucket (nano::uint128_t priority) -> bucket &
{
	auto it = std::upper_bound (buckets.begin (), buckets.end (), priority, [] (nano::uint128_t const & priority, std::unique_ptr<bucket> const & bucket) {
		return priority < bucket->minimum_balance;
	});
	release_assert (it != buckets.begin ()); // There should always be a bucket with a minimum_balance of 0
	it = std::prev (it);

	return **it; // TODO: Revisit this
}

std::unique_ptr<nano::container_info_component> nano::scheduler::priority::collect_container_info (std::string const & name)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	auto composite = std::make_unique<container_info_composite> (name);
	//	composite->add_component (buckets.collect_container_info ("buckets"));
	return composite;
}
