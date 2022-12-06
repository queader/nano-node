#include <nano/node/bootstrap/block_deserializer.hpp>
#include <nano/node/bootstrap/bootstrap_ascending.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/secure/common.hpp>

#include <boost/format.hpp>

#include <algorithm>

using namespace std::chrono_literals;

/*
 * account_sets::iterator
 */

nano::bootstrap_ascending::account_sets::iterator_t::iterator_t (nano::store & store) :
	store{ store }
{
}

nano::account nano::bootstrap_ascending::account_sets::iterator_t::operator* () const
{
	return current;
}

void nano::bootstrap_ascending::account_sets::iterator_t::next (nano::transaction & tx)
{
	switch (table)
	{
		case table_t::account:
		{
			auto i = current.number () + 1;
			auto item = store.account.begin (tx, i);
			if (item != store.account.end ())
			{
				current = item->first;
			}
			else
			{
				table = table_t::pending;
				current = { 0 };
			}
			break;
		}
		case table_t::pending:
		{
			auto i = current.number () + 1;
			auto item = store.pending.begin (tx, nano::pending_key{ i, 0 });
			if (item != store.pending.end ())
			{
				current = item->first.account;
			}
			else
			{
				table = table_t::account;
				current = { 0 };
			}
			break;
		}
	}
}

/*
 * account_sets
 */

nano::bootstrap_ascending::account_sets::account_sets (nano::stat & stats_a, nano::store & store_a) :
	stats{ stats_a },
	store{ store_a },
	iter{ store }
{
}

void nano::bootstrap_ascending::account_sets::advance (const nano::account & account)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	unblock (account);
	priority_up (account);
	timestamp (account, 0); // Reset timestamp
}

void nano::bootstrap_ascending::account_sets::send (const nano::account & account, const nano::block_hash & source)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	unblock (account, source);
	priority_up (account, /* only insert new priority entry */ 0.0f);
}

void nano::bootstrap_ascending::account_sets::suppress (const nano::account & account)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	priority_down (account);
}

void nano::bootstrap_ascending::account_sets::dump () const
{
	//	std::cerr << boost::str (boost::format ("Blocking: %1%\n") % blocking.size ());
	//	std::deque<size_t> weight_counts;
	//	float max = 0.0f;
	//	for (auto const & [account, priority] : priorities)
	//	{
	//		auto count = std::log2 (std::max (priority, 1.0f));
	//		if (weight_counts.size () <= count)
	//		{
	//			weight_counts.resize (count + 1);
	//		}
	//		++weight_counts[count];
	//		max = std::max (max, priority);
	//	}
	//	std::string output;
	//	output += "Priorities hist (max: " + std::to_string (max) + " size: " + std::to_string (priorities.size ()) + "): ";
	//	for (size_t i = 0, n = weight_counts.size (); i < n; ++i)
	//	{
	//		output += std::to_string (weight_counts[i]) + ' ';
	//	}
	//	output += '\n';
	//	std::cerr << output;
}

void nano::bootstrap_ascending::account_sets::priority_up (nano::account const & account, float priority_increase)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	auto blocking_iter = blocking.find (account);
	if (blocking_iter == blocking.end ())
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::prioritize);

		//		const float priority_increase = 0.4f;
		const float priority_initial = 1.4f;
		const float priority_max = 64.0f;

		auto iter = priorities.get<tag_account> ().find (account);
		if (iter != priorities.get<tag_account> ().end ())
		{
			priorities.get<tag_account> ().modify (iter, [priority_increase, priority_max] (auto & entry) {
				entry.priority = std::min (entry.priority + priority_increase, priority_max);
			});
		}
		else
		{
			priorities.get<tag_account> ().insert ({ bootstrap_ascending::generate_id (), account, priority_initial, 0 });

			// Erase oldest entry
			if (max_priorities_size > 0 && priorities.size () > max_priorities_size)
			{
				priorities.get<tag_sequenced> ().pop_front ();
			}
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::prioritize_failed);
	}
}

void nano::bootstrap_ascending::account_sets::priority_down (nano::account const & account)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		auto priority_new = iter->priority / 2.0f;
		if (priority_new <= priority_cutoff)
		{
			priorities.get<tag_account> ().erase (iter);
		}
		else
		{
			priorities.get<tag_account> ().modify (iter, [priority_new] (auto & entry) {
				entry.priority = priority_new;
			});
		}
	}
	else
	{
	}
}

void nano::bootstrap_ascending::account_sets::block (nano::account const & account, nano::block_hash const & dependency)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::block);

	//	auto existing = priorities.get<tag_account> ().find (account);
	//	auto count = existing == priorities.get<tag_account> ().end () ? 1.0f : existing->priority;

	priorities.get<tag_account> ().erase (account);
	//	blocking[account] = std::make_pair (dependency, count);
	blocking[account] = dependency;
}

bool nano::bootstrap_ascending::account_sets::unblock (nano::account const & account, std::optional<nano::block_hash> const & source)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	auto existing = blocking.find (account);
	// Unblock only if the dependency is fulfilled
	if (existing != blocking.end () && (!source || existing->second == *source))
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::unblock);
		blocking.erase (account);
		return true; // Unblocked
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::unblock_failed);
		return false;
	}
}

void nano::bootstrap_ascending::account_sets::timestamp (const nano::account & account, nano::millis_t time)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		priorities.get<tag_account> ().modify (iter, [time] (auto & entry) {
			entry.last_request = time;
		});
	}
}

nano::account nano::bootstrap_ascending::account_sets::next ()
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	static std::size_t counter = 0;

	bool use_database = priorities.empty () || (counter++) % 2;
	if (use_database)
	{
		return next_database ();
	}
	else
	{
		return next_prioritization ();
	}
}

nano::account nano::bootstrap_ascending::account_sets::next_prioritization ()
{
	debug_assert (!priorities.empty ());

	stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::next_prioritization);

	std::vector<float> weights;
	std::vector<nano::account> candidates;

	int iterations = 0;
	while (candidates.size () < account_sets::consideration_count && iterations++ < account_sets::consideration_count * 10)
	{
		debug_assert (candidates.size () == weights.size ());

		auto search = generate_id ();

		auto iter = priorities.get<tag_id> ().lower_bound (search);
		if (iter == priorities.get<tag_id> ().end ())
		{
			iter = priorities.get<tag_id> ().begin ();
		}

		// Ensure there is enough spacing between requests for the same account
		auto const entry = *iter;
		if (nano::milliseconds_since_epoch () - entry.last_request > cooldown)
		{
			candidates.push_back (entry.account);
			weights.push_back (entry.priority);
		}
	}

	if (candidates.empty ())
	{
		return { 0 }; // All sampled accounts are inside cooldown period
	}

	std::discrete_distribution dist{ weights.begin (), weights.end () };
	auto selection = dist (rng);
	debug_assert (!weights.empty () && selection < weights.size ());
	auto result = candidates[selection];

	// Set timestamp to not query the same account too quickly
	timestamp (result, nano::milliseconds_since_epoch ());

	return result;
}

nano::account nano::bootstrap_ascending::account_sets::next_database ()
{
	stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::next_database);

	auto tx = store.tx_begin_read ();
	iter.next (tx);
	return *iter;
}

bool nano::bootstrap_ascending::account_sets::blocked (nano::account const & account) const
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };
	return blocking.count (account) > 0;
}

std::size_t nano::bootstrap_ascending::account_sets::priority_size () const
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };
	return priorities.size ();
}

std::size_t nano::bootstrap_ascending::account_sets::blocked_size () const
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };
	return blocking.size ();
}

float nano::bootstrap_ascending::account_sets::priority (nano::account const & account) const
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };
	if (blocked (account))
	{
		return 0.0f;
	}
	auto existing = priorities.get<tag_account> ().find (account);
	if (existing != priorities.get<tag_account> ().end ())
	{
		return existing->priority;
	}
	return 1.0f;
}

auto nano::bootstrap_ascending::account_sets::info () const -> info_t
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	std::vector<nano::account> blocking_list;
	std::transform (blocking.begin (), blocking.end (), std::back_inserter (blocking_list), [] (auto const & entry) { return entry.first; });

	std::vector<priority_entry> priorities_list;
	std::transform (priorities.begin (), priorities.end (), std::back_inserter (priorities_list), [] (auto const & entry) { return entry; });

	//	return { blocking_list, priorities_list };
	return { blocking, priorities_list };
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::account_sets::collect_container_info (const std::string & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "priorities", priorities.size (), sizeof (decltype (priorities)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocking", blocking.size (), sizeof (decltype (blocking)::value_type) }));
	return composite;
}

void nano::bootstrap_ascending::account_sets::stat (const nano::account & account, stat_type type, std::size_t increase)
{
	nano::lock_guard<std::recursive_mutex> guard{ mutex };

	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		priorities.get<tag_account> ().modify (iter, [type, increase] (priority_entry & entry) {
			switch (type)
			{
				case stat_type::request:
					entry.stats.request += increase;
					break;
				case stat_type::timeout:
					entry.stats.timeout += increase;
					break;
				case stat_type::reply:
					entry.stats.reply += increase;
					break;
				case stat_type::old:
					entry.stats.old += increase;
					break;
				case stat_type::blocks:
					entry.stats.blocks += increase;
					break;
				case stat_type::progress:
					entry.stats.progress += increase;
					break;
				case stat_type::gap_source:
					entry.stats.gap_source += increase;
					break;
				case stat_type::gap_previous:
					entry.stats.gap_previous += increase;
					break;
				case stat_type::corrupt:
					entry.stats.corrupt += increase;
					break;
				case stat_type::nothing_new:
					entry.stats.nothing_new += increase;
					break;
				case stat_type::wtf:
					entry.stats.wtf += increase;
					break;
			}
		});
	}
}

boost::property_tree::ptree nano::bootstrap_ascending::account_sets::priority_entry::collect_info () const
{
	boost::property_tree::ptree ptree;
	ptree.put ("account", account.to_account ());
	ptree.put ("priority", priority);
	ptree.put ("last_request", last_request);

	boost::property_tree::ptree stats_ptree;
	stats_ptree.put ("request", stats.request);
	stats_ptree.put ("timeout", stats.timeout);
	stats_ptree.put ("reply", stats.reply);
	stats_ptree.put ("old", stats.old);
	stats_ptree.put ("blocks", stats.blocks);
	stats_ptree.put ("progress", stats.progress);
	stats_ptree.put ("gap_source", stats.gap_source);
	stats_ptree.put ("gap_previous", stats.gap_previous);
	stats_ptree.put ("corrupt", stats.corrupt);
	stats_ptree.put ("nothing_new", stats.nothing_new);
	stats_ptree.put ("wtf", stats.wtf);

	ptree.add_child ("stats", stats_ptree);

	return ptree;
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
	accounts{ stats, store_a }
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
	debug_assert (threads.empty ());
	debug_assert (!timeout_thread.joinable ());

	// TODO: Use value read from node config
	const std::size_t thread_count = 4;

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
}

nano::bootstrap_ascending::id_t nano::bootstrap_ascending::generate_id () const
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
	request_payload.start_type = tag.pulling_by_account () ? nano::asc_pull_req::blocks_payload::type::account : nano::asc_pull_req::blocks_payload::type::blocks;

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

size_t nano::bootstrap_ascending::priority_size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.priority_size ();
}

size_t nano::bootstrap_ascending::blocked_size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.blocked_size ();
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
			const bool is_send = ledger.is_send (tx, block);

			//			nano::lock_guard<nano::mutex> lock{ mutex };

			// If we've inserted any block in to an account, unmark it as blocked
			accounts.advance (account);
			// Forward and initialize backoff value with 0.0 for the current account
			// 0.0 has the highest priority
			//			accounts.priority_up (account);
			//			accounts.timestamp (account, 0);

			if (is_send)
			{
				// Initialize with value of 1.0 a value of lower priority than an account itselt
				// This is the same priority as if it had already made 1 attempt.

				// TODO: Encapsulate this as a helper somewhere
				nano::account destination{ 0 };
				switch (block.type ())
				{
					// Forward and initialize backoff for the referenced account
					case nano::block_type::send:
						destination = block.destination ();
						break;
					case nano::block_type::state:
						destination = block.link ().as_account ();
						break;
					default:
						debug_assert (false, "unexpected block type");
						break;
				}
				if (!destination.is_zero ())
				{
					//					accounts.advance (destination, hash); // TODO: Do not increase priority & timestamp
					accounts.send (destination, hash);

					//					bool unblocked = accounts.unblock (destination, hash);
					//					if (unblocked)
					//					{
					//						// TODO: Move into unblock
					//						accounts.timestamp (destination, 0);
					//						accounts.priority_up (destination);
					//					}
				}
			}
			condition.notify_all (); // Notify threads waiting for account cooldown
			break;
		}
		case nano::process_result::gap_source:
		{
			const auto account = block.previous ().is_zero () ? block.account () : ledger.account (tx, block.previous ());
			const auto source = block.source ().is_zero () ? block.link ().as_block_hash () : block.source ();

			// Mark account as blocked because it is missing the source block
			accounts.block (account, source);

			break;
		}
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

nano::account nano::bootstrap_ascending::wait_available_account (nano::unique_lock<nano::mutex> & lock)
{
	while (!stopped)
	{
		auto account = accounts.next ();

		if (!account.is_zero ())
		{
			if (tags.get<tag_account> ().count (account) == 0)
			{
				return account;
			}
		}
		else
		{
			condition.wait_for (lock, 10ms);
		}
	}
	return {};
}

bool nano::bootstrap_ascending::request (nano::unique_lock<nano::mutex> & lock, nano::account & account, std::shared_ptr<nano::transport::channel> & channel)
{
	nano::account_info info;
	nano::hash_or_account start = account;

	// std::cerr << boost::str (boost::format ("req account: %1%\n") % account.to_account ());

	// Check if the account picked has blocks, if it does, start the pull from the highest block
	if (!store.account.get (store.tx_begin_read (), account, info))
	{
		start = info.head;
	}
	else
	{
	}

	const async_tag tag{ generate_id (), start, nano::milliseconds_since_epoch (), account };

	on_request.notify (tag, channel);

	track (tag);

	lock.unlock ();

	send (channel, tag);

	accounts.stat (tag.account, account_sets::stat_type::request);

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

	nano::unique_lock<nano::mutex> lock{ mutex };

	auto account = wait_available_account (lock);
	if (account.is_zero ())
	{
		return false;
	}

	bool success = request (lock, account, channel);
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

			// TODO: Add to config
			const nano::millis_t threshold = 5 * 1000;

			auto & tags_by_order = tags.get<tag_sequenced> ();
			while (!tags_by_order.empty () && nano::time_difference (tags_by_order.front ().time, nano::milliseconds_since_epoch ()) > threshold)
			{
				auto tag = tags_by_order.front ();
				tags_by_order.pop_front ();
				on_timeout.notify (tag);
				stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::timeout);
				accounts.stat (tag.account, account_sets::stat_type::timeout);
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

		condition.notify_all (); // Notify threads waiting for max parallel requests

		on_reply.notify (tag);

		std::visit ([this, &tag] (auto && request) { return process (request, tag); }, message.payload);
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::missing_tag);
	}
}

namespace
{
bool nothing_new (nano::bootstrap_ascending::async_tag const & tag, std::vector<std::shared_ptr<nano::block>> const & blocks)
{
	if (blocks.empty ())
	{
		return true;
	}
	if (blocks.size () == 1 && blocks.front ()->hash () == tag.start)
	{
		return true;
	}
	return false;
}
}

void nano::bootstrap_ascending::process (const nano::asc_pull_ack::blocks_payload & response, const nano::bootstrap_ascending::async_tag & tag)
{
	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::reply);

	accounts.stat (tag.account, account_sets::stat_type::reply);

	if (nothing_new (tag, response.blocks))
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::old);

		accounts.suppress (tag.account);
		accounts.stat (tag.account, account_sets::stat_type::nothing_new);
		return;
	}

	accounts.stat (tag.account, account_sets::stat_type::blocks, response.blocks.size ());

	if (verify (response, tag))
	//	if (true)
	{
		stats.add (nano::stat::type::bootstrap_ascending, nano::stat::detail::blocks, nano::stat::dir::in, response.blocks.size ());

		// TODO: If first block already exists, priority_down

		for (auto & block : response.blocks)
		{
			block_processor.add (block, [this, account = tag.account] (nano::process_result result) {
				switch (result)
				{
					case nano::process_result::progress:
					{
						accounts.stat (account, account_sets::stat_type::progress);
						break;
					}
					case nano::process_result::gap_source:
					{
						accounts.stat (account, account_sets::stat_type::gap_source);
						break;
					}
					case nano::process_result::gap_previous:
					{
						accounts.stat (account, account_sets::stat_type::gap_previous);
						break;
					}
					case nano::process_result::old:
					{
						accounts.stat (account, account_sets::stat_type::old);
						break;
					}
					default:
					{
						accounts.stat (account, account_sets::stat_type::wtf);
						break;
					}
				}
			});
		}
	}
	else
	{
		accounts.stat (tag.account, account_sets::stat_type::corrupt);

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

	if (tag.pulling_by_account ())
	{
		// Open & state blocks always contain account field
		if (first->account () != tag.start)
		{
			// TODO: Stat & log
			return false;
		}
	}
	else
	{
		if (first->hash () != tag.start)
		{
			// TODO: Stat & log
			return false;
		}
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

	//	nano::lock_guard<nano::mutex> lock{ mutex };
	tags.get<tag_id> ().insert (tag);
}

void nano::bootstrap_ascending::debug_log (const std::string & s) const
{
	std::cerr << s << std::endl;
}

auto nano::bootstrap_ascending::info () const -> account_sets::info_t
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.info ();
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::collect_container_info (std::string const & name)
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (accounts.collect_container_info ("accounts"));
	return composite;
}
