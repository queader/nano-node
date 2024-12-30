#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/lib/threading.hpp>
#include <nano/lib/timer.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/block_processor.hpp>
#include <nano/node/local_vote_history.hpp>
#include <nano/node/node.hpp>
#include <nano/node/unchecked_map.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/component.hpp>

#include <utility>

/*
 * block_processor
 */

nano::block_processor::block_processor (nano::node_config const & node_config, nano::ledger & ledger_a, nano::unchecked_map & unchecked_a, nano::stats & stats_a, nano::logger & logger_a) :
	config{ node_config.block_processor },
	network_params{ node_config.network_params },
	ledger{ ledger_a },
	unchecked{ unchecked_a },
	stats{ stats_a },
	logger{ logger_a },
	workers{ 1, nano::thread_role::name::block_processing_notifications }
{
	queue.max_size_query = [this] (auto const & origin) {
		switch (origin.source)
		{
			case nano::block_source::live:
			case nano::block_source::live_originator:
				return config.max_peer_queue;
			default:
				return config.max_system_queue;
		}
	};

	queue.priority_query = [this] (auto const & origin) -> size_t {
		switch (origin.source)
		{
			case nano::block_source::live:
			case nano::block_source::live_originator:
				return config.priority_live;
			case nano::block_source::bootstrap:
			case nano::block_source::bootstrap_legacy:
			case nano::block_source::unchecked:
				return config.priority_bootstrap;
			case nano::block_source::local:
				return config.priority_local;
			default:
				return config.priority_system;
		}
	};

	// Requeue blocks that could not be immediately processed
	unchecked.satisfied.add ([this] (nano::unchecked_info const & info) {
		add (info.block, nano::block_source::unchecked);
	});
}

nano::block_processor::~block_processor ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
	debug_assert (!workers.alive ());
}

void nano::block_processor::start ()
{
	debug_assert (!thread.joinable ());

	workers.start ();

	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::block_processing);
		run ();
	});
}

void nano::block_processor::stop ()
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
	workers.stop ();
}

// TODO: Remove and replace all checks with calls to size (block_source)
std::size_t nano::block_processor::size () const
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	return queue.size ();
}

std::size_t nano::block_processor::size (nano::block_source source) const
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	return queue.size ({ source });
}

bool nano::block_processor::add (std::shared_ptr<nano::block> const & block, block_source const source, std::shared_ptr<nano::transport::channel> const & channel, std::function<void (nano::block_status)> callback)
{
	if (network_params.work.validate_entry (*block)) // true => error
	{
		stats.inc (nano::stat::type::block_processor, nano::stat::detail::insufficient_work);
		return false; // Not added
	}

	stats.inc (nano::stat::type::block_processor, nano::stat::detail::process);
	logger.debug (nano::log::type::block_processor, "Processing block (async): {} (source: {} {})",
	block->hash ().to_string (),
	to_string (source),
	channel ? channel->to_string () : "<unknown>"); // TODO: Lazy eval

	return add_impl (context{ block, source, std::move (callback) }, channel);
}

std::optional<nano::block_status> nano::block_processor::add_blocking (std::shared_ptr<nano::block> const & block, block_source const source)
{
	stats.inc (nano::stat::type::block_processor, nano::stat::detail::process_blocking);
	logger.debug (nano::log::type::block_processor, "Processing block (blocking): {} (source: {})", block->hash ().to_string (), to_string (source));

	context ctx{ block, source };
	auto future = ctx.get_future ();
	add_impl (std::move (ctx));

	try
	{
		future.wait ();
		return future.get ();
	}
	catch (std::future_error const &)
	{
		stats.inc (nano::stat::type::block_processor, nano::stat::detail::process_blocking_timeout);
		logger.error (nano::log::type::block_processor, "Block dropped when processing: {}", block->hash ().to_string ());
	}

	return std::nullopt;
}

void nano::block_processor::force (std::shared_ptr<nano::block> const & block_a)
{
	stats.inc (nano::stat::type::block_processor, nano::stat::detail::force);
	logger.debug (nano::log::type::block_processor, "Forcing block: {}", block_a->hash ().to_string ());

	add_impl (context{ block_a, block_source::forced });
}

bool nano::block_processor::add_impl (context ctx, std::shared_ptr<nano::transport::channel> const & channel)
{
	auto const source = ctx.source;
	bool added = false;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		added = queue.push (std::move (ctx), { source, channel });
	}
	if (added)
	{
		condition.notify_all ();
	}
	else
	{
		stats.inc (nano::stat::type::block_processor, nano::stat::detail::overfill);
		stats.inc (nano::stat::type::block_processor_overfill, to_stat_detail (source));
	}
	return added;
}

void nano::block_processor::rollback_competitor (secure::write_transaction const & transaction, nano::block const & fork_block)
{
	auto const hash = fork_block.hash ();
	auto const successor_hash = ledger.any.block_successor (transaction, fork_block.qualified_root ());
	auto const successor = successor_hash ? ledger.any.block_get (transaction, successor_hash.value ()) : nullptr;
	if (successor != nullptr && successor->hash () != hash)
	{
		// Replace our block with the winner and roll back any dependent blocks
		logger.debug (nano::log::type::block_processor, "Rolling back: {} and replacing with: {}", successor->hash ().to_string (), hash.to_string ());

		std::deque<std::shared_ptr<nano::block>> rollback_list;
		if (ledger.rollback (transaction, successor->hash (), rollback_list))
		{
			stats.inc (nano::stat::type::ledger, nano::stat::detail::rollback_failed);
			logger.error (nano::log::type::block_processor, "Failed to roll back: {} because it or a successor was confirmed", successor->hash ().to_string ());
		}
		else
		{
			stats.inc (nano::stat::type::ledger, nano::stat::detail::rollback);
			logger.debug (nano::log::type::block_processor, "Blocks rolled back: {}", rollback_list.size ());
		}

		// Notify observers of the rolled back blocks on a background thread while not holding the ledger write lock
		workers.post ([this, rollback_list = std::move (rollback_list), root = fork_block.qualified_root ()] () {
			rolled_back.notify (rollback_list, root);
		});
	}
}

void nano::block_processor::run ()
{
	nano::interval log_interval;
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		if (!queue.empty ())
		{
			// It's possible that ledger processing happens faster than the notifications can be processed by other components, cooldown here
			while (workers.queued_tasks () >= config.max_queued_notifications)
			{
				stats.inc (nano::stat::type::block_processor, nano::stat::detail::cooldown);
				condition.wait_for (lock, 100ms, [this] { return stopped; });
				if (stopped)
				{
					return;
				}
			}

			if (log_interval.elapsed (15s))
			{
				logger.info (nano::log::type::block_processor, "{} blocks (+ {} forced) in processing queue",
				queue.size (),
				queue.size ({ nano::block_source::forced }));
			}

			auto processed = process_batch (lock);
			debug_assert (!lock.owns_lock ());
			lock.lock ();

			// Queue notifications to be dispatched in the background
			workers.post ([this, processed = std::move (processed)] () mutable {
				stats.inc (nano::stat::type::block_processor, nano::stat::detail::notify);
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
			});
		}
		else
		{
			condition.wait (lock, [this] {
				return stopped || !queue.empty ();
			});
		}
	}
}

auto nano::block_processor::next () -> context
{
	debug_assert (!mutex.try_lock ());
	debug_assert (!queue.empty ()); // This should be checked before calling next

	if (!queue.empty ())
	{
		auto [request, origin] = queue.next ();
		release_assert (origin.source != nano::block_source::forced || request.source == nano::block_source::forced);
		return std::move (request);
	}

	release_assert (false, "next() called when no blocks are ready");
}

auto nano::block_processor::next_batch (size_t max_count) -> std::deque<context>
{
	debug_assert (!mutex.try_lock ());
	debug_assert (!queue.empty ());

	queue.periodic_update ();

	std::deque<context> results;
	while (!queue.empty () && results.size () < max_count)
	{
		results.push_back (next ());
	}
	return results;
}

auto nano::block_processor::process_batch (nano::unique_lock<nano::mutex> & lock) -> processed_batch_t
{
	debug_assert (lock.owns_lock ());
	debug_assert (!mutex.try_lock ());
	debug_assert (!queue.empty ());

	auto batch = next_batch (config.batch_size);

	lock.unlock ();

	auto transaction = ledger.tx_begin_write (nano::store::writer::block_processor);

	nano::timer<std::chrono::milliseconds> timer;
	timer.start ();

	// Processing blocks
	size_t number_of_blocks_processed = 0;
	size_t number_of_forced_processed = 0;

	processed_batch_t processed;
	for (auto & ctx : batch)
	{
		auto const hash = ctx.block->hash ();
		bool const force = ctx.source == nano::block_source::forced;

		transaction.refresh_if_needed ();

		if (force)
		{
			number_of_forced_processed++;
			rollback_competitor (transaction, *ctx.block);
		}

		number_of_blocks_processed++;

		auto result = process_one (transaction, ctx, force);
		processed.emplace_back (result, std::move (ctx));
	}

	if (number_of_blocks_processed != 0 && timer.stop () > std::chrono::milliseconds (100))
	{
		logger.debug (nano::log::type::block_processor, "Processed {} blocks ({} forced) in {} {}", number_of_blocks_processed, number_of_forced_processed, timer.value ().count (), timer.unit ());
	}

	return processed;
}

nano::block_status nano::block_processor::process_one (secure::write_transaction const & transaction_a, context const & context, bool const forced_a)
{
	auto block = context.block;
	auto const hash = block->hash ();
	nano::block_status result = ledger.process (transaction_a, block);

	stats.inc (nano::stat::type::block_processor_result, to_stat_detail (result));
	stats.inc (nano::stat::type::block_processor_source, to_stat_detail (context.source));

	logger.trace (nano::log::type::block_processor, nano::log::detail::block_processed,
	nano::log::arg{ "result", result },
	nano::log::arg{ "source", context.source },
	nano::log::arg{ "arrival", nano::log::microseconds (context.arrival) },
	nano::log::arg{ "forced", forced_a },
	nano::log::arg{ "block", block });

	switch (result)
	{
		case nano::block_status::progress:
		{
			unchecked.trigger (hash);

			/*
			 * For send blocks check epoch open unchecked (gap pending).
			 * For state blocks check only send subtype and only if block epoch is not last epoch.
			 * If epoch is last, then pending entry shouldn't trigger same epoch open block for destination account.
			 */
			if (block->type () == nano::block_type::send || (block->type () == nano::block_type::state && block->is_send () && std::underlying_type_t<nano::epoch> (block->sideband ().details.epoch) < std::underlying_type_t<nano::epoch> (nano::epoch::max)))
			{
				unchecked.trigger (block->destination ());
			}
			break;
		}
		case nano::block_status::gap_previous:
		{
			unchecked.put (block->previous (), block);
			stats.inc (nano::stat::type::ledger, nano::stat::detail::gap_previous);
			break;
		}
		case nano::block_status::gap_source:
		{
			release_assert (block->source_field () || block->link_field ());
			unchecked.put (block->source_field ().value_or (block->link_field ().value_or (0).as_block_hash ()), block);
			stats.inc (nano::stat::type::ledger, nano::stat::detail::gap_source);
			break;
		}
		case nano::block_status::gap_epoch_open_pending:
		{
			unchecked.put (block->account_field ().value_or (0), block); // Specific unchecked key starting with epoch open block account public key
			stats.inc (nano::stat::type::ledger, nano::stat::detail::gap_source);
			break;
		}
		case nano::block_status::old:
		{
			stats.inc (nano::stat::type::ledger, nano::stat::detail::old);
			break;
		}
		case nano::block_status::bad_signature:
		{
			break;
		}
		case nano::block_status::negative_spend:
		{
			break;
		}
		case nano::block_status::unreceivable:
		{
			break;
		}
		case nano::block_status::fork:
		{
			stats.inc (nano::stat::type::ledger, nano::stat::detail::fork);
			break;
		}
		case nano::block_status::opened_burn_account:
		{
			break;
		}
		case nano::block_status::balance_mismatch:
		{
			break;
		}
		case nano::block_status::representative_mismatch:
		{
			break;
		}
		case nano::block_status::block_position:
		{
			break;
		}
		case nano::block_status::insufficient_work:
		{
			break;
		}
	}
	return result;
}

nano::container_info nano::block_processor::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	nano::container_info info;
	info.put ("blocks", queue.size ());
	info.put ("forced", queue.size ({ nano::block_source::forced }));
	info.add ("queue", queue.container_info ());
	info.add ("workers", workers.container_info ());
	return info;
}

/*
 * block_processor::context
 */

nano::block_processor::context::context (std::shared_ptr<nano::block> block, nano::block_source source_a, callback_t callback_a) :
	block{ std::move (block) },
	source{ source_a },
	callback{ std::move (callback_a) }
{
	debug_assert (source != nano::block_source::unknown);
}

auto nano::block_processor::context::get_future () -> std::future<result_t>
{
	return promise.get_future ();
}

void nano::block_processor::context::set_result (result_t const & result)
{
	promise.set_value (result);
}

/*
 * block_processor_config
 */

nano::block_processor_config::block_processor_config (const nano::network_constants & network_constants)
{
}

nano::error nano::block_processor_config::serialize (nano::tomlconfig & toml) const
{
	toml.put ("max_peer_queue", max_peer_queue, "Maximum number of blocks to queue from network peers. \ntype:uint64");
	toml.put ("max_system_queue", max_system_queue, "Maximum number of blocks to queue from system components (local RPC, bootstrap). \ntype:uint64");
	toml.put ("priority_live", priority_live, "Priority for live network blocks. Higher priority gets processed more frequently. \ntype:uint64");
	toml.put ("priority_bootstrap", priority_bootstrap, "Priority for bootstrap blocks. Higher priority gets processed more frequently. \ntype:uint64");
	toml.put ("priority_local", priority_local, "Priority for local RPC blocks. Higher priority gets processed more frequently. \ntype:uint64");

	return toml.get_error ();
}

nano::error nano::block_processor_config::deserialize (nano::tomlconfig & toml)
{
	toml.get ("max_peer_queue", max_peer_queue);
	toml.get ("max_system_queue", max_system_queue);
	toml.get ("priority_live", priority_live);
	toml.get ("priority_bootstrap", priority_bootstrap);
	toml.get ("priority_local", priority_local);

	return toml.get_error ();
}

/*
 *
 */

std::string_view nano::to_string (nano::block_source source)
{
	return nano::enum_util::name (source);
}

nano::stat::detail nano::to_stat_detail (nano::block_source type)
{
	return nano::enum_util::cast<nano::stat::detail> (type);
}