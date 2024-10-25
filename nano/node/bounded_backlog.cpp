#include <nano/lib/blocks.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/node/backlog_scan.hpp>
#include <nano/node/blockprocessor.hpp>
#include <nano/node/bounded_backlog.hpp>
#include <nano/node/confirming_set.hpp>
#include <nano/node/node.hpp>
#include <nano/node/scheduler/component.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_confirmed.hpp>
#include <nano/secure/transaction.hpp>

nano::bounded_backlog::bounded_backlog (nano::bounded_backlog_config const & config_a, nano::node & node_a, nano::ledger & ledger_a, nano::bucketing & bucketing_a, nano::backlog_scan & backlog_scan_a, nano::block_processor & block_processor_a, nano::confirming_set & confirming_set_a, nano::stats & stats_a, nano::logger & logger_a) :
	config{ config_a },
	node{ node_a },
	ledger{ ledger_a },
	bucketing{ bucketing_a },
	backlog_scan{ backlog_scan_a },
	block_processor{ block_processor_a },
	confirming_set{ confirming_set_a },
	stats{ stats_a },
	logger{ logger_a },
	scan_limiter{ config.batch_size, 1.0 }
{
	// Activate accounts with unconfirmed blocks
	// TODO: Use batch scan event
	backlog_scan.activated.add ([this] (auto const & _, auto const & info) {
		auto transaction = ledger.tx_begin_read ();
		activate (transaction, info.account, info.account_info, info.conf_info);
	});

	// Track unconfirmed blocks
	block_processor.batch_processed.add ([this] (auto const & batch) {
		auto transaction = ledger.tx_begin_read ();
		for (auto const & [result, context] : batch)
		{
			if (result == nano::block_status::progress)
			{
				auto const & block = context.block;
				insert (transaction, *block);
			}
		}
	});

	// Remove rolled back blocks from the backlog
	// TODO: Use batch rollback event
	block_processor.rolled_back.add ([this] (auto const & block, auto const & rollback_root) {
		nano::lock_guard<nano::mutex> guard{ mutex };
		index.erase (block->hash ());
	});

	// Remove cemented blocks from the backlog
	confirming_set.batch_cemented.add ([this] (auto const & batch) {
		nano::lock_guard<nano::mutex> guard{ mutex };
		for (auto const & context : batch)
		{
			index.erase (context.block->hash ());
		}
	});
}

nano::bounded_backlog::~bounded_backlog ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
	debug_assert (!scan_thread.joinable ());
}

void nano::bounded_backlog::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::bounded_backlog);
		run ();
	} };

	scan_thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::backlog_scan);
		run_scan ();
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
	if (scan_thread.joinable ())
	{
		scan_thread.join ();
	}
}

uint64_t nano::bounded_backlog::backlog_size () const
{
	return index.size ();
}

bool nano::bounded_backlog::erase (nano::secure::transaction const & transaction, nano::account const & account)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return index.erase (account);
}

void nano::bounded_backlog::activate (nano::secure::transaction & transaction, nano::account const & account, nano::account_info const & account_info, nano::confirmation_height_info const & conf_info)
{
	debug_assert (conf_info.frontier != account_info.head);

	auto contains = [this] (nano::block_hash const & hash) {
		nano::lock_guard<nano::mutex> guard{ mutex };
		return index.contains (hash);
	};

	// Insert blocks into the index starting from the account head block
	auto block = ledger.any.block_get (transaction, account_info.head);
	while (block)
	{
		// We reached the confirmed frontier, no need to track more blocks
		if (block->hash () == conf_info.frontier)
		{
			break;
		}
		// Check if the block is already in the backlog, avoids unnecessary ledger lookups
		if (contains (block->hash ()))
		{
			break;
		}

		bool inserted = insert (transaction, *block);

		// If the block was not inserted, we already have it in the backlog
		if (!inserted)
		{
			break;
		}

		transaction.refresh_if_needed ();

		block = ledger.any.block_get (transaction, block->previous ());
	}
}

void nano::bounded_backlog::update (nano::secure::transaction const & transaction, nano::block_hash const & hash)
{
	// Erase if the block is either confirmed or missing
	if (!ledger.unconfirmed_exists (transaction, hash))
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		index.erase (hash);
	}
}

bool nano::bounded_backlog::insert (nano::secure::transaction const & transaction, nano::block const & block)
{
	auto const [priority_balance, priority_timestamp] = block_priority (transaction, block);
	auto const bucket_index = bucketing.index (priority_balance);

	nano::lock_guard<nano::mutex> guard{ mutex };

	return index.insert (block, bucket_index, priority_timestamp);
}

auto nano::bounded_backlog::block_priority (nano::secure::transaction const & transaction, nano::block const & block) const -> block_priority_result
{
	auto const balance = block.balance ();
	auto const previous_block = !block.previous ().is_zero () ? ledger.any.block_get (transaction, block.previous ()) : nullptr;
	auto const previous_balance = previous_block ? previous_block->balance () : 0;
	auto const priority_balance = std::max (balance, block.is_send () ? previous_balance : 0); // Handle full send case nicely
	auto const priority_timestamp = previous_block ? previous_block->sideband ().timestamp : block.sideband ().timestamp; // Use previous timestamp as priority timestamp
	return { priority_balance, priority_timestamp };
}

bool nano::bounded_backlog::predicate () const
{
	return ledger.backlog_count () > config.max_backlog;
}

void nano::bounded_backlog::run ()
{
	std::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		if (predicate ())
		{
			stats.inc (nano::stat::type::bounded_backlog, nano::stat::detail::loop);

			// Calculate the number of targets to rollback
			uint64_t const backlog = ledger.backlog_count ();
			uint64_t const target_count = backlog > config.max_backlog ? backlog - config.max_backlog : 0;

			auto targets = gather_targets (std::min (target_count, static_cast<uint64_t> (config.batch_size)));
			if (!targets.empty ())
			{
				lock.unlock ();

				stats.add (nano::stat::type::bounded_backlog, nano::stat::detail::gathered_targets, targets.size ());
				perform_rollbacks (targets);

				lock.lock ();

				// Erase the rolled back blocks from the index
				for (auto const & [hash, account] : targets)
				{
					index.erase (hash);
				}
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
	if (node.scheduler.exists (hash))
	{
		return false;
	}
	if (node.confirming_set.contains (hash))
	{
		return false;
	}
	if (node.local_block_broadcaster.contains (hash))
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

	std::deque<std::shared_ptr<nano::block>> rollbacks;

	for (auto const & [hash, account] : targets)
	{
		// Here we check that the block is still OK to rollback, there could be a delay between gathering the targets and performing the rollbacks
		if (auto block = ledger.any.block_get (transaction, hash); block && should_rollback (hash))
		{
			debug_assert (block->account () == account);
			logger.debug (nano::log::type::bounded_backlog, "Rolling back: {}, account: {}", hash.to_string (), account.to_account ());

			std::vector<std::shared_ptr<nano::block>> rollback_list;
			bool error = ledger.rollback (transaction, hash, rollback_list);
			stats.inc (nano::stat::type::bounded_backlog, error ? nano::stat::detail::rollback_failed : nano::stat::detail::rollback);
			rollbacks.insert (rollbacks.end (), rollback_list.begin (), rollback_list.end ());
		}
		else
		{
			stats.inc (nano::stat::type::bounded_backlog, nano::stat::detail::rollback_missing_block);
		}
	}

	rolled_back.notify (rollbacks);
}

auto nano::bounded_backlog::gather_targets (size_t max_count) const -> std::deque<rollback_target>
{
	debug_assert (!mutex.try_lock ());

	std::deque<rollback_target> targets;

	for (auto bucket : bucketing.indices ())
	{
		// Only start rolling back if the bucket is over the threshold of unconfirmed blocks
		if (index.size (bucket) > config.bucket_threshold)
		{
			auto const count = std::min (max_count, config.batch_size);

			auto const top = index.top (bucket, count, [this] (auto const & hash) {
				// Only rollback if the block is not being used by the node
				return should_rollback (hash);
			});

			for (auto const & entry : top)
			{
				targets.push_back (entry);
			}
		}
	}

	return targets;
}

void nano::bounded_backlog::run_scan ()
{
	std::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		auto wait = [&] (auto count) {
			while (!scan_limiter.should_pass (count))
			{
				condition.wait_for (lock, 100ms);
				if (stopped)
				{
					return;
				}
			}
		};

		nano::block_hash last = 0;
		while (!stopped)
		{
			wait (config.batch_size);

			stats.inc (nano::stat::type::bounded_backlog, nano::stat::detail::loop_scan);

			auto batch = index.next (last, config.batch_size);
			if (batch.empty ()) // If batch is empty, we iterated over all accounts in the index
			{
				break;
			}

			lock.unlock ();
			{
				auto transaction = ledger.tx_begin_read ();
				for (auto const & hash : batch)
				{
					stats.inc (nano::stat::type::bounded_backlog, nano::stat::detail::scanned);
					update (transaction, hash);
					last = hash;
				}
			}
			lock.lock ();
		}
	}
}

nano::container_info nano::bounded_backlog::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return index.container_info ();
}

/*
 * backlog_index
 */

bool nano::backlog_index::insert (nano::block const & block, nano::bucket_index bucket, nano::priority_timestamp priority)
{
	auto const hash = block.hash ();
	auto const account = block.account ();
	auto const height = block.sideband ().height;

	entry new_entry{
		.hash = hash,
		.account = account,
		.bucket = bucket,
		.priority = priority,
		.height = height,
	};

	auto [it, inserted] = blocks.emplace (new_entry);
	if (inserted)
	{
		size_by_bucket[bucket]++;
		return true;
	}
	return false;
}

bool nano::backlog_index::erase (nano::account const & account)
{
	auto const [begin, end] = blocks.get<tag_account> ().equal_range (account);
	for (auto it = begin; it != end;)
	{
		size_by_bucket[it->bucket]--;
		it = blocks.get<tag_account> ().erase (it);
	}
	return begin != end;
}

bool nano::backlog_index::erase (nano::block_hash const & hash)
{
	if (auto existing = blocks.get<tag_hash> ().find (hash); existing != blocks.get<tag_hash> ().end ())
	{
		size_by_bucket[existing->bucket]--;
		blocks.get<tag_hash> ().erase (existing);
		return true;
	}
	return false;
}

nano::block_hash nano::backlog_index::head (nano::account const & account) const
{
	// Find the highest height hash for the account
	auto it = blocks.get<tag_height> ().upper_bound (height_key{ account, std::numeric_limits<uint64_t>::max () });
	if (it != blocks.get<tag_height> ().begin ())
	{
		--it;
		if (it->account == account)
		{
			return it->hash;
		}
	}
	debug_assert (false); // Should be checked before calling
	return { 0 };
}

nano::block_hash nano::backlog_index::tail (nano::account const & account) const
{
	// Find the lowest height hash for the account
	auto it = blocks.get<tag_height> ().lower_bound (height_key{ account, 0 });
	if (it != blocks.get<tag_height> ().end () && it->account == account)
	{
		return it->hash;
	}
	debug_assert (false); // Should be checked before calling
	return { 0 };
}

auto nano::backlog_index::top (nano::bucket_index bucket, size_t count, filter_callback const & filter) const -> std::deque<rollback_target>
{
	priority_key const starting_key{ bucket, std::numeric_limits<nano::priority_timestamp>::max () }; // Highest timestamp, lowest priority

	std::deque<rollback_target> results;

	auto begin = blocks.get<tag_priority> ().lower_bound (starting_key);
	for (auto it = begin; it != blocks.get<tag_priority> ().end () && it->bucket == bucket && results.size () < count; ++it)
	{
		if (filter (it->hash))
		{
			results.push_back ({ it->hash, it->account });
		}
	}

	return results;
}

std::deque<nano::block_hash> nano::backlog_index::next (nano::block_hash last, size_t count) const
{
	std::deque<block_hash> results;

	auto it = blocks.get<tag_hash_ordered> ().upper_bound (last);
	auto end = blocks.get<tag_hash_ordered> ().end ();

	while (it != end && results.size () < count)
	{
		results.push_back (it->hash);
		last = it->hash;
		it = blocks.get<tag_hash_ordered> ().upper_bound (last);
	}

	return results;
}

bool nano::backlog_index::contains (nano::block_hash const & hash) const
{
	return blocks.get<tag_hash> ().contains (hash);
}

size_t nano::backlog_index::size () const
{
	return blocks.size ();
}

size_t nano::backlog_index::size (nano::bucket_index bucket) const
{
	if (auto it = size_by_bucket.find (bucket); it != size_by_bucket.end ())
	{
		return it->second;
	}
	return 0;
}

nano::container_info nano::backlog_index::container_info () const
{
	auto collect_bucket_sizes = [&] () {
		nano::container_info info;
		for (auto [bucket, count] : size_by_bucket)
		{
			info.put (std::to_string (bucket), count);
		}
		return info;
	};

	nano::container_info info;
	info.put ("blocks", blocks);
	info.add ("sizes", collect_bucket_sizes ());
	return info;
}