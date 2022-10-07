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

nano::bootstrap::bootstrap_ascending::account_sets::account_sets (nano::stat & stats_a) :
	stats{ stats_a }
{
}

void nano::bootstrap::bootstrap_ascending::account_sets::dump () const
{
	std::cerr << boost::str (boost::format ("Forwarding: %1%   blocking: %2%\n") % forwarding.size () % blocking.size ());
	std::deque<size_t> weight_counts;
	for (auto & [account, count] : backoff)
	{
		auto log = std::log2 (std::max<decltype (count)> (count, 1));
		// std::cerr << "log: " << log << ' ';
		auto index = static_cast<size_t> (log);
		if (weight_counts.size () <= index)
		{
			weight_counts.resize (index + 1);
		}
		++weight_counts[index];
	}
	std::string output;
	output += "Backoff hist (size: " + std::to_string (backoff.size ()) + "): ";
	for (size_t i = 0, n = weight_counts.size (); i < n; ++i)
	{
		output += std::to_string (weight_counts[i]) + ' ';
	}
	output += '\n';
	std::cerr << output;
}

void nano::bootstrap::bootstrap_ascending::account_sets::prioritize (nano::account const & account, float priority)
{
	if (blocking.count (account) == 0)
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::prioritize);

		forwarding.insert (account);
		auto iter = backoff.find (account);
		if (iter == backoff.end ())
		{
			backoff.emplace (account, priority);
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::prioritize_failed);
	}
}

void nano::bootstrap::bootstrap_ascending::account_sets::block (nano::account const & account, nano::block_hash const & dependency)
{
	stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::block);

	backoff.erase (account);
	forwarding.erase (account);
	blocking[account] = dependency;
}

void nano::bootstrap::bootstrap_ascending::account_sets::unblock (nano::account const & account, nano::block_hash const & hash)
{
	// Unblock only if the dependency is fulfilled
	if (blocking.count (account) > 0 && blocking[account] == hash)
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::unblock);

		blocking.erase (account);
		backoff[account] = 0.0f;
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::unblock_failed);
	}
}

void nano::bootstrap::bootstrap_ascending::account_sets::force_unblock (const nano::account & account)
{
	blocking.erase (account);
	backoff[account] = 0.0f;
}

std::vector<double> nano::bootstrap::bootstrap_ascending::account_sets::probability_transform (std::vector<decltype (backoff)::mapped_type> const & attempts) const
{
	std::vector<double> result;
	result.reserve (attempts.size ());
	for (auto i = attempts.begin (), n = attempts.end (); i != n; ++i)
	{
		result.push_back (1.0 / std::pow (2.0, *i));
	}
	return result;
}

nano::account nano::bootstrap::bootstrap_ascending::account_sets::random ()
{
	debug_assert (!backoff.empty ());
	std::vector<decltype (backoff)::mapped_type> attempts;
	std::vector<nano::account> candidates;
	while (candidates.size () < account_sets::backoff_exclusion)
	{
		debug_assert (candidates.size () == attempts.size ());
		nano::account search;
		nano::random_pool::generate_block (search.bytes.data (), search.bytes.size ());
		auto iter = backoff.lower_bound (search);
		if (iter == backoff.end ())
		{
			iter = backoff.begin ();
		}
		auto const [account, count] = *iter;
		attempts.push_back (count);
		candidates.push_back (account);
	}
	auto weights = probability_transform (attempts);
	std::discrete_distribution dist{ weights.begin (), weights.end () };
	auto selection = dist (rng);
	debug_assert (!weights.empty () && selection < weights.size ());
	auto result = candidates[selection];
	return result;
}

nano::account nano::bootstrap::bootstrap_ascending::account_sets::next ()
{
	nano::account result;
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
	backoff[result] += 1.0f;
	return result;
}

bool nano::bootstrap::bootstrap_ascending::account_sets::blocked (nano::account const & account) const
{
	return blocking.count (account) > 0;
}

nano::bootstrap::bootstrap_ascending::account_sets::backoff_info_t nano::bootstrap::bootstrap_ascending::account_sets::backoff_info () const
{
	return { forwarding, blocking, backoff };
}

/*
 * bootstrap_ascending
 */

nano::bootstrap::bootstrap_ascending::thread::thread (std::shared_ptr<bootstrap_ascending> bootstrap) :
	bootstrap_ptr{ bootstrap }
{
}

uint64_t nano::bootstrap::bootstrap_ascending::generate_id () const
{
	id_t id;
	nano::random_pool::generate_block (reinterpret_cast<uint8_t *> (&id), sizeof (id));
	return id;
}

void nano::bootstrap::bootstrap_ascending::thread::send (std::shared_ptr<nano::transport::channel> channel, async_tag tag)
{
	nano::asc_pull_req message{ bootstrap.node->network_params.network };
	message.id = tag.id;
	message.start = tag.start;

	++bootstrap.requests_total;
	bootstrap.stats.inc (nano::stat::type::bootstrap_ascending_thread, nano::stat::detail::request);

	std::cout << "requesting: " << std::setw (28) << tag.id
			  << " | "
			  << "channel: " << channel->to_string ()
			  << std::endl;

	bool sent = channel->send (message, [this_l = shared (), tag] (boost::system::error_code const & ec, std::size_t size) {
		if (ec)
		{
			std::cerr << "send error: " << ec << std::endl;
		}
	});
	release_assert (sent);
}

nano::account nano::bootstrap::bootstrap_ascending::thread::pick_account ()
{
	nano::lock_guard<nano::mutex> lock{ bootstrap.mutex };
	return bootstrap.accounts.next ();
}

/** Inspects a block that has been processed by the block processor
- Marks an account as blocked if the result code is gap source as there is no reason request additional blocks for this account until the dependency is resolved
- Marks an account as forwarded if it has been recently referenced by a block that has been inserted.
 */
void nano::bootstrap::bootstrap_ascending::inspect (nano::transaction const & tx, nano::process_return const & result, nano::block const & block)
{
	auto const hash = block.hash ();

	switch (result.code)
	{
		case nano::process_result::progress:
		{
			const auto account = node->ledger.account (tx, hash);
			const auto is_send = node->ledger.is_send (tx, block);

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
			const auto account = block.previous ().is_zero () ? block.account () : node->ledger.account (tx, block.previous ());
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

void nano::bootstrap::bootstrap_ascending::dump_stats ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	std::cerr << boost::str (boost::format ("Requests total: %1% forwarded: %2% responses: %3%\n") % requests_total.load () % forwarded % responses.load ());
	accounts.dump ();
	responses = 0;
}

bool nano::bootstrap::bootstrap_ascending::thread::wait_available_request ()
{
	nano::unique_lock<nano::mutex> lock{ bootstrap.mutex };
	bootstrap.condition.wait (lock, [this] () { return bootstrap.stopped || bootstrap.tags.size () < requests_max; });

	bootstrap.debug_log (boost::str (boost::format ("wait_available_request stopped=%1% request=%2%") % bootstrap.stopped % bootstrap.tags.size ()));

	return bootstrap.stopped;
}

nano::bootstrap::bootstrap_ascending::bootstrap_ascending (std::shared_ptr<nano::node> const & node_a, uint64_t incremental_id_a, std::string id_a) :
	bootstrap_attempt{ node_a, nano::bootstrap_mode::ascending, incremental_id_a, id_a },
	stats{ node->stats }, // TODO: For convenience, once ascending bootstrap is a separate node component, pass via constructor
	accounts{ node->stats }
{
	uint64_t account_count = 0, receivable_count = 0;

	auto tx = node_a->store.tx_begin_read ();
	for (auto i = node_a->store.account.begin (tx), n = node_a->store.account.end (); i != n; ++i)
	{
		accounts.prioritize (i->first, 0.0f);
		account_count++;
	}
	for (auto i = node_a->store.pending.begin (tx), n = node_a->store.pending.end (); i != n; ++i)
	{
		accounts.prioritize (i->first.key (), 0.0f);
		receivable_count++;
	}

	debug_log (boost::str (boost::format ("bootstrap_ascending constructor: incr_id=%1% id=%2% accounts=%3% receivable=%4%")
	% incremental_id_a % id_a % account_count % receivable_count));
}

std::shared_ptr<nano::transport::channel> nano::bootstrap::bootstrap_ascending::available_channel ()
{
	auto channels = node->network.random_set (16, node->network_params.network.bootstrap_protocol_version_min, /* include temporary channels */ true);
	for (auto & channel : channels)
	{
		if (!channel->max ())
		{
			return channel;
		}
	}
	return nullptr;
}

std::shared_ptr<nano::transport::channel> nano::bootstrap::bootstrap_ascending::wait_available_channel ()
{
	std::shared_ptr<nano::transport::channel> channel;
	while (!(channel = available_channel ()))
	{
		std::this_thread::sleep_for (10ms);
	}
	return channel;
}

bool nano::bootstrap::bootstrap_ascending::thread::request_one ()
{
	auto account = pick_account ();
	nano::account_info info;
	nano::hash_or_account start = account;

	// check if the account picked has blocks, if it does, start the pull from the highest block
	if (!bootstrap.node->store.account.get (bootstrap.node->store.tx_begin_read (), account, info))
	{
		start = info.head;
	}

	const async_tag tag{ bootstrap.generate_id (), start, nano::milliseconds_since_epoch () };

	auto channel = bootstrap.wait_available_channel ();
	if (channel)
	{
		bootstrap.track (tag);
		send (channel, tag);
		return true;
	}
	else
	{
		std::cout << "fail" << std::endl;

		// No suitable channel
		return false;
	}
}

void nano::bootstrap::bootstrap_ascending::thread::run ()
{
	while (!bootstrap.stopped)
	{
		// TODO: Make threadsafe
		// do not do too many requests in parallel, impose throttling
		wait_available_request ();

		bool success = request_one ();
		if (!success)
		{
			std::this_thread::sleep_for (10s);
		}
		//		else
		//		{
		//			std::this_thread::sleep_for (100ms);
		//		}
	}
}

auto nano::bootstrap::bootstrap_ascending::thread::shared () -> std::shared_ptr<thread>
{
	return shared_from_this ();
}

void nano::bootstrap::bootstrap_ascending::run ()
{
	node->block_processor.processed.add ([this_w = std::weak_ptr<nano::bootstrap::bootstrap_ascending>{ shared () }] (nano::transaction const & tx, nano::process_return const & result, nano::block const & block) {
		auto this_l = this_w.lock ();
		if (this_l == nullptr)
		{
			// std::cerr << boost::str (boost::format ("Missed block: %1%\n") % block.hash ().to_string ());
			return;
		}
		this_l->inspect (tx, result, block);
	});

	std::deque<std::thread> threads;
	for (auto i = 0; i < parallelism; ++i)
	{
		threads.emplace_back ([this_l = shared ()] () {
			nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
			auto thread = std::make_shared<bootstrap_ascending::thread> (this_l);
			thread->run ();
		});
	}

	while (true)
	{
		std::this_thread::sleep_for (1s);
		timeout_tags ();
		condition.notify_all ();
	}
}

void nano::bootstrap::bootstrap_ascending::timeout_tags ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	const nano::millis_t threshold = 5 * 1000;

	auto & tags_by_order = tags.get<tag_sequenced> ();
	while (!tags_by_order.empty () && nano::time_difference (tags_by_order.front ().time, nano::milliseconds_since_epoch ()) > threshold)
	{
		std::cout << "timeout: " << tags_by_order.front ().id
				  << " | "
				  << "count: " << tags.size () << std::endl;

		tags_by_order.pop_front ();

		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::timeout);
	}
}

void nano::bootstrap::bootstrap_ascending::process (const nano::asc_pull_ack & message)
{
	//	std::cout << "reply: " << message.id << std::endl;

	nano::unique_lock<nano::mutex> lock{ mutex };

	auto & tags_by_id = tags.get<tag_id> ();
	if (tags_by_id.count (message.id) > 0)
	{
		auto iterator = tags_by_id.find (message.id);
		auto tag = *iterator;
		tags_by_id.erase (iterator);

		lock.unlock ();
		condition.notify_all ();

		process (message, tag);
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::missing_tag);
	}
}

void nano::bootstrap::bootstrap_ascending::process (const nano::asc_pull_ack & message, async_tag const & tag)
{
	debug_assert (message.id == tag.id);

	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::process);

	// TODO: Do verifications

	for (auto & block : message.blocks ())
	{
		node->block_processor.add (block);
	}
}

void nano::bootstrap::bootstrap_ascending::track (async_tag const & tag)
{
	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::track);

	nano::lock_guard<nano::mutex> lock{ mutex };
	tags.get<tag_id> ().insert (tag);

	//	std::cout << "tracking: " << tag.id
	//			  << " | "
	//			  << "count: " << tags.size ()
	//			  << std::endl;
}

void nano::bootstrap::bootstrap_ascending::get_information (boost::property_tree::ptree &)
{
}

std::shared_ptr<nano::bootstrap::bootstrap_ascending> nano::bootstrap::bootstrap_ascending::shared ()
{
	return std::static_pointer_cast<nano::bootstrap::bootstrap_ascending> (shared_from_this ());
}

void nano::bootstrap::bootstrap_ascending::debug_log (const std::string & s) const
{
	//	std::cerr << s << std::endl;
}

nano::bootstrap::bootstrap_ascending::account_sets::backoff_info_t nano::bootstrap::bootstrap_ascending::backoff_info () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.backoff_info ();
}