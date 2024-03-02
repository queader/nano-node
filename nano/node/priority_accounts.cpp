#include <nano/lib/blocks.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/backlog_population.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/priority_accounts.hpp>
#include <nano/node/scheduler/priority.hpp>
#include <nano/store/component.hpp>

nano::priority_accounts::priority_accounts (nano::node & node) :
	node{ node }
{
}

nano::priority_accounts::~priority_accounts ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::priority_accounts::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::priority_accounts);
		run ();
	} };
}

void nano::priority_accounts::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	nano::join_or_pass (thread);
}

bool nano::priority_accounts::is_priority (nano::account account) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.contains (account);
}

std::unique_ptr<nano::container_info_component> nano::priority_accounts::collect_container_info (std::string const & name) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "accounts", accounts.size (), sizeof (decltype (accounts)::value_type) }));
	return composite;
}

void nano::priority_accounts::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		node.stats.inc (nano::stat::type::priority_accounts, nano::stat::detail::loop);

		lock.unlock ();
		run_iteration ();
		lock.lock ();

		warmed_up = true;
	}
}

void nano::priority_accounts::run_iteration ()
{
	size_t constexpr batch_size = 1024 * 32;

	bool done = false;
	nano::account next = 0;
	while (!stopped && !done)
	{
		{
			auto transaction = node.store.tx_begin_read ();

			size_t count = 0;
			auto i = node.store.account.begin (transaction, next);
			auto const end = node.store.account.end ();
			for (; i != end && count < batch_size; ++i, ++count)
			{
				transaction.refresh_if_needed ();

				node.stats.inc (nano::stat::type::priority_accounts, nano::stat::detail::total);

				auto const account = i->first;
				auto const & info = i->second;
				activate (transaction, account, info);

				next = account.number () + 1;
			}
			done = node.store.account.begin (transaction, next) == end;
		}

		// Give the rest of the node time to progress without holding database lock
		nano::unique_lock<nano::mutex> lock{ mutex };
		condition.wait_for (lock, std::chrono::milliseconds{ warmed_up ? 100 : 10 }); // TODO: Adjust
	}
}

void nano::priority_accounts::activate (nano::store::transaction const & transaction, nano::account const & account, nano::account_info const & account_info)
{
	nano::confirmation_height_info conf_info{};
	node.store.confirmation_height.get (transaction, account, conf_info);

	bool error = false; // Ignore
	auto const head_balance = node.ledger.balance (transaction, account_info.head).value_or (0);

	auto const head_block = node.ledger.head_block (transaction, account);
	auto const previous_balance = node.ledger.balance (transaction, head_block->previous ()).value_or (0);

	auto const conf_balance = node.ledger.balance (transaction, conf_info.frontier).value_or (0);

	auto const balance = std::max (head_balance, std::max (previous_balance, conf_balance));

	nano::lock_guard<nano::mutex> lock{ mutex };

	if (balance >= balance_threshold)
	{
		node.stats.inc (nano::stat::type::priority_accounts, nano::stat::detail::activated);

		accounts.insert ({ account, balance });

		while (accounts.size () > max_size)
		{
			node.stats.inc (nano::stat::type::priority_accounts, nano::stat::detail::overfill);

			accounts.get<tag_balance> ().erase (accounts.get<tag_balance> ().begin ());
		}
	}
	else
	{
		auto erased = accounts.erase (account);
		if (erased > 0)
		{
			node.stats.inc (nano::stat::type::priority_accounts, nano::stat::detail::erased);
		}
	}
}