#include <nano/lib/blocks.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/node/backlog_scan.hpp>
#include <nano/node/blockprocessor.hpp>
#include <nano/node/bounded_backlog.hpp>
#include <nano/node/node.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/confirmation_height.hpp>

nano::bounded_backlog::bounded_backlog (nano::bounded_backlog_config const & config_a, nano::node & node_a, nano::ledger & ledger_a, nano::bucketing & bucketing_a, nano::backlog_scan & backlog_scan_a, nano::block_processor & block_processor_a, nano::stats & stats_a, nano::logger & logger_a) :
	config{ config_a },
	node{ node_a },
	ledger{ ledger_a },
	bucketing{ bucketing_a },
	backlog_scan{ backlog_scan_a },
	block_processor{ block_processor_a },
	stats{ stats_a },
	logger{ logger_a }
{
	backlog_scan.activated.add ([this] (auto const & transaction, auto const & info) {
		activate (transaction, info.account, info.account_info, info.conf_info);
	});

	block_processor.batch_processed.add ([this] (auto const & batch) {
		auto transaction = ledger.tx_begin_read ();
		for (auto const & [result, context] : batch)
		{
			if (result == nano::block_status::progress)
			{
				auto const & block = context.block;
				update (transaction, block->account ());
			}
		}
	});

	block_processor.rolled_back.add ([this] (auto const & block) {
		auto transaction = ledger.tx_begin_read ();
		update (transaction, block->account ());
	});
}

nano::bounded_backlog::~bounded_backlog ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::bounded_backlog::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::bounded_backlog);
		run ();
	} };
}

void nano::bounded_backlog::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

uint64_t nano::bounded_backlog::backlog_size () const
{
	return backlog_counter.load ();
}

// TODO: This is a very naive implementation, it should be optimized
bool nano::bounded_backlog::update (nano::secure::transaction const & transaction, nano::account const & account)
{
	debug_assert (!account.is_zero ());

	if (auto info = ledger.any.account_get (transaction, account))
	{
		nano::confirmation_height_info conf_info;
		ledger.store.confirmation_height.get (transaction, account, conf_info);
		if (conf_info.height < info->block_count)
		{
			return activate (transaction, account, *info, conf_info);
		}
		else
		{
			return erase (transaction, account);
		}
	}
	return false; // Not activated
}

bool nano::bounded_backlog::erase (nano::secure::transaction const & transaction, nano::account const & account)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	if (auto existing = accounts.get<tag_account> ().find (account); existing != accounts.get<tag_account> ().end ())
	{
		backlog_counter -= existing->unconfirmed;
		accounts.get<tag_account> ().erase (existing);
		return true;
	}
	return false; // Nothing to erase
}

bool nano::bounded_backlog::activate (nano::secure::transaction const & transaction, nano::account const & account, nano::account_info const & account_info, nano::confirmation_height_info const & conf_info)
{
	debug_assert (conf_info.frontier != account_info.head);

	auto const hash = account_info.head;

	// Check if the block is already in the backlog, avoids unnecessary ledger lookups
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		if (auto existing = accounts.get<tag_account> ().find (account); existing != accounts.get<tag_account> ().end ())
		{
			if (existing->head == hash)
			{
				return false; // Block already exists in the backlog
			}
		}
	}

	auto const block = ledger.any.block_get (transaction, hash);
	release_assert (block != nullptr);

	auto const priority_balance = block_priority_balance (transaction, *block);
	auto const priority_timestamp = block_priority_timestamp (transaction, *block);
	auto const bucket_index = bucketing.index (priority_balance);

	release_assert (account_info.block_count >= conf_info.height); // Conf height cannot be higher than the head block height
	auto const unconfirmed = account_info.block_count - conf_info.height;

	nano::lock_guard<nano::mutex> guard{ mutex };

	entry updated_entry{
		.account = account,
		.bucket = bucket_index,
		.priority = priority_timestamp,
		.head = hash,
		.unconfirmed = unconfirmed
	};

	uint64_t previous_unconfirmed;

	// Insert or update the account in the backlog
	if (auto existing = accounts.get<tag_account> ().find (account); existing != accounts.get<tag_account> ().end ())
	{
		previous_unconfirmed = existing->unconfirmed;
		accounts.get<tag_account> ().replace (existing, updated_entry);
	}
	else
	{
		previous_unconfirmed = 0;
		accounts.get<tag_account> ().insert (updated_entry);
	}

	// Update the backlog size, sizes are unsigned so some care is needed
	if (unconfirmed > previous_unconfirmed)
	{
		backlog_counter += unconfirmed - previous_unconfirmed;
	}
	else // previous_unconfirmed >= unconfirmed
	{
		backlog_counter -= previous_unconfirmed - unconfirmed;
	}

	return true; // Updated
}

nano::amount nano::bounded_backlog::block_priority_balance (nano::secure::transaction const & transaction, nano::block const & block) const
{
	auto previous_balance_get = [&] (nano::block const & block) {
		auto previous_block = ledger.any.block_get (transaction, block.previous ());
		release_assert (previous_block);
		return previous_block->balance ();
	};

	auto balance = block.balance ();
	auto previous_balance = block.is_send () ? previous_balance_get (block) : 0; // Handle full send case nicely

	return std::max (balance, previous_balance);
}

nano::priority_timestamp nano::bounded_backlog::block_priority_timestamp (nano::secure::transaction const & transaction, nano::block const & block) const
{
	return block.sideband ().timestamp;
}

bool nano::bounded_backlog::predicate () const
{
	return backlog_counter >= config.max_backlog;
}

void nano::bounded_backlog::run ()
{
	std::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		if (predicate ())
		{
			stats.inc (nano::stat::type::bounded_backlog, nano::stat::detail::loop);

			auto targets = gather_targets ();
			if (!targets.empty ())
			{
				lock.unlock ();

				stats.add (nano::stat::type::bounded_backlog, nano::stat::detail::gathered_targets, targets.size ());
				perform_rollbacks (targets);

				// Update info for freshly rolled back accounts
				auto transaction = ledger.tx_begin_read ();
				for (auto const & [account, hash] : targets)
				{
					update (transaction, account);
				}

				lock.lock ();
			}
			else
			{
				stats.inc (nano::stat::type::bounded_backlog, nano::stat::detail::no_targets);

				// Cooldown, this should not happen in normal operation
				condition.wait_for (lock, 100ms, [this] {
					return stopped.load ();
				});
			}
		}
		else
		{
			condition.wait_for (lock, 1s, [this] {
				return stopped || predicate ();
			});
		}
	}
}

bool nano::bounded_backlog::should_rollback (nano::block_hash const & hash) const
{
	if (node.vote_cache.exists (hash))
	{
		return false;
	}
	if (node.vote_router.exists (hash))
	{
		return false;
	}
	if (node.active.recently_confirmed.exists (hash))
	{
		return false;
	}
	if (node.confirming_set.exists (hash))
	{
		return false;
	}
	return true;
}

void nano::bounded_backlog::perform_rollbacks (std::deque<rollback_target> const & targets)
{
	stats.inc (nano::stat::type::bounded_backlog, nano::stat::detail::performing_rollbacks);

	auto transaction = ledger.tx_begin_write (nano::store::writer::bounded_backlog);

	// for (auto const & [account, hash] : targets)
	// {
	// 	std::cout << "rollback: " << hash.to_string () << ", account: " << account.to_account () << std::endl;
	// }

	for (auto const & [account, hash] : targets)
	{
		// Here we check that the block is still OK to rollback, there could be a delay between gathering the targets and performing the rollbacks
		if (ledger.any.block_exists (transaction, hash) && should_rollback (hash))
		{
			logger.debug (nano::log::type::bounded_backlog, "Rolling back: {}, account: {}", hash.to_string (), account.to_account ());

			bool error = ledger.rollback (transaction, hash);
			if (error)
			{
				stats.inc (nano::stat::type::bounded_backlog, nano::stat::detail::rollback_failed);
			}
			else
			{
				stats.inc (nano::stat::type::bounded_backlog, nano::stat::detail::rollback);
			}
		}
		else
		{
			stats.inc (nano::stat::type::bounded_backlog, nano::stat::detail::rollback_missing_block);
		}
	}
}

auto nano::bounded_backlog::gather_targets () const -> std::deque<rollback_target>
{
	debug_assert (!mutex.try_lock ());

	std::deque<rollback_target> targets;

	for (auto bucket_index : bucketing.indices ())
	{
		key const starting_key{ bucket_index, std::numeric_limits<nano::priority_timestamp>::max () }; // Highest timestamp, lowest priority

		std::deque<rollback_target> current;

		auto begin = accounts.get<tag_key> ().lower_bound (starting_key);
		for (auto it = begin; it != accounts.get<tag_key> ().end () && it->bucket == bucket_index && current.size () < config.batch_size; ++it)
		{
			// Only rollback if the block is not being used by the node
			if (should_rollback (it->head))
			{
				current.push_back ({ it->account, it->head });
			}
		}

		for (auto i = 0; i < config.batch_size && i < current.size (); ++i)
		{
			targets.push_back (current[i]);
		}
	}

	return targets;
}

nano::container_info nano::bounded_backlog::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	auto collect_bucket_info = [&] () {
		nano::container_info info;
		for (auto bucket_index : bucketing.indices ())
		{
			key const starting_key{ bucket_index, std::numeric_limits<nano::priority_timestamp>::max () }; // Highest timestamp, lowest priority

			size_t count = 0;

			auto begin = accounts.get<tag_key> ().lower_bound (starting_key);
			for (auto it = begin; it != accounts.get<tag_key> ().end () && it->bucket == bucket_index; ++it)
			{
				++count;
			}

			info.put (std::to_string (bucket_index), count);
		}
		return info;
	};

	nano::container_info info;
	info.put ("accounts", accounts.size ());
	info.put ("backlog", backlog_counter.load ());
	info.add ("buckets", collect_bucket_info ());
	return info;
}
