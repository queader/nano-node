#include <nano/lib/stats.hpp>
#include <nano/node/node.hpp>
#include <nano/node/optimistic_scheduler.hpp>

nano::optimistic_scheduler::optimistic_scheduler (const nano::optimistic_scheduler::config & config_a, nano::node & node_a, nano::active_transactions & active_a, nano::stat & stats_a) :
	config_m{ config_a },
	node{ node_a },
	active{ active_a },
	stats{ stats_a }
{
}

nano::optimistic_scheduler::~optimistic_scheduler ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::optimistic_scheduler::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::optimistic_scheduler);
		run ();
	} };
}

void nano::optimistic_scheduler::stop ()
{
	stopped = true;
	notify ();
	nano::join_or_pass (thread);
}

void nano::optimistic_scheduler::notify ()
{
	condition.notify_all ();
}

bool nano::optimistic_scheduler::activate (const nano::account & account, const nano::account_info & account_info, const nano::confirmation_height_info & conf_info)
{
	debug_assert (account_info.block_count >= conf_info.height);

	if (account_info.block_count - conf_info.height > config_m.optimistic_gap_threshold)
	{
		{
			stats.inc (nano::stat::type::optimistic, nano::stat::detail::activated);

			nano::unique_lock<nano::mutex> lock{ mutex };
			candidates.push_back (account);
			if (candidates.size () > max_size)
			{
				candidates.pop_front ();
			}
		}
		notify ();
		return true; // Activated
	}
	return false; // Not activated
}

bool nano::optimistic_scheduler::predicate () const
{
	return !candidates.empty () && active.vacancy_optimistic () > 0;
}

void nano::optimistic_scheduler::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		if (predicate ())
		{
			stats.inc (nano::stat::type::optimistic, nano::stat::detail::loop);

			run_one (lock);

			debug_assert (lock.owns_lock ());
		}

		condition.wait (lock, [this] () {
			return stopped || predicate ();
		});
	}
}

void nano::optimistic_scheduler::run_one (nano::unique_lock<nano::mutex> & lock)
{
	debug_assert (lock.owns_lock ());
	debug_assert (!candidates.empty ());
	debug_assert (active.vacancy_optimistic () > 0);

	auto candidate = candidates.front ();
	candidates.pop_front ();

	lock.unlock ();

	auto block = node.head_block (candidate);
	if (block)
	{
		// Ensure block is not already confirmed
		if (!node.block_confirmed_or_being_confirmed (block->hash ()))
		{
			// Try to insert it into AEC
			// We check for AEC vacancy inside our predicate
			auto result = node.active.insert_optimistic (block);

			stats.inc (nano::stat::type::optimistic, result.inserted ? nano::stat::detail::insert : nano::stat::detail::insert_failed);
		}
	}

	lock.lock ();
}
