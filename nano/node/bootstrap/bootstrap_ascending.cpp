#include <nano/node/bootstrap/block_deserializer.hpp>
#include <nano/node/bootstrap/bootstrap_ascending.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/secure/common.hpp>

#include <boost/format.hpp>

using namespace std::chrono_literals;

/*
 * account_sets
 */

nano::bootstrap_ascending::account_sets::account_sets (nano::node & node_a, nano::store & store_a, nano::stat & stats_a) :
	node{ node_a },
	store{ store_a },
	stats{ stats_a }
{
}

nano::bootstrap_ascending::account_sets::~account_sets ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::bootstrap_ascending::account_sets::start ()
{
	//	seed ();

	thread = std::thread ([this] () {
		// TODO: Unique thread name
		nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
		run ();
	});
}

void nano::bootstrap_ascending::account_sets::stop ()
{
	debug_assert (thread.joinable ());

	stopped = true;
	thread.join ();
}

void nano::bootstrap_ascending::account_sets::prioritize (nano::account const & account, float priority)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	if (blocking.count (account) == 0)
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::prioritize);

//		forwarding.insert (account);
		auto iter = backoff.find (account);
		if (iter == backoff.end ())
		{
			backoff.emplace (account, backoff_entry{ priority, 0 });
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::prioritize_failed);
	}
}

void nano::bootstrap_ascending::account_sets::block (nano::account const & account, nano::block_hash const & dependency)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::block);

	backoff.erase (account);
	forwarding.erase (account);
	blocking[account] = dependency;
}

void nano::bootstrap_ascending::account_sets::unblock (nano::account const & account, nano::block_hash const & hash)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	// Unblock only if the dependency is fulfilled
	if (blocking.count (account) > 0 && blocking[account] == hash)
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::unblock);

		blocking.erase (account);
		backoff[account] = {}; // Reset backoff and cooldown
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::unblock_failed);
	}
}

void nano::bootstrap_ascending::account_sets::force_unblock (const nano::account & account)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	blocking.erase (account);
	backoff[account] = {}; // Reset backoff and cooldown
}

std::vector<double> nano::bootstrap_ascending::account_sets::probability_transform (std::vector<decltype (backoff)::mapped_type> const & attempts) const
{
	std::vector<double> result;
	result.reserve (attempts.size ());
	for (auto i = attempts.begin (), n = attempts.end (); i != n; ++i)
	{
		result.push_back (1.0 / std::pow (2.0, i->backoff));
	}
	return result;
}

std::optional<nano::account> nano::bootstrap_ascending::account_sets::random ()
{
	debug_assert (!backoff.empty ());

	std::vector<decltype (backoff)::mapped_type> attempts;
	std::vector<nano::account> candidates;

	// Ensure we do not spin forever if no suitable candidate can be found
	int iterations = 0;
	while (candidates.size () < backoff_exclusion && iterations++ < backoff_exclusion * 10)
	{
		debug_assert (candidates.size () == attempts.size ());

		nano::account search;
		nano::random_pool::generate_block (search.bytes.data (), search.bytes.size ());
		auto iter = backoff.lower_bound (search);
		if (iter == backoff.end ())
		{
			iter = backoff.begin ();
		}

		auto const [account, entry] = *iter;
		// Ensure there is spacing between requests for the same account
		if (nano::milliseconds_since_epoch () - entry.last_request > cooldown)
		{
			attempts.push_back (entry);
			candidates.push_back (account);
		}
	}

	if (candidates.empty ())
	{
		return std::nullopt; // All accounts are inside cooldown period
	}

	auto weights = probability_transform (attempts);
	std::discrete_distribution dist{ weights.begin (), weights.end () };
	auto selection = dist (rng);
	debug_assert (!weights.empty () && selection < weights.size ());
	auto result = candidates[selection];
	return result;
}

std::optional<nano::account> nano::bootstrap_ascending::account_sets::next ()
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	std::optional<nano::account> result;
	if (!forwarding.empty ())
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::next_forwarding);

		auto iter = forwarding.begin ();
		result = *iter;
		forwarding.erase (iter);
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::next_random);

		result = random ();
	}
	if (result)
	{
		auto & entry = backoff[*result];
		entry.backoff += 1.0f;
		entry.last_request = nano::milliseconds_since_epoch ();
	}
	return result;
}

bool nano::bootstrap_ascending::account_sets::blocked (nano::account const & account) const
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };
	return blocking.count (account) > 0;
}

bool nano::bootstrap_ascending::account_sets::exists (const nano::account & account) const
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };
	return backoff.count (account) > 0 || blocking.count (account) > 0;
}

void nano::bootstrap_ascending::account_sets::run ()
{
	while (!stopped)
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::loop);

		resample ();

		std::this_thread::sleep_for (backoff.size () < max_backoff_size ? 100ms : 10s);
	}
}

void nano::bootstrap_ascending::account_sets::resample ()
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	// Evict surplus backoff entries
	while (backoff.size () > max_backoff_size)
	{
		// Find account with the worst backoff
		nano::account candidate{ 0 };
		{
			float worst_backoff = 0;

			int iterations = 0;
			while (iterations++ < backoff_exclusion * 10)
			{
				nano::account search;
				nano::random_pool::generate_block (search.bytes.data (), search.bytes.size ());
				auto iter = backoff.lower_bound (search);
				if (iter == backoff.end ())
				{
					iter = backoff.begin ();
				}

				auto const [account, entry] = *iter;
				if (entry.backoff > worst_backoff)
				{
					candidate = account;
				}
			}
		}

		if (!candidate.is_zero ())
		{
			stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::erase);

			backoff.erase (candidate);
		}
		else
		{
			// If no candidate was found and backoff is not empty then something went wrong
			debug_assert ("should not happen");
		}
	}

	// Randomly sample additional accounts from disk
	{
		uint64_t account_count = 0;
		uint64_t receivable_count = 0;

		nano::account search;
		nano::random_pool::generate_block (search.bytes.data (), search.bytes.size ());

		const std::size_t sample_count = 1024 * 16;

		auto tx = store.tx_begin_read ();
		for (auto i = store.account.begin (tx, search), n = store.account.end (); i != n && account_count < sample_count; ++i)
		{
			if (!exists (i->first))
			{
				stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::sample_account);

				prioritize (i->first, 0.0f);
				account_count++;
			}
		}
		for (auto i = store.pending.begin (tx, nano::pending_key (search, 0)), n = store.pending.end (); i != n && receivable_count < sample_count; ++i)
		{
			if (!exists (i->first.key ()))
			{
				stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::sample_pending);

				prioritize (i->first.key (), 0.0f);
				receivable_count++;
			}
		}
	}
}

nano::bootstrap_ascending::account_sets::backoff_info_t nano::bootstrap_ascending::account_sets::backoff_info () const
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };
	return { forwarding, blocking, backoff };
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::account_sets::collect_container_info (const std::string & name)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "backoff", backoff.size (), sizeof (decltype (backoff)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "forwarding", forwarding.size (), sizeof (decltype (forwarding)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocking", blocking.size (), sizeof (decltype (blocking)::value_type) }));
	return composite;
}

/*
 * bootstrap_ascending
 */

nano::bootstrap_ascending::bootstrap_ascending (nano::node & node_a, nano::store & store_a, nano::block_processor & block_processor_a, nano::ledger & ledger_a, nano::network & network_a, nano::stat & stat_a) :
	node{ node_a },
	store{ store_a },
	block_processor{ block_processor_a },
	ledger{ ledger_a },
	network{ network_a },
	stats{ stat_a },
	accounts{ node, store, stats }
{
	block_processor.processed.add ([this] (nano::transaction const & tx, nano::process_return const & result, nano::block const & block) {
		inspect (tx, result, block);
	});

	//	on_timeout.add ([this] (auto tag) {
	//		std::cout << "timeout: " << tag.id
	//				  << " | "
	//				  << "count: " << tags.size ()
	//				  << std::endl;
	//	});
	//
	//	on_request.add ([this] (auto tag, auto channel) {
	//		std::cout << "requesting: " << tag.id
	//				  << " | "
	//				  << "channel: " << channel->to_string ()
	//				  << std::endl;
	//	});
}

nano::bootstrap_ascending::~bootstrap_ascending ()
{
	// All threads must be stopped before destruction
	debug_assert (threads.empty ());
	debug_assert (!timeout_thread.joinable ());
}

void nano::bootstrap_ascending::start ()
{
	accounts.start ();

	debug_assert (threads.empty ());
	debug_assert (!timeout_thread.joinable ());

	// TODO: Use value read from node config
	const std::size_t thread_count = 2;

	for (int n = 0; n < thread_count; ++n)
	{
		threads.emplace_back ([this] () {
			nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
			run ();
		});
	}

	timeout_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
		run_timeouts ();
	});
}

void nano::bootstrap_ascending::stop ()
{
	stopped = true;

	for (auto & thread : threads)
	{
		debug_assert (thread.joinable ());
		thread.join ();
	}
	threads.clear ();

	nano::join_or_pass (timeout_thread);

	accounts.stop ();
}

uint64_t nano::bootstrap_ascending::generate_id () const
{
	id_t id;
	nano::random_pool::generate_block (reinterpret_cast<uint8_t *> (&id), sizeof (id));
	return id;
}

void nano::bootstrap_ascending::send (std::shared_ptr<nano::transport::channel> channel, async_tag tag)
{
	nano::asc_pull_req request{ node.network_params.network };
	request.id = tag.id;
	request.type = nano::asc_pull_type::blocks;

	nano::asc_pull_req::blocks_payload request_payload;
	request_payload.start = tag.start;
	request_payload.count = nano::bootstrap_server::max_blocks;

	request.payload = request_payload;
	request.update_header ();

	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::request, nano::stat::dir::out);

	//		std::cout << "requesting: " << std::setw (28) << tag.id
	//			  << " | "
	//			  << "channel: " << channel->to_string ()
	//			  << std::endl;

	channel->send (
	request, [this, tag] (boost::system::error_code const & ec, std::size_t size) {
		if (ec)
		{
			std::cerr << "send error: " << ec.message () << std::endl;
		}
	},
	nano::buffer_drop_policy::no_limiter_drop, nano::bandwidth_limit_type::bootstrap);
}

/** Inspects a block that has been processed by the block processor
- Marks an account as blocked if the result code is gap source as there is no reason request additional blocks for this account until the dependency is resolved
- Marks an account as forwarded if it has been recently referenced by a block that has been inserted.
 */
void nano::bootstrap_ascending::inspect (nano::transaction const & tx, nano::process_return const & result, nano::block const & block)
{
	auto const hash = block.hash ();

	switch (result.code)
	{
		case nano::process_result::progress:
		{
			const auto account = ledger.account (tx, hash);
			const auto is_send = ledger.is_send (tx, block);

			nano::lock_guard<nano::mutex> lock{ mutex };

			// If we've inserted any block in to an account, unmark it as blocked
			accounts.force_unblock (account);
			// Forward and initialize backoff value with 0.0 for the current account
			// 0.0 has the highest priority
			accounts.prioritize (account, 0.0f);

			if (is_send)
			{
				// Initialize with value of 1.0 a value of lower priority than an account itselt
				// This is the same priority as if it had already made 1 attempt.
				auto const send_factor = 1.0f;

				switch (block.type ())
				{
					// Forward and initialize backoff for the referenced account
					case nano::block_type::send:
						accounts.unblock (block.destination (), hash);
						accounts.prioritize (block.destination (), send_factor);
						break;
					case nano::block_type::state:
						accounts.unblock (block.link ().as_account (), hash);
						accounts.prioritize (block.link ().as_account (), send_factor);
						break;
					default:
						debug_assert (false);
						break;
				}
			}
			break;
		}
		case nano::process_result::gap_source:
		{
			const auto account = block.previous ().is_zero () ? block.account () : ledger.account (tx, block.previous ());
			const auto source = block.source ().is_zero () ? block.link ().as_block_hash () : block.source ();

			nano::lock_guard<nano::mutex> lock{ mutex };
			// Mark account as blocked because it is missing the source block
			accounts.block (account, source);
			break;
		}
		case nano::process_result::gap_previous:
			break;
		default:
			break;
	}
}

void nano::bootstrap_ascending::wait_blockprocessor () const
{
	while (!stopped && block_processor.half_full ())
	{
		std::this_thread::sleep_for (500ms); // Blockprocessor is relatively slow, sleeping here instead of using conditions
	}
}

void nano::bootstrap_ascending::wait_available_request () const
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	condition.wait (lock, [this] () { return stopped || tags.size () < requests_max; });
}

std::shared_ptr<nano::transport::channel> nano::bootstrap_ascending::available_channel ()
{
	auto channels = network.random_set (32, node.network_params.network.bootstrap_protocol_version_min, /* include temporary channels */ true);
	for (auto & channel : channels)
	{
		if (!channel->max ())
		{
			return channel;
		}
	}
	return nullptr;
}

std::shared_ptr<nano::transport::channel> nano::bootstrap_ascending::wait_available_channel ()
{
	std::shared_ptr<nano::transport::channel> channel;
	// Wait until a channel is available
	while (!stopped && !(channel = available_channel ()))
	{
		std::this_thread::sleep_for (100ms);
	}
	return channel;
}

nano::account nano::bootstrap_ascending::wait_available_account ()
{
	while (!stopped)
	{
		nano::unique_lock<nano::mutex> lock{ mutex };

		if (auto maybe_account = accounts.next ())
		{
			return *maybe_account;
		}

		condition.wait_for (lock, 100ms);
	}
	return {};
}

bool nano::bootstrap_ascending::request (nano::account & account, std::shared_ptr<nano::transport::channel> & channel)
{
	nano::hash_or_account start = account;

	nano::account_info info;
	// check if the account picked has blocks, if it does, start the pull from the highest block
	if (!store.account.get (store.tx_begin_read (), account, info))
	{
		start = info.head;
	}

	const async_tag tag{ generate_id (), start, nano::milliseconds_since_epoch () };

	on_request.notify (tag, channel);

	track (tag);
	send (channel, tag);

	return true; // Request sent
}

bool nano::bootstrap_ascending::request_one ()
{
	// Ensure there is enough space in blockprocessor for queuing new blocks
	wait_blockprocessor ();

	// Do not do too many requests in parallel, impose throttling
	wait_available_request ();

	auto channel = wait_available_channel ();
	if (!channel)
	{
		return false;
	}

	auto account = wait_available_account ();
	if (account.is_zero ())
	{
		return false;
	}

	bool success = request (account, channel);
	return success;
}

void nano::bootstrap_ascending::run ()
{
	while (!stopped)
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::loop);

		request_one ();
	}
}

void nano::bootstrap_ascending::run_timeouts ()
{
	while (!stopped)
	{
		std::this_thread::sleep_for (1s);

		{
			nano::lock_guard<nano::mutex> lock{ mutex };

			const nano::millis_t threshold = 5 * 1000;

			auto & tags_by_order = tags.get<tag_sequenced> ();
			while (!tags_by_order.empty () && nano::time_difference (tags_by_order.front ().time, nano::milliseconds_since_epoch ()) > threshold)
			{
				auto tag = tags_by_order.front ();
				tags_by_order.pop_front ();
				on_timeout.notify (tag);
				stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::timeout);
			}
		}

		condition.notify_all ();
	}
}

void nano::bootstrap_ascending::process (const nano::asc_pull_ack & message)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	// Only process messages that have a known tag
	auto & tags_by_id = tags.get<tag_id> ();
	if (tags_by_id.count (message.id) > 0)
	{
		auto iterator = tags_by_id.find (message.id);
		auto tag = *iterator;
		tags_by_id.erase (iterator);

		lock.unlock ();
		condition.notify_all ();

		on_reply.notify (tag);

		return std::visit ([this, &tag] (auto && request) { return process (request, tag); }, message.payload);
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::missing_tag);
	}
}

void nano::bootstrap_ascending::process (const nano::asc_pull_ack::blocks_payload & response, const nano::bootstrap_ascending::async_tag & tag)
{
	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::reply);

	// Continue only if there are any blocks to process
	if (response.blocks.empty ())
	{
		return;
	}

	if (verify (response, tag))
	//	if (true)
	{
		stats.add (nano::stat::type::bootstrap_ascending, nano::stat::detail::blocks, nano::stat::dir::in, response.blocks.size ());

		for (auto & block : response.blocks)
		{
			block_processor.add (block);
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::bad_sender);
	}
}

void nano::bootstrap_ascending::process (const nano::asc_pull_ack::account_info_payload & response, const nano::bootstrap_ascending::async_tag & tag)
{
	// TODO: Make use of account info
}

void nano::bootstrap_ascending::process (const nano::empty_payload & response, const nano::bootstrap_ascending::async_tag & tag)
{
	// Should not happen
	debug_assert (false, "empty payload");
}

bool nano::bootstrap_ascending::verify (const nano::asc_pull_ack::blocks_payload & response, const nano::bootstrap_ascending::async_tag & tag) const
{
	debug_assert (!response.blocks.empty ());

	auto first = response.blocks.front ();
	// The `start` field should correspond to either previous block or account
	if (first->hash () == tag.start)
	{
		// Pass
	}
	// Open & state blocks always contain account field
	else if (first->account () == tag.start)
	{
		// Pass
	}
	else
	{
		// TODO: Stat & log
		//		std::cerr << "bad head" << std::endl;
		return false; // Bad head block
	}

	// Verify blocks make a valid chain
	nano::block_hash previous_hash = response.blocks.front ()->hash ();
	for (int n = 1; n < response.blocks.size (); ++n)
	{
		auto & block = response.blocks[n];
		if (block->previous () != previous_hash)
		{
			// TODO: Stat & log
			//			std::cerr << "bad previous" << std::endl;
			return false; // Blocks do not make a chain
		}
		previous_hash = block->hash ();
	}

	return true; // Pass verification
}

void nano::bootstrap_ascending::track (async_tag const & tag)
{
	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::track);

	nano::lock_guard<nano::mutex> lock{ mutex };
	tags.get<tag_id> ().insert (tag);
}

nano::bootstrap_ascending::account_sets::backoff_info_t nano::bootstrap_ascending::backoff_info () const
{
	return accounts.backoff_info ();
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::collect_container_info (std::string const & name)
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (accounts.collect_container_info ("accounts"));
	return composite;
}