#include "bootstrap_prioritization.hpp"

#include <nano/lib/locks.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/node.hpp>
#include <nano/secure/store.hpp>

nano::bootstrap_prioritization::bootstrap_prioritization (nano::node & node_a) :
	node (node_a),
	thread_lazy_bootsrap{ [this] () { run_lazy_bootstrap (); } },
	thread_initialize{ [this] () { run_initialize (); } }
{
}

nano::bootstrap_prioritization::~bootstrap_prioritization ()
{
	thread_lazy_bootsrap.join ();
	thread_initialize.join ();
}

// void nano::bootstrap_prioritization::stop()
//{
//	nano::unique_lock<nano::mutex> lock{ mutex };
//	if (!stopped)
//	{
//		stopped = true;
//		condition.notify_all (); // Notify flush (), run ()
//	}
// }

void nano::bootstrap_prioritization::queue_send (nano::transaction const & transaction, nano::account const & origin_account, nano::account const & destination_account, nano::amount const & balance, bool force)
{
	if (destination_account.is_zero ())
	{
		return;
	}

	if (force || !node.store.account.exists (transaction, destination_account))
	{
		debug_assert (force || priority_map.count (origin_account) > 0);
		priority_value priority = force ? 0 : (priority_map.at (origin_account) + 1);
		priority_map.insert_or_assign (destination_account, priority);

		nano::unique_lock<nano::mutex> lock{ mutex };
		missing.emplace (value_type{ balance, destination_account, priority, force });
		done_accounts.emplace (destination_account);
	}
}

void nano::bootstrap_prioritization::queue_receive (nano::transaction const & transaction, nano::account const & origin_account, nano::account const & source_account, nano::amount const & balance, bool force)
{
	if (source_account.is_zero ())
	{
		return;
	}

	if (force || !node.store.account.exists (transaction, source_account))
	{
		priority_value priority = force ? 0 : (priority_map.count (origin_account) == 0 ? std::numeric_limits<priority_value>::max () : priority_map.at (origin_account));

		nano::unique_lock<nano::mutex> lock{ mutex };
		missing.emplace (value_type{ balance, source_account, priority, force });
		done_accounts.emplace (source_account);
	}
}

nano::bootstrap_prioritization::value_type nano::bootstrap_prioritization::top ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	if (!missing.empty ())
	{
		auto it = missing.begin ();
		auto result = *it;
		missing.erase (it);

		return result;
	}

	return nano::bootstrap_prioritization::value_type{ 0, 0, 0, false };
}

void nano::bootstrap_prioritization::requeue (nano::bootstrap_prioritization::value_type entry)
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	missing.emplace (entry);
}

bool nano::bootstrap_prioritization::exists (nano::account const & account)
{
	auto transaction = node.store.tx_begin_read ();

	return node.store.account.exists (transaction, account);
}

void nano::bootstrap_prioritization::run_lazy_bootstrap ()
{
	if (node.flags.inactive_node)
	{
		return;
	}

	node.node_initialized_latch.wait ();

	int ctr = 0;

	while (!node.stopped)
	{
		bool inserted = false;

		auto entry = top ();
		auto account = entry.account;

		if (!entry.forced && exists (account))
		{
			continue;
		}

		if (!account.is_zero ())
		{
			inserted = node.bootstrap_initiator.bootstrap_lazy (account, false, false);
			if (inserted)
			{
				node.logger.always_log (boost::format ("Bootstrap prioritized: %1%") % ++ctr);
			}
			else
			{
				requeue (entry);
				node.logger.always_log (boost::format ("Bootstrap prioritization lazy queue full"));
			}
		}
		else
		{
			node.logger.always_log (boost::format ("Bootstrap prioritization empty"));
		}

		if (!inserted)
		{
			check_status ();
			std::this_thread::sleep_for (std::chrono::seconds (3));
		}

		//		if (inserted)
		//		{
		//			return;
		//		}
	}
}

void nano::bootstrap_prioritization::check_status ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	auto transaction = node.store.tx_begin_read ();

	int ctr = 0;
	for (const auto & account : done_accounts)
	{
		if (node.store.account.exists (transaction, account))
		{
			ctr++;
		}
	}

	node.logger.always_log (boost::format ("Bootstrap prioritization accounts %1% / %2%") % ctr % done_accounts.size ());
}

void nano::bootstrap_prioritization::run_initialize ()
{
	if (node.flags.inactive_node)
	{
		return;
	}

	node.node_initialized_latch.wait ();

	{
		auto transaction = node.store.tx_begin_read ();
		auto genesis = node.network_params.ledger.genesis->account ();
		queue_send (transaction, genesis, genesis, 0, true);
	}
}

bool nano::bootstrap_prioritization::value_type::operator<(const nano::bootstrap_prioritization::value_type & other) const
{
	return priority == other.priority ? (amount == other.amount ? account < other.account : amount < other.amount) : priority > other.priority;
	//	return amount == other.amount ? account < other.account : amount < other.amount;
}

bool nano::bootstrap_prioritization::value_type::operator== (const nano::bootstrap_prioritization::value_type & other) const
{
	return priority == other.priority && amount == other.amount && account == other.account;
	//	return amount == other.amount && account == other.account;
}
