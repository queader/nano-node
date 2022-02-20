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
	thread_lazy_bootsrap.join();
}

void nano::bootstrap_prioritization::queue (nano::transaction const & transaction_a, nano::account const & account_a, nano::amount const & amount_a, bool force)
{
	if (account_a.is_zero())
	{
		return;
	}

	if (force || (done_accounts.count (account_a) == 0) && !node.store.account.exists (transaction_a, account_a))
	{
		nano::unique_lock<nano::mutex> lock{ mutex };
		missing.emplace (value_type{ amount_a, account_a });
		done_accounts.emplace (account_a);
	}
}

nano::account nano::bootstrap_prioritization::top ()
{
	nano::account result = 0;

	nano::unique_lock<nano::mutex> lock{ mutex };
	if (!missing.empty ())
	{
		result = missing.begin ()->account;
	}

	return result;
}

void nano::bootstrap_prioritization::erase (nano::account const & account_a)
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	missing.erase (value_type{ 0, account_a });
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

		auto account = top ();
		if (!account.is_zero ())
		{
			inserted = node.bootstrap_initiator.bootstrap_lazy (account, false, false);
//			erase (account);
			if (inserted)
			{
				erase (account);
//				if (ctr++ % 1 == 0) node.logger.always_log (boost::format ("Bootstrap prioritized: %1%") % ctr);
				node.logger.always_log (boost::format ("Bootstrap prioritized: %1%") % ++ctr);
			}
			else
			{
				node.logger.always_log (boost::format ("Bootstrap prioritization lazy queue full"));
			}
		}
		else
		{
			node.logger.always_log (boost::format ("Bootstrap prioritization empty"));
		}

		if (!inserted)
		{
			std::this_thread::sleep_for (std::chrono::seconds (3));
		}

		//TEMP
//		if (inserted)
//		{
//			return;
//		}
	}
}

void nano::bootstrap_prioritization::run_initialize ()
{
	if (node.flags.inactive_node)
	{
		return;
	}

	node.node_initialized_latch.wait ();

	{
		auto transaction = node.store.tx_begin_read();
		queue (transaction, node.network_params.ledger.genesis->account(), 0, true);
	}
}

bool nano::bootstrap_prioritization::value_type::operator< (const nano::bootstrap_prioritization::value_type & other_a) const
{
	return account != other_a.account && amount < other_a.amount;
}

bool nano::bootstrap_prioritization::value_type::operator== (const nano::bootstrap_prioritization::value_type & other_a) const
{
	return account == other_a.account;
}
