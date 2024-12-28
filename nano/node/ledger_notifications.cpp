#include <nano/lib/thread_roles.hpp>
#include <nano/node/ledger_notifications.hpp>
#include <nano/node/nodeconfig.hpp>

nano::ledger_notifications::ledger_notifications (nano::node_config const & config, nano::stats & stats, nano::logger & logger) :
	config{ config },
	stats{ stats },
	logger{ logger }
{
}

nano::ledger_notifications::~ledger_notifications ()
{
	debug_assert (!thread.joinable ());
}

void nano::ledger_notifications::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::ledger_notifications);
		run ();
	} };
}

void nano::ledger_notifications::stop ()
{
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::ledger_notifications::wait (std::function<void ()> cooldown_action)
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	condition.wait (lock, [this, &cooldown_action] {
		bool predicate = stopped || notifications.size () < config.max_ledger_notifications;
		if (!predicate && cooldown_action)
		{
			cooldown_action ();
		}
		return predicate;
	});
}

// Write transaction is ignored, it's passed to ensure that notifications are dispatched when still holding the database write lock
void nano::ledger_notifications::notify_processed (nano::store::write_transaction const &, processed_batch_t processed, std::function<void ()> callback)
{
	// Components should cooperate to ensure that the batch size is within the limit
	debug_assert (processed.size () <= config.max_ledger_notifications * 2);
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		notifications.push_back (nano::wrap_move_only ([this, processed = std::move (processed), callback = std::move (callback)] () mutable {
			stats.inc (nano::stat::type::ledger_notifications, nano::stat::detail::notify_processed);

			// Set results for futures when not holding the lock
			for (auto & [result, context] : processed)
			{
				if (context.callback)
				{
					context.callback (result);
				}
				context.set_result (result);
			}

			batch_processed.notify (processed);

			if (callback)
			{
				callback ();
			}
		}));
	}
	condition.notify_all ();
}

// Write transaction is ignored, it's passed to ensure that notifications are dispatched when still holding the database write lock
void nano::ledger_notifications::notify_rolled_back (nano::store::write_transaction const &, rolled_back_batch_t batch, nano::qualified_root rollback_root, std::function<void ()> callback)
{
	// Components should cooperate to ensure that the batch size is within the limit
	debug_assert (batch.size () <= config.max_ledger_notifications * 2);
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		notifications.push_back (nano::wrap_move_only ([this, batch = std::move (batch), rollback_root, callback = std::move (callback)] () {
			stats.inc (nano::stat::type::ledger_notifications, nano::stat::detail::notify_rolled_back);

			rolled_back.notify (batch, rollback_root);

			if (callback)
			{
				callback ();
			}
		}));
	}
	condition.notify_all ();
}

void nano::ledger_notifications::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait (lock, [this] {
			return stopped || !notifications.empty ();
		});

		if (stopped)
		{
			return;
		}

		while (!notifications.empty ())
		{
			auto notification = std::move (notifications.front ());
			notifications.pop_front ();
			lock.unlock ();

			notification ();

			condition.notify_all (); // Notify waiting threads about possible vacancy

			lock.lock ();
		}
	}
}

nano::container_info nano::ledger_notifications::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	nano::container_info info;
	info.put ("notifications", notifications.size ());
	return info;
}