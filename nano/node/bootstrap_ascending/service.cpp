#include <nano/lib/blocks.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/node/blockprocessor.hpp>
#include <nano/node/bootstrap_ascending/service.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/account.hpp>
#include <nano/store/component.hpp>

using namespace std::chrono_literals;

/*
 * bootstrap_ascending
 */

nano::bootstrap_ascending::service::service (nano::node_config const & config_a, nano::block_processor & block_processor_a, nano::ledger & ledger_a, nano::network & network_a, nano::stats & stat_a) :
	config{ config_a },
	network_consts{ config.network_params.network },
	block_processor{ block_processor_a },
	ledger{ ledger_a },
	network{ network_a },
	stats{ stat_a },
	accounts{ config.bootstrap_ascending.account_sets, stats },
	iterator{ ledger },
	throttle{ compute_throttle_size () },
	scoring{ config.bootstrap_ascending, config.network_params.network },
	frontiers{ config.bootstrap_ascending.frontier_scan, stats },
	database_limiter{ config.bootstrap_ascending.database_rate_limit, 1.0 },
	frontiers_limiter{ 10, 1.0 }
{
	// TODO: This is called from a very congested blockprocessor thread. Offload this work to a dedicated processing thread
	block_processor.batch_processed.add ([this] (auto const & batch) {
		{
			nano::lock_guard<nano::mutex> lock{ mutex };

			auto transaction = ledger.tx_begin_read ();
			for (auto const & [result, context] : batch)
			{
				debug_assert (context.block != nullptr);
				inspect (transaction, result, *context.block);
			}
		}
		condition.notify_all ();
	});
}

nano::bootstrap_ascending::service::~service ()
{
	// All threads must be stopped before destruction
	debug_assert (!priorities_thread.joinable ());
	debug_assert (!database_thread.joinable ());
	debug_assert (!dependencies_thread.joinable ());
	debug_assert (!frontiers_thread.joinable ());
	debug_assert (!frontiers_processing_thread.joinable ());
	debug_assert (!timeout_thread.joinable ());
}

void nano::bootstrap_ascending::service::start ()
{
	debug_assert (!priorities_thread.joinable ());
	debug_assert (!database_thread.joinable ());
	debug_assert (!dependencies_thread.joinable ());
	debug_assert (!frontiers_thread.joinable ());
	debug_assert (!frontiers_processing_thread.joinable ());
	debug_assert (!timeout_thread.joinable ());

	priorities_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
		run_priorities ();
	});

	// database_thread = std::thread ([this] () {
	// 	nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
	// 	run_database ();
	// });

	dependencies_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
		run_dependencies ();
	});

	frontiers_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
		run_frontiers ();
	});

	frontiers_processing_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
		run_frontiers_processing ();
	});

	timeout_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
		run_timeouts ();
	});
}

void nano::bootstrap_ascending::service::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();

	nano::join_or_pass (priorities_thread);
	nano::join_or_pass (database_thread);
	nano::join_or_pass (dependencies_thread);
	nano::join_or_pass (frontiers_thread);
	nano::join_or_pass (frontiers_processing_thread);
	nano::join_or_pass (timeout_thread);
}

bool nano::bootstrap_ascending::service::send (std::shared_ptr<nano::transport::channel> const & channel, async_tag tag)
{
	debug_assert (tag.type != query_type::invalid);
	debug_assert (tag.source != query_source::invalid);

	nano::asc_pull_req request{ network_consts };
	request.id = tag.id;

	switch (tag.type)
	{
		case query_type::blocks_by_hash:
		case query_type::blocks_by_account:
		{
			request.type = nano::asc_pull_type::blocks;

			nano::asc_pull_req::blocks_payload pld;
			pld.start = tag.start;
			pld.count = tag.count;
			pld.start_type = tag.type == query_type::blocks_by_hash ? nano::asc_pull_req::hash_type::block : nano::asc_pull_req::hash_type::account;
			request.payload = pld;
		}
		break;
		case query_type::account_info_by_hash:
		{
			request.type = nano::asc_pull_type::account_info;

			nano::asc_pull_req::account_info_payload pld;
			pld.target_type = nano::asc_pull_req::hash_type::block; // Query account info by block hash
			pld.target = tag.start;
			request.payload = pld;
		}
		break;
		case query_type::frontiers:
		{
			request.type = nano::asc_pull_type::frontiers;

			nano::asc_pull_req::frontiers_payload pld;
			pld.start = tag.start.as_account ();
			pld.count = nano::asc_pull_ack::frontiers_payload::max_frontiers;
			request.payload = pld;
		}
		break;
		default:
			debug_assert (false);
	}

	request.update_header ();

	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		debug_assert (tags.get<tag_id> ().count (tag.id) == 0);
		tags.get<tag_id> ().insert (tag);
	}

	bool sent = channel->send (
	request, nullptr,
	nano::transport::buffer_drop_policy::limiter, nano::transport::traffic_type::bootstrap);

	if (sent)
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::request, nano::stat::dir::out);
		stats.inc (nano::stat::type::bootstrap_ascending_request, to_stat_detail (tag.type), nano::stat::dir::out);
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::request_failed, nano::stat::dir::in);
		stats.inc (nano::stat::type::bootstrap_ascending_request, to_stat_detail (tag.type), nano::stat::dir::in);

		// Avoid holding the lock during send operation, this should be infrequent case
		nano::lock_guard<nano::mutex> lock{ mutex };
		debug_assert (tags.get<tag_id> ().count (tag.id) > 0);
		tags.get<tag_id> ().erase (tag.id);
	}

	return sent;
}

std::size_t nano::bootstrap_ascending::service::priority_size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.priority_size ();
}

std::size_t nano::bootstrap_ascending::service::blocked_size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.blocked_size ();
}

std::size_t nano::bootstrap_ascending::service::score_size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return scoring.size ();
}

/** Inspects a block that has been processed by the block processor
- Marks an account as blocked if the result code is gap source as there is no reason request additional blocks for this account until the dependency is resolved
- Marks an account as forwarded if it has been recently referenced by a block that has been inserted.
 */
void nano::bootstrap_ascending::service::inspect (secure::transaction const & tx, nano::block_status const & result, nano::block const & block)
{
	debug_assert (!mutex.try_lock ());

	auto const hash = block.hash ();

	switch (result)
	{
		case nano::block_status::progress:
		{
			const auto account = block.account ();

			// If we've inserted any block in to an account, unmark it as blocked
			accounts.unblock (account);
			accounts.priority_up (account);

			if (block.is_send ())
			{
				auto destination = block.destination ();
				accounts.unblock (destination, hash); // Unblocking automatically inserts account into priority set
				accounts.priority_set (destination);
			}
		}
		break;
		case nano::block_status::gap_source:
		{
			const auto account = block.previous ().is_zero () ? block.account_field ().value () : ledger.any.block_account (tx, block.previous ()).value ();
			const auto source = block.source_field ().value_or (block.link_field ().value_or (0).as_block_hash ());

			// Mark account as blocked because it is missing the source block
			accounts.block (account, source);
		}
		break;
		case nano::block_status::gap_previous:
		{
			if (block.type () == block_type::state)
			{
				const auto account = block.account_field ().value ();
				accounts.priority_set (account);
			}
		}
		break;
		default: // No need to handle other cases
			break;
	}
}

void nano::bootstrap_ascending::service::wait (std::function<bool ()> const & predicate) const
{
	std::unique_lock<nano::mutex> lock{ mutex };

	std::chrono::milliseconds interval = 5ms;
	while (!stopped && !predicate ())
	{
		condition.wait_for (lock, interval);
		interval = std::min (interval * 2, config.bootstrap_ascending.throttle_wait);
	}
}

void nano::bootstrap_ascending::service::wait_tags ()
{
	wait ([this] () {
		debug_assert (!mutex.try_lock ());
		return tags.size () < config.bootstrap_ascending.max_requests;
	});
}

void nano::bootstrap_ascending::service::wait_blockprocessor ()
{
	wait ([this] () {
		return block_processor.size (nano::block_source::bootstrap) < config.bootstrap_ascending.block_wait_count;
	});
}

std::shared_ptr<nano::transport::channel> nano::bootstrap_ascending::service::wait_channel ()
{
	std::shared_ptr<nano::transport::channel> channel;

	wait ([this, &channel] () {
		debug_assert (!mutex.try_lock ());
		channel = scoring.channel ();
		return channel != nullptr; // Wait until a channel is available
	});

	return channel;
}

size_t nano::bootstrap_ascending::service::count_tags (nano::account const & account, query_source source) const
{
	debug_assert (!mutex.try_lock ());
	auto [begin, end] = tags.get<tag_account> ().equal_range (account);
	return std::count_if (begin, end, [source] (auto const & tag) { return tag.source == source; });
}

size_t nano::bootstrap_ascending::service::count_tags (nano::block_hash const & hash, query_source source) const
{
	debug_assert (!mutex.try_lock ());
	auto [begin, end] = tags.get<tag_hash> ().equal_range (hash);
	return std::count_if (begin, end, [source] (auto const & tag) { return tag.source == source; });
}

std::pair<nano::account, double> nano::bootstrap_ascending::service::next_priority ()
{
	debug_assert (!mutex.try_lock ());

	auto account = accounts.next_priority ([this] (nano::account const & account) {
		return count_tags (account, query_source::priority) < 4;
	});

	if (account.is_zero ())
	{
		return {};
	}

	stats.inc (nano::stat::type::bootstrap_ascending_next, nano::stat::detail::next_priority);

	// TODO: Priority could be returned by the accounts.next_priority() call
	return { account, accounts.priority (account) };
}

std::pair<nano::account, double> nano::bootstrap_ascending::service::wait_priority ()
{
	std::pair<nano::account, double> result{ 0, 0 };

	wait ([this, &result] () {
		debug_assert (!mutex.try_lock ());
		result = next_priority ();
		if (!result.first.is_zero ())
		{
			return true;
		}
		return false;
	});

	return result;
}

nano::account nano::bootstrap_ascending::service::next_database (bool should_throttle)
{
	debug_assert (!mutex.try_lock ());

	// Throttling increases the weight of database requests
	// TODO: Make this ratio configurable
	if (!database_limiter.should_pass (should_throttle ? 22 : 1))
	{
		return { 0 };
	}

	auto account = iterator.next ([this] (nano::account const & account) {
		return count_tags (account, query_source::database) == 0;
	});

	if (account.is_zero ())
	{
		return { 0 };
	}

	stats.inc (nano::stat::type::bootstrap_ascending_next, nano::stat::detail::next_database);
	return account;
}

nano::account nano::bootstrap_ascending::service::wait_database (bool should_throttle)
{
	nano::account result{ 0 };

	wait ([this, &result, should_throttle] () {
		debug_assert (!mutex.try_lock ());
		result = next_database (should_throttle);
		if (!result.is_zero ())
		{
			return true;
		}
		return false;
	});

	return result;
}

nano::block_hash nano::bootstrap_ascending::service::next_blocking ()
{
	debug_assert (!mutex.try_lock ());

	auto blocking = accounts.next_blocking ([this] (nano::block_hash const & hash) {
		return count_tags (hash, query_source::blocking) == 0;
	});

	if (blocking.is_zero ())
	{
		return { 0 };
	}

	stats.inc (nano::stat::type::bootstrap_ascending_next, nano::stat::detail::next_blocking);
	return blocking;
}

nano::block_hash nano::bootstrap_ascending::service::wait_blocking ()
{
	nano::block_hash result{ 0 };

	wait ([this, &result] () {
		debug_assert (!mutex.try_lock ());
		result = next_blocking ();
		if (!result.is_zero ())
		{
			return true;
		}
		return false;
	});

	return result;
}

nano::account nano::bootstrap_ascending::service::wait_frontier ()
{
	nano::account result{ 0 };

	wait ([this, &result] () {
		debug_assert (!mutex.try_lock ());
		result = frontiers.next ();
		if (!result.is_zero ())
		{
			stats.inc (nano::stat::type::bootstrap_ascending_next, nano::stat::detail::next_frontier);
			return true;
		}
		return false;
	});

	return result;
}

bool nano::bootstrap_ascending::service::request (nano::account account, size_t count, std::shared_ptr<nano::transport::channel> const & channel, query_source source)
{
	debug_assert (count > 0);
	debug_assert (count <= nano::bootstrap_server::max_blocks);

	async_tag tag{};
	tag.source = source;
	tag.account = account;
	tag.count = count;

	// Check if the account picked has blocks, if it does, start the pull from the highest block
	auto info = ledger.store.account.get (ledger.store.tx_begin_read (), account);
	if (info)
	{
		tag.type = query_type::blocks_by_hash;
		tag.start = info->head;
		tag.hash = info->head;
	}
	else
	{
		tag.type = query_type::blocks_by_account;
		tag.start = account;
	}

	return send (channel, tag);
}

bool nano::bootstrap_ascending::service::request_info (nano::block_hash hash, std::shared_ptr<nano::transport::channel> const & channel, query_source source)
{
	async_tag tag{};
	tag.type = query_type::account_info_by_hash;
	tag.source = source;
	tag.start = hash;
	tag.hash = hash;

	return send (channel, tag);
}

bool nano::bootstrap_ascending::service::request_frontiers (nano::account start, std::shared_ptr<nano::transport::channel> const & channel, query_source source)
{
	async_tag tag{};
	tag.type = query_type::frontiers;
	tag.source = source;
	tag.start = start;

	return send (channel, tag);
}

void nano::bootstrap_ascending::service::run_one_priority ()
{
	wait_tags ();
	wait_blockprocessor ();
	auto channel = wait_channel ();
	if (!channel)
	{
		return;
	}
	auto [account, priority] = wait_priority ();
	if (account.is_zero ())
	{
		return;
	}
	auto count = std::clamp (static_cast<size_t> (priority), 2ul, nano::bootstrap_server::max_blocks);
	bool sent = request (account, count, channel, query_source::priority);
	if (sent)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		accounts.timestamp_set (account);
	}
}

void nano::bootstrap_ascending::service::run_priorities ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		lock.unlock ();
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::loop);
		run_one_priority ();
		lock.lock ();
	}
}

void nano::bootstrap_ascending::service::run_one_database (bool should_throttle)
{
	wait_tags ();
	wait_blockprocessor ();
	auto channel = wait_channel ();
	if (!channel)
	{
		return;
	}
	auto account = wait_database (should_throttle);
	if (account.is_zero ())
	{
		return;
	}
	request (account, 2, channel, query_source::database);
}

void nano::bootstrap_ascending::service::run_database ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		// Avoid high churn rate of database requests
		bool should_throttle = !iterator.warmup () && throttle.throttled ();
		lock.unlock ();
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::loop_database);
		run_one_database (should_throttle);
		lock.lock ();
	}
}

void nano::bootstrap_ascending::service::run_one_blocking ()
{
	wait_tags ();
	wait_blockprocessor ();
	auto channel = wait_channel ();
	if (!channel)
	{
		return;
	}
	auto blocking = wait_blocking ();
	if (blocking.is_zero ())
	{
		return;
	}
	request_info (blocking, channel, query_source::blocking);
}

void nano::bootstrap_ascending::service::run_dependencies ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		lock.unlock ();
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::loop_dependencies);
		run_one_blocking ();
		lock.lock ();
	}
}

void nano::bootstrap_ascending::service::run_one_frontier ()
{
	wait ([this] () {
		return accounts.priority_size () < 1000 && accounts.blocked_vacancy () > 10000;
	});
	wait ([this] () {
		return frontiers_limiter.should_pass (1);
	});
	wait ([this] () {
		return pending_frontiers.size () < 16;
	});
	wait_tags ();
	auto channel = wait_channel ();
	if (!channel)
	{
		return;
	}
	auto frontier = wait_frontier ();
	if (frontier.is_zero ())
	{
		return;
	}
	request_frontiers (frontier, channel, query_source::frontiers);
}

void nano::bootstrap_ascending::service::run_frontiers ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		lock.unlock ();
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::loop_frontiers);
		run_one_frontier ();
		lock.lock ();
	}
}

void nano::bootstrap_ascending::service::run_frontiers_processing ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		if (!pending_frontiers.empty ())
		{
			stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::loop_frontiers_processing);

			auto frontiers = pending_frontiers.front ();
			pending_frontiers.pop_front ();
			lock.unlock ();

			process_frontiers (frontiers);

			lock.lock ();
		}
		else
		{
			condition.wait (lock, [this] () {
				return !pending_frontiers.empty () || stopped;
			});
		}
	}
}

void nano::bootstrap_ascending::service::process_frontiers (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers)
{
	// Accounts with outdated frontiers to sync
	std::deque<nano::account> result;
	{
		auto transaction = ledger.tx_begin_read ();
		for (auto const & [account, frontier] : frontiers)
		{
			if (!ledger.any.block_exists_or_pruned (transaction, frontier))
			{
				result.push_back (account);
			}
		}
	}

	stats.add (nano::stat::type::bootstrap_ascending, nano::stat::detail::frontier_processed, frontiers.size ());
	stats.add (nano::stat::type::bootstrap_ascending, nano::stat::detail::frontier_outdated, result.size ());

	nano::lock_guard<nano::mutex> guard{ mutex };

	for (auto const & account : result)
	{
		accounts.priority_set (account);
	}
}

void nano::bootstrap_ascending::service::cleanup_and_sync ()
{
	debug_assert (!mutex.try_lock ());

	scoring.sync (network.list ());
	scoring.timeout ();

	throttle.resize (compute_throttle_size ());

	auto const cutoff = std::chrono::steady_clock::now () - config.bootstrap_ascending.request_timeout;
	auto should_timeout = [cutoff] (async_tag const & tag) {
		return tag.timestamp < cutoff;
	};

	auto & tags_by_order = tags.get<tag_sequenced> ();
	while (!tags_by_order.empty () && should_timeout (tags_by_order.front ()))
	{
		auto tag = tags_by_order.front ();
		tags_by_order.pop_front ();
		on_timeout.notify (tag);
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::timeout);
	}

	if (sync_dependencies_interval.elapsed (60s))
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::sync_dependencies);
		accounts.sync_dependencies ();
	}
}

void nano::bootstrap_ascending::service::run_timeouts ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::loop_cleanup);
		cleanup_and_sync ();
		condition.wait_for (lock, 5s, [this] () { return stopped; });
	}
}

void nano::bootstrap_ascending::service::process (nano::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> const & channel)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	// Only process messages that have a known tag
	auto it = tags.get<tag_id> ().find (message.id);
	if (it == tags.get<tag_id> ().end ())
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::missing_tag);
		return;
	}

	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::reply);

	auto tag = *it;
	tags.get<tag_id> ().erase (it); // Iterator is invalid after this point

	// Verifies that response type corresponds to our query
	struct payload_verifier
	{
		query_type type;

		bool operator() (const nano::asc_pull_ack::blocks_payload & response) const
		{
			return type == query_type::blocks_by_hash || type == query_type::blocks_by_account;
		}
		bool operator() (const nano::asc_pull_ack::account_info_payload & response) const
		{
			return type == query_type::account_info_by_hash;
		}
		bool operator() (const nano::asc_pull_ack::frontiers_payload & response) const
		{
			return type == query_type::frontiers;
		}
		bool operator() (const nano::empty_payload & response) const
		{
			return false; // Should not happen
		}
	};

	bool valid = std::visit (payload_verifier{ tag.type }, message.payload);
	if (!valid)
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::invalid_response_type);
		return;
	}

	// Track bootstrap request response time
	stats.inc (nano::stat::type::bootstrap_ascending_reply, to_stat_detail (tag.type));
	stats.sample (nano::stat::sample::bootstrap_tag_duration, nano::log::milliseconds_delta (tag.timestamp), { 0, config.bootstrap_ascending.request_timeout.count () });

	scoring.received_message (channel);

	lock.unlock ();

	on_reply.notify (tag);

	// Process the response payload
	std::visit ([this, &tag] (auto && request) { return process (request, tag); }, message.payload);

	condition.notify_all ();
}

void nano::bootstrap_ascending::service::process (const nano::asc_pull_ack::blocks_payload & response, const async_tag & tag)
{
	debug_assert (tag.type == query_type::blocks_by_hash || tag.type == query_type::blocks_by_account);

	stats.inc (nano::stat::type::bootstrap_ascending_process, nano::stat::detail::blocks);

	auto result = verify (response, tag);
	switch (result)
	{
		case verify_result::ok:
		{
			stats.inc (nano::stat::type::bootstrap_ascending_verify_blocks, nano::stat::detail::ok);
			stats.add (nano::stat::type::bootstrap_ascending, nano::stat::detail::blocks, nano::stat::dir::in, response.blocks.size ());

			auto blocks = response.blocks;

			// Avoid re-processing the block we already have
			release_assert (blocks.size () >= 1);
			if (blocks.front ()->hash () == tag.start.as_block_hash ())
			{
				blocks.pop_front ();
			}

			for (auto const & block : blocks)
			{
				if (block == blocks.back ())
				{
					// It's the last block submitted for this account chanin, reset timestamp to allow more requests
					block_processor.add (block, nano::block_source::bootstrap, nullptr, [this, account = tag.account] (auto result) {
						stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::timestamp_reset);
						{
							nano::lock_guard<nano::mutex> guard{ mutex };
							accounts.timestamp_reset (account);
						}
						condition.notify_all ();
					});
				}
				else
				{
					block_processor.add (block, nano::block_source::bootstrap);
				}
			}

			if (tag.source == query_source::database)
			{
				nano::lock_guard<nano::mutex> lock{ mutex };
				throttle.add (true);
			}
		}
		break;
		case verify_result::nothing_new:
		{
			stats.inc (nano::stat::type::bootstrap_ascending_verify_blocks, nano::stat::detail::nothing_new);

			nano::lock_guard<nano::mutex> lock{ mutex };
			accounts.priority_down (tag.account);
			if (tag.source == query_source::database)
			{
				throttle.add (false);
			}
		}
		break;
		case verify_result::invalid:
		{
			stats.inc (nano::stat::type::bootstrap_ascending_verify_blocks, nano::stat::detail::invalid);
		}
		break;
	}
}

void nano::bootstrap_ascending::service::process (const nano::asc_pull_ack::account_info_payload & response, const async_tag & tag)
{
	debug_assert (tag.type == query_type::account_info_by_hash);
	debug_assert (!tag.hash.is_zero ());

	if (response.account.is_zero ())
	{
		stats.inc (nano::stat::type::bootstrap_ascending_process, nano::stat::detail::account_info_empty);
		return;
	}

	stats.inc (nano::stat::type::bootstrap_ascending_process, nano::stat::detail::account_info);

	// Prioritize account containing the dependency
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		accounts.dependency_update (tag.hash, response.account);
		accounts.priority_set (response.account);
	}
}

void nano::bootstrap_ascending::service::process (const nano::asc_pull_ack::frontiers_payload & response, const async_tag & tag)
{
	debug_assert (tag.type == query_type::frontiers);
	debug_assert (!tag.start.is_zero ());

	if (response.frontiers.empty ())
	{
		stats.inc (nano::stat::type::bootstrap_ascending_process, nano::stat::detail::frontiers_empty);
		return;
	}

	stats.inc (nano::stat::type::bootstrap_ascending_process, nano::stat::detail::frontiers);

	auto result = verify (response, tag);
	switch (result)
	{
		case verify_result::ok:
		{
			stats.inc (nano::stat::type::bootstrap_ascending_verify_frontiers, nano::stat::detail::ok);
			stats.add (nano::stat::type::bootstrap_ascending, nano::stat::detail::frontiers, nano::stat::dir::in, response.frontiers.size ());

			nano::lock_guard<nano::mutex> lock{ mutex };

			frontiers.process (tag.start.as_account (), response.frontiers.back ().first);
			pending_frontiers.push_back (response.frontiers);
		}
		break;
		case verify_result::nothing_new:
		{
			stats.inc (nano::stat::type::bootstrap_ascending_verify_frontiers, nano::stat::detail::nothing_new);
		}
		break;
		case verify_result::invalid:
		{
			stats.inc (nano::stat::type::bootstrap_ascending_verify_frontiers, nano::stat::detail::invalid);
		}
		break;
	}
}

void nano::bootstrap_ascending::service::process (const nano::empty_payload & response, const async_tag & tag)
{
	stats.inc (nano::stat::type::bootstrap_ascending_process, nano::stat::detail::empty);
	debug_assert (false, "empty payload"); // Should not happen
}

nano::bootstrap_ascending::service::verify_result nano::bootstrap_ascending::service::verify (const nano::asc_pull_ack::blocks_payload & response, const nano::bootstrap_ascending::service::async_tag & tag) const
{
	auto const & blocks = response.blocks;

	if (blocks.empty ())
	{
		return verify_result::nothing_new;
	}
	if (blocks.size () == 1 && blocks.front ()->hash () == tag.start.as_block_hash ())
	{
		return verify_result::nothing_new;
	}
	if (blocks.size () > tag.count)
	{
		return verify_result::invalid;
	}

	auto const & first = blocks.front ();
	switch (tag.type)
	{
		case query_type::blocks_by_hash:
		{
			if (first->hash () != tag.start.as_block_hash ())
			{
				// TODO: Stat & log
				return verify_result::invalid;
			}
		}
		break;
		case query_type::blocks_by_account:
		{
			// Open & state blocks always contain account field
			if (first->account_field () != tag.start.as_account ())
			{
				// TODO: Stat & log
				return verify_result::invalid;
			}
		}
		break;
		default:
			return verify_result::invalid;
	}

	// Verify blocks make a valid chain
	nano::block_hash previous_hash = blocks.front ()->hash ();
	for (int n = 1; n < blocks.size (); ++n)
	{
		auto & block = blocks[n];
		if (block->previous () != previous_hash)
		{
			// TODO: Stat & log
			return verify_result::invalid; // Blocks do not make a chain
		}
		previous_hash = block->hash ();
	}

	return verify_result::ok;
}

nano::bootstrap_ascending::service::verify_result nano::bootstrap_ascending::service::verify (nano::asc_pull_ack::frontiers_payload const & response, async_tag const & tag) const
{
	auto const & frontiers = response.frontiers;

	if (frontiers.empty ())
	{
		return verify_result::nothing_new;
	}

	// Ensure frontiers accounts are in ascending order
	nano::account previous{ 0 };
	for (auto const & [account, _] : frontiers)
	{
		if (account.number () <= previous.number ())
		{
			return verify_result::invalid;
		}
		previous = account;
	}

	// Ensure the frontiers are larger or equal to the requested frontier
	if (frontiers.front ().first.number () < tag.start.as_account ().number ())
	{
		return verify_result::invalid;
	}

	return verify_result::ok;
}

auto nano::bootstrap_ascending::service::info () const -> nano::bootstrap_ascending::account_sets::info_t
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.info ();
}

std::size_t nano::bootstrap_ascending::service::compute_throttle_size () const
{
	// Scales logarithmically with ledger block
	// Returns: config.throttle_coefficient * sqrt(block_count)
	std::size_t size_new = config.bootstrap_ascending.throttle_coefficient * std::sqrt (ledger.block_count ());
	return size_new == 0 ? 16 : size_new;
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::service::collect_container_info (std::string const & name)
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "tags", tags.size (), sizeof (decltype (tags)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "throttle", throttle.size (), 0 }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "throttle_successes", throttle.successes (), 0 }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "pending_frontiers", pending_frontiers.size (), sizeof (decltype (pending_frontiers)::value_type) }));
	composite->add_component (accounts.collect_container_info ("accounts"));
	composite->add_component (frontiers.collect_container_info ("frontiers"));
	return composite;
}

/*
 *
 */

nano::stat::detail nano::bootstrap_ascending::to_stat_detail (nano::bootstrap_ascending::service::query_type type)
{
	return nano::enum_util::cast<nano::stat::detail> (type);
}