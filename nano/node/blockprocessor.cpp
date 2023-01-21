#include <nano/lib/threading.hpp>
#include <nano/lib/timer.hpp>
#include <nano/node/blockprocessor.hpp>
#include <nano/node/election.hpp>
#include <nano/node/node.hpp>
#include <nano/node/websocket.hpp>
#include <nano/secure/store.hpp>

#include <boost/format.hpp>

std::chrono::milliseconds constexpr nano::block_processor::confirmation_request_delay;

nano::block_post_events::block_post_events (std::function<nano::read_transaction ()> && get_transaction_a) :
	get_transaction (std::move (get_transaction_a))
{
}

nano::block_post_events::~block_post_events ()
{
	debug_assert (get_transaction != nullptr);
	auto transaction (get_transaction ());
	for (auto const & i : events)
	{
		i (transaction);
	}
}

nano::block_processor::block_processor (nano::node & node_a, nano::write_database_queue & write_database_queue_a) :
	next_log (std::chrono::steady_clock::now ()),
	node (node_a),
	write_database_queue (write_database_queue_a),
	state_block_signature_verification (node.checker, node.ledger.constants.epochs, node.config, node.logger, node.flags.block_processor_verification_size)
{
	state_block_signature_verification.blocks_verified_callback = [this] (std::deque<nano::state_block_signature_verification::value_type> & items, std::vector<int> const & verifications, std::vector<nano::block_hash> const & hashes, std::vector<nano::signature> const & blocks_signatures) {
		this->process_verified_state_blocks (items, verifications, hashes, blocks_signatures);
	};
	state_block_signature_verification.transition_inactive_callback = [this] () {
		if (this->flushing)
		{
			{
				// Prevent a race with condition.wait in block_processor::flush
				nano::lock_guard<nano::mutex> guard{ this->mutex };
			}
			this->condition.notify_all ();
		}
	};
	processing_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::block_processing);
		this->process_blocks ();
	});
}

void nano::block_processor::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	state_block_signature_verification.stop ();
	nano::join_or_pass (processing_thread);
}

void nano::block_processor::flush ()
{
	node.checker.flush ();
	flushing = true;
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped && (have_blocks () || active || state_block_signature_verification.is_active ()))
	{
		condition.wait (lock);
	}
	flushing = false;
}

std::size_t nano::block_processor::size ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	return (blocks.size () + state_block_signature_verification.size () + forced.size ());
}

bool nano::block_processor::full ()
{
	return size () >= node.flags.block_processor_full_size;
}

bool nano::block_processor::half_full ()
{
	return size () >= node.flags.block_processor_full_size / 2;
}

void nano::block_processor::add (std::shared_ptr<nano::block> const & block_a)
{
	nano::unchecked_info info (block_a);
	add (info);
}

void nano::block_processor::add (nano::unchecked_info const & info_a)
{
	auto const & block = info_a.block;
	debug_assert (!node.network_params.work.validate_entry (*block));
	if (block->type () == nano::block_type::state || block->type () == nano::block_type::open)
	{
		state_block_signature_verification.add ({ block });
	}
	else
	{
		{
			nano::lock_guard<nano::mutex> guard{ mutex };
			blocks.emplace_back (info_a);
		}
		condition.notify_all ();
	}
}

void nano::block_processor::add_local (nano::unchecked_info const & info_a)
{
	debug_assert (!node.network_params.work.validate_entry (*info_a.block));
	state_block_signature_verification.add ({ info_a.block });
}

void nano::block_processor::force (std::shared_ptr<nano::block> const & block_a)
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		forced.push_back (block_a);
	}
	condition.notify_all ();
}

void nano::block_processor::wait_write ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	awaiting_write = true;
}

void nano::block_processor::process_blocks ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		if (have_blocks_ready ())
		{
			active = true;
			lock.unlock ();
			process_batch (lock);
			lock.lock ();
			active = false;
		}
		else
		{
			condition.notify_one ();
			condition.wait (lock);
		}
	}
}

bool nano::block_processor::should_log ()
{
	auto result (false);
	auto now (std::chrono::steady_clock::now ());
	if (next_log < now)
	{
		next_log = now + (node.config.logging.timing_logging () ? std::chrono::seconds (2) : std::chrono::seconds (15));
		result = true;
	}
	return result;
}

bool nano::block_processor::have_blocks_ready ()
{
	debug_assert (!mutex.try_lock ());
	return !blocks.empty () || !forced.empty ();
}

bool nano::block_processor::have_blocks ()
{
	debug_assert (!mutex.try_lock ());
	return have_blocks_ready () || state_block_signature_verification.size () != 0;
}

void nano::block_processor::process_verified_state_blocks (std::deque<nano::state_block_signature_verification::value_type> & items, std::vector<int> const & verifications, std::vector<nano::block_hash> const & hashes, std::vector<nano::signature> const & blocks_signatures)
{
	{
		nano::unique_lock<nano::mutex> lk{ mutex };
		for (auto i (0); i < verifications.size (); ++i)
		{
			debug_assert (verifications[i] == 1 || verifications[i] == 0);
			auto & item = items.front ();
			auto & [block] = item;
			if (!block->link ().is_zero () && node.ledger.is_epoch_link (block->link ()))
			{
				// Epoch blocks
				if (verifications[i] == 1)
				{
					blocks.emplace_back (block);
				}
				else
				{
					// Possible regular state blocks with epoch link (send subtype)
					blocks.emplace_back (block);
				}
			}
			else if (verifications[i] == 1)
			{
				// Non epoch blocks
				blocks.emplace_back (block);
			}
			else
			{
				requeue_invalid (hashes[i], { block });
			}
			items.pop_front ();
		}
	}
	condition.notify_all ();
}

void nano::block_processor::process_batch (nano::unique_lock<nano::mutex> & lock_a)
{
	auto scoped_write_guard = write_database_queue.wait (nano::writer::process_batch);
	block_post_events post_events ([&store = node.store] { return store.tx_begin_read (); });
	auto transaction (node.store.tx_begin_write ({ tables::accounts, tables::blocks, tables::frontiers, tables::pending, tables::unchecked }));
	nano::timer<std::chrono::milliseconds> timer_l;
	lock_a.lock ();
	timer_l.start ();
	// Processing blocks
	unsigned number_of_blocks_processed (0), number_of_forced_processed (0);
	auto deadline_reached = [&timer_l, deadline = node.config.block_processor_batch_max_time] { return timer_l.after_deadline (deadline); };
	auto processor_batch_reached = [&number_of_blocks_processed, max = node.flags.block_processor_batch_size] { return number_of_blocks_processed >= max; };
	auto store_batch_reached = [&number_of_blocks_processed, max = node.store.max_block_write_batch_num ()] { return number_of_blocks_processed >= max; };
	while (have_blocks_ready () && (!deadline_reached () || !processor_batch_reached ()) && !awaiting_write && !store_batch_reached ())
	{
		if ((blocks.size () + state_block_signature_verification.size () + forced.size () > 64) && should_log ())
		{
			// TODO: Cleaner periodic logging
			nlogger.debug ("{} blocks [+ {} state blocks] [+ {} forced] in processing queue", blocks.size (), state_block_signature_verification.size (), forced.size ());
		}

		nano::unchecked_info info;
		nano::block_hash hash (0);
		bool force (false);
		if (forced.empty ())
		{
			info = blocks.front ();
			blocks.pop_front ();
			hash = info.block->hash ();
		}
		else
		{
			info = nano::unchecked_info (forced.front ());
			forced.pop_front ();
			hash = info.block->hash ();
			force = true;
			number_of_forced_processed++;
		}
		lock_a.unlock ();
		if (force)
		{
			auto successor (node.ledger.successor (transaction, info.block->qualified_root ()));
			if (successor != nullptr && successor->hash () != hash)
			{
				// Replace our block with the winner and roll back any dependent blocks
				nlogger.debug ("Rolling back: {} and replacing with: {}", successor->hash ().to_string (), hash.to_string ());

				std::vector<std::shared_ptr<nano::block>> rollback_list;
				if (node.ledger.rollback (transaction, successor->hash (), rollback_list))
				{
					nlogger.error ("Failed to roll back: {} because it or a successor was confirmed", successor->hash ().to_string ());
					node.stats.inc (nano::stat::type::ledger, nano::stat::detail::rollback_failed);
				}
				else
				{
					nlogger.debug ("Blocks rolled back: {}", rollback_list.size ());
				}

				// Deleting from votes cache, stop active transaction
				for (auto & i : rollback_list)
				{
					node.history.erase (i->root ());
					// Stop all rolled back active transactions except initial
					if (i->hash () != successor->hash ())
					{
						node.active.erase (*i);
					}
				}
			}
		}
		number_of_blocks_processed++;
		process_one (transaction, post_events, info, force);
		lock_a.lock ();
	}
	awaiting_write = false;
	lock_a.unlock ();

	if (number_of_blocks_processed != 0 && timer_l.stop () > std::chrono::milliseconds (100))
	{
		nlogger.debug ("Processed {} blocks ({} forced) in {}{}", number_of_blocks_processed, number_of_forced_processed, timer_l.value ().count (), timer_l.unit ());
	}
}

void nano::block_processor::process_live (nano::transaction const & transaction_a, nano::block_hash const & hash_a, std::shared_ptr<nano::block> const & block_a, nano::process_return const & process_return_a, nano::block_origin const origin_a)
{
	// Start collecting quorum on block
	if (node.ledger.dependents_confirmed (transaction_a, *block_a))
	{
		auto account = block_a->account ().is_zero () ? block_a->sideband ().account : block_a->account ();
		node.scheduler.activate (account, transaction_a);
	}

	// Notify inactive vote cache about a new live block
	node.inactive_vote_cache.trigger (block_a->hash ());

	// Announce block contents to the network
	if (origin_a == nano::block_origin::local)
	{
		node.network.flood_block_initial (block_a);
	}
	else if (!node.flags.disable_block_processor_republishing && node.block_arrival.recent (hash_a))
	{
		node.network.flood_block (block_a, nano::buffer_drop_policy::limiter);
	}

	if (node.websocket_server && node.websocket_server->any_subscriber (nano::websocket::topic::new_unconfirmed_block))
	{
		node.websocket_server->broadcast (nano::websocket::message_builder ().new_block_arrived (*block_a));
	}
}

nano::process_return nano::block_processor::process_one (nano::write_transaction const & transaction_a, block_post_events & events_a, nano::unchecked_info info_a, bool const forced_a, nano::block_origin const origin_a)
{
	nano::process_return result;
	auto block (info_a.block);
	auto hash (block->hash ());
	result = node.ledger.process (transaction_a, *block);
	events_a.events.emplace_back ([this, result, block = info_a.block] (nano::transaction const & tx) {
		processed.notify (tx, result, *block);
	});
	switch (result.code)
	{
		case nano::process_result::progress:
		{
			nlogger_process.debug ("Processing block: {}", hash.to_string ());

			events_a.events.emplace_back ([this, hash, block = info_a.block, result, origin_a] (nano::transaction const & post_event_transaction_a) {
				process_live (post_event_transaction_a, hash, block, result, origin_a);
			});
			queue_unchecked (transaction_a, hash);
			/* For send blocks check epoch open unchecked (gap pending).
			For state blocks check only send subtype and only if block epoch is not last epoch.
			If epoch is last, then pending entry shouldn't trigger same epoch open block for destination account. */
			if (block->type () == nano::block_type::send || (block->type () == nano::block_type::state && block->sideband ().details.is_send && std::underlying_type_t<nano::epoch> (block->sideband ().details.epoch) < std::underlying_type_t<nano::epoch> (nano::epoch::max)))
			{
				/* block->destination () for legacy send blocks
				block->link () for state blocks (send subtype) */
				queue_unchecked (transaction_a, block->destination ().is_zero () ? block->link () : block->destination ());
			}
			break;
		}
		case nano::process_result::gap_previous:
		{
			nlogger_process.debug ("Gap previous: {}", hash.to_string ());

			debug_assert (info_a.modified () != 0);
			node.unchecked.put (block->previous (), info_a);

			events_a.events.emplace_back ([this, hash] (nano::transaction const & /* unused */) { this->node.gap_cache.add (hash); });
			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::gap_previous);
			break;
		}
		case nano::process_result::gap_source:
		{
			nlogger_process.debug ("Gap source: {}", hash.to_string ());

			debug_assert (info_a.modified () != 0);
			node.unchecked.put (node.ledger.block_source (transaction_a, *(block)), info_a);

			events_a.events.emplace_back ([this, hash] (nano::transaction const & /* unused */) { this->node.gap_cache.add (hash); });
			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::gap_source);
			break;
		}
		case nano::process_result::gap_epoch_open_pending:
		{
			nlogger_process.debug ("Gap pending entries for epoch open: {}", hash.to_string ());

			debug_assert (info_a.modified () != 0);
			node.unchecked.put (block->account (), info_a); // Specific unchecked key starting with epoch open block account public key

			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::gap_source);
			break;
		}
		case nano::process_result::old:
		{
			nlogger_process.debug ("Old: {}", hash.to_string ());

			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::old);
			break;
		}
		case nano::process_result::bad_signature:
		{
			nlogger_process.debug ("Bad signature: {}", hash.to_string ());

			events_a.events.emplace_back ([this, hash, info_a] (nano::transaction const & /* unused */) { requeue_invalid (hash, info_a); });
			break;
		}
		case nano::process_result::negative_spend:
		{
			nlogger_process.debug ("Negative spend: {}", hash.to_string ());
			break;
		}
		case nano::process_result::unreceivable:
		{
			nlogger_process.debug ("Unreceivable: {}", hash.to_string ());
			break;
		}
		case nano::process_result::fork:
		{
			nlogger_process.debug ("Fork: {} [root: {}]", hash.to_string (), block->root ().to_string ());

			events_a.events.emplace_back ([this, block] (nano::transaction const &) { this->node.active.publish (block); });

			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::fork);
			break;
		}
		case nano::process_result::opened_burn_account:
		{
			nlogger_process.debug ("Rejecting open block for burn account: {}", hash.to_string ());
			break;
		}
		case nano::process_result::balance_mismatch:
		{
			nlogger_process.debug ("Balance mismatch: {}", hash.to_string ());
			break;
		}
		case nano::process_result::representative_mismatch:
		{
			nlogger_process.debug ("Representative mismatch: {}", hash.to_string ());
			break;
		}
		case nano::process_result::block_position:
		{
			nlogger_process.debug ("Block: {} cannot follow predecessor {}", hash.to_string (), block->previous ().to_string ());
			break;
		}
		case nano::process_result::insufficient_work:
		{
			nlogger_process.debug ("Insufficient work for: {} [work: {}, difficulty: {}]", hash.to_string (), nano::to_string_hex (block->block_work ()), nano::to_string_hex (node.network_params.work.difficulty (*block)));
			break;
		}
	}

	node.stats.inc (nano::stat::type::blockprocessor, nano::to_stat_detail (result.code));

	return result;
}

nano::process_return nano::block_processor::process_one (nano::write_transaction const & transaction_a, block_post_events & events_a, std::shared_ptr<nano::block> const & block_a)
{
	nano::unchecked_info info (block_a);
	auto result (process_one (transaction_a, events_a, info));
	return result;
}

void nano::block_processor::queue_unchecked (nano::write_transaction const & transaction_a, nano::hash_or_account const & hash_or_account_a)
{
	node.unchecked.trigger (hash_or_account_a);
	node.gap_cache.erase (hash_or_account_a.hash);
}

void nano::block_processor::requeue_invalid (nano::block_hash const & hash_a, nano::unchecked_info const & info_a)
{
	debug_assert (hash_a == info_a.block->hash ());
	node.bootstrap_initiator.lazy_requeue (hash_a, info_a.block->previous ());
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (block_processor & block_processor, std::string const & name)
{
	std::size_t blocks_count;
	std::size_t forced_count;

	{
		nano::lock_guard<nano::mutex> guard{ block_processor.mutex };
		blocks_count = block_processor.blocks.size ();
		forced_count = block_processor.forced.size ();
	}

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (collect_container_info (block_processor.state_block_signature_verification, "state_block_signature_verification"));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocks", blocks_count, sizeof (decltype (block_processor.blocks)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "forced", forced_count, sizeof (decltype (block_processor.forced)::value_type) }));
	return composite;
}
