#include <nano/lib/threading.hpp>
#include <nano/node/backlog_population.hpp>
#include <nano/node/election_scheduler.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/secure/store.hpp>

nano::backlog_population::backlog_population (const config & config_a, nano::store & store_a, nano::election_scheduler & scheduler_a) :
	config_m{ config_a },
	store_m{ store_a },
	scheduler{ scheduler_a }
{
}

nano::backlog_population::~backlog_population ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::backlog_population::start ()
{
	if (!thread.joinable ())
	{
		thread = std::thread{ [this] () { run (); } };
	}
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
	// In addition to the backlog population happening in fixed intervals (5 minutes by default)
	// it can be either triggered manually (via RPC command `populate_backlog`)
	// or if the last backlog population resulted in overflown priority queue
	return triggered || overflown;
}

void nano::backlog_population::run ()
{
	nano::thread_role::set (nano::thread_role::name::backlog_population);
	const auto delay = std::chrono::seconds{ config_m.delay_between_runs_in_seconds };
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		if (predicate () || config_m.ongoing_backlog_population_enabled)
		{
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
	nano::account next = 0;
	uint64_t total = 0;
	while (!stopped && !done)
	{
		auto transaction = store_m.tx_begin_read ();
		auto count = 0;
		auto i = store_m.account.begin (transaction, next);
		const auto end = store_m.account.end ();
		for (; !stopped && i != end && count < chunk_size; ++i, ++count, ++total)
		{
			auto const & account = i->first;
			bool over = scheduler.activate (account, transaction);
			if (over)
			{
				overflow = true;
			}

			next = account.number () + 1;
		}
		done = store_m.account.begin (transaction, next) == end;
	}

	return overflow;
}
