#include <nano/lib/stats.hpp>
#include <nano/lib/threading.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/inproc.hpp>
#include <nano/node/vote_processor.hpp>
#include <nano/node/voting.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/store.hpp>

#include <chrono>

/*
 * vote_spacing
 */

void nano::vote_spacing::trim ()
{
	recent.get<tag_time> ().erase (recent.get<tag_time> ().begin (), recent.get<tag_time> ().upper_bound (std::chrono::steady_clock::now () - delay));
}

bool nano::vote_spacing::votable (nano::root const & root_a, nano::block_hash const & hash_a) const
{
	bool result = true;
	for (auto range = recent.get<tag_root> ().equal_range (root_a); result && range.first != range.second; ++range.first)
	{
		auto & item = *range.first;
		result = hash_a == item.hash || item.time < std::chrono::steady_clock::now () - delay;
	}
	return result;
}

void nano::vote_spacing::flag (nano::root const & root_a, nano::block_hash const & hash_a)
{
	trim ();
	auto now = std::chrono::steady_clock::now ();
	auto existing = recent.get<tag_root> ().find (root_a);
	if (existing != recent.end ())
	{
		recent.get<tag_root> ().modify (existing, [now] (entry & entry) {
			entry.time = now;
		});
	}
	else
	{
		recent.insert ({ root_a, now, hash_a });
	}
}

std::size_t nano::vote_spacing::size () const
{
	return recent.size ();
}

/*
 * local_vote_history
 */

bool nano::local_vote_history::consistency_check (nano::root const & root_a) const
{
	auto & history_by_root (history.get<tag_root> ());
	auto const range (history_by_root.equal_range (root_a));
	// All cached votes for a root must be for the same hash, this is actively enforced in local_vote_history::add
	auto consistent_same = std::all_of (range.first, range.second, [hash = range.first->hash] (auto const & info_a) { return info_a.hash == hash; });
	std::vector<nano::account> accounts;

	std::transform (range.first, range.second, std::back_inserter (accounts), [] (auto const & info_a) {
		return info_a.vote->account;
	});

	std::sort (accounts.begin (), accounts.end ());
	// All cached votes must be unique by account, this is actively enforced in local_vote_history::add
	auto consistent_unique = accounts.size () == std::unique (accounts.begin (), accounts.end ()) - accounts.begin ();
	auto result = consistent_same && consistent_unique;
	debug_assert (result);
	return result;
}

void nano::local_vote_history::add (nano::root const & root_a, nano::block_hash const & hash_a, std::shared_ptr<nano::vote> const & vote_a)
{
	debug_assert (vote_a != nullptr);

	nano::lock_guard<nano::mutex> guard (mutex);
	clean ();
	auto add_vote (true);
	auto & history_by_root (history.get<tag_root> ());
	// Erase any vote that is not for this hash, or duplicate by account, and if new timestamp is higher
	auto range (history_by_root.equal_range (root_a));
	for (auto i (range.first); i != range.second;)
	{
		if (i->hash != hash_a || (vote_a->account == i->vote->account && i->vote->timestamp () <= vote_a->timestamp ()))
		{
			i = history_by_root.erase (i);
		}
		else if (vote_a->account == i->vote->account && i->vote->timestamp () > vote_a->timestamp ())
		{
			add_vote = false;
			++i;
		}
		else
		{
			++i;
		}
	}
	// Do not add new vote to cache if representative account is same and timestamp is lower
	if (add_vote)
	{
		auto result (history_by_root.emplace (root_a, hash_a, vote_a));
		(void)result;
		debug_assert (result.second);
	}
	debug_assert (consistency_check (root_a));
}

void nano::local_vote_history::erase (nano::root const & root_a)
{
	nano::lock_guard<nano::mutex> guard (mutex);
	auto & history_by_root (history.get<tag_root> ());
	auto range (history_by_root.equal_range (root_a));
	history_by_root.erase (range.first, range.second);
}

std::vector<std::shared_ptr<nano::vote>> nano::local_vote_history::votes (nano::root const & root_a) const
{
	nano::lock_guard<nano::mutex> guard (mutex);
	std::vector<std::shared_ptr<nano::vote>> result;
	auto range (history.get<tag_root> ().equal_range (root_a));
	std::transform (range.first, range.second, std::back_inserter (result), [] (auto const & entry) { return entry.vote; });
	return result;
}

std::vector<std::shared_ptr<nano::vote>> nano::local_vote_history::votes (nano::root const & root_a, nano::block_hash const & hash_a, bool const is_final_a) const
{
	nano::lock_guard<nano::mutex> guard (mutex);
	std::vector<std::shared_ptr<nano::vote>> result;
	auto range (history.get<tag_root> ().equal_range (root_a));
	// clang-format off
	nano::transform_if (range.first, range.second, std::back_inserter (result),
		[&hash_a, is_final_a](auto const & entry) { return entry.hash == hash_a && (!is_final_a || entry.vote->timestamp () == std::numeric_limits<uint64_t>::max ()); },
		[](auto const & entry) { return entry.vote; });
	// clang-format on
	return result;
}

bool nano::local_vote_history::exists (nano::root const & root_a) const
{
	nano::lock_guard<nano::mutex> guard (mutex);
	return history.get<tag_root> ().find (root_a) != history.get<tag_root> ().end ();
}

void nano::local_vote_history::clean ()
{
	debug_assert (constants.max_cache > 0);
	auto & history_by_sequence (history.get<tag_sequence> ());
	while (history_by_sequence.size () > constants.max_cache)
	{
		history_by_sequence.erase (history_by_sequence.begin ());
	}
}

std::size_t nano::local_vote_history::size () const
{
	nano::lock_guard<nano::mutex> guard (mutex);
	return history.size ();
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (nano::local_vote_history & history, std::string const & name)
{
	std::size_t history_count = history.size ();
	auto sizeof_element = sizeof (decltype (history.history)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	/* This does not currently loop over each element inside the cache to get the sizes of the votes inside history*/
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "history", history_count, sizeof_element }));
	return composite;
}

/*
 * vote_generator
 */

nano::vote_generator::vote_generator (nano::node_config const & config_a, nano::ledger & ledger_a, nano::store & store_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a, nano::stat & stats_a, bool is_final_a) :
	config{ config_a },
	ledger{ ledger_a },
	store{ store_a },
	wallets{ wallets_a },
	vote_processor{ vote_processor_a },
	history{ history_a },
	spacing{ config_a.network_params.voting.delay },
	network{ network_a },
	stats{ stats_a },
	is_final{ is_final_a },
	broadcast_requests{ stats, nano::stat::type::vote_generator, nano::thread_role::name::vote_generator_queue, /* single threaded */ 1, /* max queue size */ 1024 * 32, /* max batch size */ 1024 },
	reply_requests{ stats, nano::stat::type::vote_generator, nano::thread_role::name::vote_generator_queue, /* single threaded */ 1, /* max queue size */ 1024 * 32, /* max batch size */ 1024 }
{
	broadcast_requests.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};

	reply_requests.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};
}

nano::vote_generator::~vote_generator ()
{
	stop ();
}

void nano::vote_generator::start ()
{
	debug_assert (!thread.joinable ());
	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::voting);
		run ();
	});

	broadcast_requests.start ();
	reply_requests.start ();
}

void nano::vote_generator::stop ()
{
	broadcast_requests.stop ();
	reply_requests.stop ();

	stopped = true;
	condition.notify_all ();

	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::vote_generator::broadcast (const nano::root & root, const nano::block_hash & hash)
{
	broadcast_requests.add (std::make_pair (root, hash));
}

void nano::vote_generator::reply (const std::vector<std::pair<nano::root, nano::block_hash>> & candidates, const std::shared_ptr<nano::transport::channel> & channel)
{
	// If channel is full our response will be dropped anyway, so filter that early
	if (!channel->max ())
	{
		reply_requests.add (std::make_pair (candidates, channel));
	}
	else
	{
		// TODO: Stats
	}
}

/*
 * broadcast requests
 */

void nano::vote_generator::process_batch (std::deque<broadcast_request_t> & batch)
{
	auto transaction = ledger.store.tx_begin_write ({ tables::final_votes });

	for (auto & [root, hash] : batch)
	{
		process_broadcast_request (transaction, root, hash);
	}
}

void nano::vote_generator::process_broadcast_request (nano::write_transaction const & transaction, nano::root const & root, nano::block_hash const & hash)
{
	auto cached_votes = history.votes (root, hash, is_final);
	if (!cached_votes.empty ())
	{
		for (auto const & vote : cached_votes)
		{
			send_broadcast (vote);
		}
	}
	else
	{
		if (should_broadcast_vote (transaction, root, hash))
		{
			nano::unique_lock<nano::mutex> lock{ mutex };
			if (candidates_m.size () < max_candidates)
			{
				candidates_m.emplace_back (root, hash);
				if (candidates_m.size () >= nano::network::confirm_ack_hashes_max)
				{
					lock.unlock ();
					condition.notify_all ();
				}
			}
		}
	}
}

bool nano::vote_generator::should_broadcast_vote (const nano::write_transaction & transaction, const nano::root & root, const nano::block_hash & hash)
{
	auto block = store.block.get (transaction, hash);
	if (block == nullptr)
	{
		return false;
	}
	debug_assert (block);
	debug_assert (block->root () == root);

	if (is_final)
	{
		// Allow generation of final votes for active elections only when dependent blocks are already confirmed and final votes database contains current hash
		if (ledger.dependents_confirmed (transaction, *block))
		{
			// Do not allow vote if hash stored in final vote table does not match
			auto final_hashes = store.final_vote.get (transaction, root);
			debug_assert (final_hashes.size () <= 1);
			for (auto & final_hash : final_hashes)
			{
				if (final_hash != hash)
				{
					return false;
				}
			}

			if (store.final_vote.put (transaction, block->qualified_root (), hash))
			{
				return true;
			}
		}
	}
	else
	{
		// Allow generation of non-final votes for active elections only when dependent blocks are already confirmed
		if (ledger.dependents_confirmed (transaction, *block))
		{
			return true;
		}
	}
	return false;
}

/*
 * reply requests
 */

void nano::vote_generator::process_batch (std::deque<reply_request_t> & batch)
{
	auto transaction = store.tx_begin_read ();

	for (auto & [candidates, channel] : batch)
	{
		if (!channel->max ())
		{
			auto votes = process_reply_request (transaction, candidates);
			if (!votes.empty ())
			{
				stats.inc (nano::stat::type::vote_generator, nano::stat::detail::reply);

				for (auto & vote : votes)
				{
					send (vote, channel);
				}
			}
			else
			{
				stats.inc (nano::stat::type::vote_generator, nano::stat::detail::empty_reply);
			}
		}
		else
		{
			// TODO: Stats
		}
	}
}

std::vector<std::shared_ptr<nano::vote>> nano::vote_generator::process_reply_request (const nano::transaction & transaction, const std::vector<candidate_t> & candidates)
{
	std::deque<candidate_t> valid_candidates;
	for (auto & candidate : candidates)
	{
		// Limit number of votes in a single reply
		if (valid_candidates.size () >= nano::network::confirm_ack_hashes_max)
		{
			break;
		}

		auto & [root, hash] = candidate;
		if (should_reply_vote (transaction, root, hash))
		{
			valid_candidates.emplace_back (candidate);
		}
		else
		{
			// TODO: Stats
		}
	}

	auto votes = vote (valid_candidates, /* do not check spacing for already cemented blocks */ false);
	// TODO: Stats
	return votes;
}

bool nano::vote_generator::should_reply_vote (const nano::transaction & transaction, const nano::root & root, const nano::block_hash & hash) const
{
	if (ledger.block_confirmed (transaction, hash))
	{
		return true;
	}
	return false;
}

std::vector<std::shared_ptr<nano::vote>> nano::vote_generator::vote (std::deque<candidate_t> & candidates, bool check_spacing)
{
	std::unordered_set<std::shared_ptr<nano::vote>> cached_votes;

	// Find up to `nano::network::confirm_ack_hashes_max` good candidates to later generate votes for
	std::vector<candidate_t> valid_candidates;
	while (!candidates.empty () && valid_candidates.size () < nano::network::confirm_ack_hashes_max)
	{
		auto candidate = candidates.front ();
		candidates.pop_front ();
		auto const & [root, hash] = candidate;

		// Check if there are any votes already cached
		auto cached = history.votes (root, hash, is_final);
		if (!cached.empty ())
		{
			// Use cached votes
			for (auto const & cached_vote : cached)
			{
				cached_votes.insert (cached_vote);
			}
		}
		else
		{
			if (check_spacing)
			{
				// Check vote spacing for live votes
				if (spacing.votable (root, hash))
				{
					spacing.flag (root, hash);
					valid_candidates.push_back (candidate);
				}
				else
				{
					stats.inc (nano::stat::type::vote_generator, nano::stat::detail::generator_spacing);
				}
			}
			else
			{
				// Vote replies for already cemented blocks do not use vote spacing
				valid_candidates.push_back (candidate);
			}
		}
	}

	stats.add (nano::stat::type::vote_generator, nano::stat::detail::requests_generated_hashes, stat::dir::out, valid_candidates.size ());

	auto generated_votes = generate_votes (valid_candidates);

	// Merge cached and generated votes
	std::vector<std::shared_ptr<nano::vote>> votes;
	votes.reserve (cached_votes.size () + generated_votes.size ());
	votes.insert (votes.end (), cached_votes.begin (), cached_votes.end ());
	votes.insert (votes.end (), generated_votes.begin (), generated_votes.end ());
	return votes;
}

std::vector<std::shared_ptr<nano::vote>> nano::vote_generator::generate_votes (const std::vector<candidate_t> & candidates)
{
	debug_assert (candidates.size () <= nano::network::confirm_ack_hashes_max);
	if (candidates.empty ())
	{
		return {};
	}

	std::vector<nano::block_hash> hashes;
	for (auto & [root, hash] : candidates)
	{
		hashes.push_back (hash);
	}

	// Each local representative generates a single vote
	std::vector<std::shared_ptr<nano::vote>> result;

	wallets.foreach_representative ([this, &hashes, &result] (nano::public_key const & pub_a, nano::raw_key const & prv_a) {
		auto timestamp = is_final ? nano::vote::timestamp_max : nano::milliseconds_since_epoch ();
		uint8_t duration = is_final ? nano::vote::duration_max : /*8192ms*/ 0x9;

		auto vote = std::make_shared<nano::vote> (pub_a, prv_a, timestamp, duration, hashes);
		result.emplace_back (vote);
	});

	// Remember generated votes in local vote history
	for (auto & vote : result)
	{
		for (auto & [root, hash] : candidates)
		{
			history.add (root, hash, vote);
		}
	}

	return result;
}

void nano::vote_generator::send (const std::shared_ptr<nano::vote> & vote, std::shared_ptr<nano::transport::channel> & channel)
{
	stats.inc (nano::stat::type::vote_generator, nano::stat::detail::send, nano::stat::dir::out);

	nano::confirm_ack confirm{ config.network_params.network, vote };
	channel->send (confirm);
}

void nano::vote_generator::send_broadcast (std::shared_ptr<nano::vote> const & vote_a) const
{
	stats.inc (nano::stat::type::vote_generator, nano::stat::detail::send_broadcast, nano::stat::dir::out);

	network.flood_vote_pr (vote_a);
	network.flood_vote (vote_a, 2.0f);
	vote_processor.vote (vote_a, std::make_shared<nano::transport::inproc::channel> (network.node, network.node));
}

/*
 * main_loop
 */

void nano::vote_generator::run_broadcast (nano::unique_lock<nano::mutex> & lock)
{
	debug_assert (lock.owns_lock ());

	// Candidates are verified inside `process_broadcast_request`
	// This pops up to `nano::network::confirm_ack_hashes_max` candidates
	auto votes = vote (candidates_m, /* check spacing (requires lock) */ true);

	lock.unlock ();
	for (auto & vote : votes)
	{
		send_broadcast (vote);
	}
	lock.lock ();
}

void nano::vote_generator::run ()
{
	std::chrono::steady_clock::time_point last_broadcast{};

	nano::unique_lock<nano::mutex> lock (mutex);
	while (!stopped)
	{
		// Wait until candidates reaches max number of entries in a single vote or deadline expires
		if (candidates_m.size () >= nano::network::confirm_ack_hashes_max || (last_broadcast + config.vote_generator_delay < std::chrono::steady_clock::now ()))
		{
			run_broadcast (lock);
			last_broadcast = std::chrono::steady_clock::now ();
		}
		else
		{
			condition.wait_for (lock, config.vote_generator_delay / 2, [this] () {
				return candidates_m.size () >= nano::network::confirm_ack_hashes_max;
			});
		}
	}
}

std::unique_ptr<nano::container_info_component> nano::vote_generator::collect_container_info (std::string const & name)
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	auto composite = std::make_unique<container_info_composite> (name);

	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "candidates", candidates_m.size (), sizeof (decltype (candidates_m)::value_type) }));
	composite->add_component (reply_requests.collect_container_info ("reply_requests"));
	composite->add_component (broadcast_requests.collect_container_info ("broadcast_requests"));

	return composite;
}
