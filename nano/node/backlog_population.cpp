#include <nano/lib/stats.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/backlog_population.hpp>
#include <nano/node/election_scheduler.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/secure/store.hpp>

nano::backlog_population::backlog_population (const config config_a, nano::store & store_a, nano::election_scheduler & scheduler_a, nano::stat & stats_a) :
	config_m{ config_a },
	store{ store_a },
	scheduler{ scheduler_a },
	stats{ stats_a }
{
}

nano::backlog_population::~backlog_population ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::backlog_population::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::backlog_population);
		run ();
	} };
}

void nano::backlog_population::stop ()
{
	stopped = true;
	notify ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::backlog_population::trigger ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	triggered = true;
	notify ();
}

void nano::backlog_population::notify ()
{
	condition.notify_all ();
}

bool nano::backlog_population::predicate () const
{
	return triggered || overflown;
}

void nano::backlog_population::run ()
{
	const auto delay = std::chrono::seconds{ config_m.delay_between_runs_seconds };
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		// Run either when predicate is triggered or when delay between runs passed
		if (predicate () || config_m.ongoing_backlog_population_enabled)
		{
			stats.inc (nano::stat::type::backlog, nano::stat::detail::loop);

			triggered = false;
			lock.unlock ();
			bool over = populate_backlog ();
			lock.lock ();
			overflown = over;
		}

		condition.wait_for (lock, delay, [this] () {
			return stopped || predicate ();
		});
	}
}

bool nano::backlog_population::populate_backlog ()
{
	bool overflow = false;

	auto done = false;
	uint64_t const chunk_size = 65536;
	nano::account next = { 0 }; // Start from the beginning
	uint64_t total = 0;
	while (!stopped && !done)
	{
		auto transaction = store.tx_begin_read ();
		auto count = 0;
		auto i = store.account.begin (transaction, next);
		const auto end = store.account.end ();
		for (; !stopped && i != end && count < chunk_size; ++i, ++count, ++total)
		{
			stats.inc (nano::stat::type::backlog, nano::stat::detail::total);

			auto const & account = i->first;
			auto [activated, over] = scheduler.activate (account, transaction);
			if (activated)
			{
				stats.inc (nano::stat::type::backlog, nano::stat::detail::activated);
			}
			if (over)
			{
				overflow = true;
				stats.inc (nano::stat::type::backlog, nano::stat::detail::overflow);
			}

			next = account.number () + 1;
		}
		done = store.account.begin (transaction, next) == end;
	}

	return overflow;
}
