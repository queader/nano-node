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
 * wallet_voter
 */

nano::wallet_voter::wallet_voter (nano::wallets & wallets_a, nano::vote_spacing & spacing_a, nano::local_vote_history & history_a, nano::stat & stats_a, bool is_final_a, nano::stat::type stat_type_a) :
	wallets{ wallets_a },
	spacing{ spacing_a },
	history{ history_a },
	stats{ stats_a },
	is_final{ is_final_a },
	stat_type{ stat_type_a }
{
}

std::vector<std::shared_ptr<nano::vote>> nano::wallet_voter::vote (std::deque<candidate_t> & candidates)
{
	std::unordered_set<std::shared_ptr<nano::vote>> cached_votes;

	// Find up to `nano::network::confirm_ack_hashes_max` good candidates to later generate votes for
	std::vector<candidate_t> cached_candidates;
	std::vector<candidate_t> valid_candidates;
	while (!candidates.empty () && valid_candidates.size () < nano::network::confirm_ack_hashes_max && cached_candidates.size () < max_cached_candidates)
	{
		auto candidate = candidates.front ();
		candidates.pop_front ();
		auto const & [root, hash] = candidate;

		// Check for duplicates
		// Using simple linear scan because count of elements is very low
		if (std::find (valid_candidates.begin (), valid_candidates.end (), candidate) != valid_candidates.end ())
		{
			continue;
		}
		if (std::find (cached_candidates.begin (), cached_candidates.end (), candidate) != cached_candidates.end ())
		{
			continue;
		}

		// Check if there are any votes already cached
		auto cached = history.votes (root, hash, is_final);
		if (!cached.empty ())
		{
			stats.inc (stat_type, nano::stat::detail::cached_hashes);

			// Use cached votes
			cached_candidates.push_back (candidate);
			for (auto const & cached_vote : cached)
			{
				cached_votes.insert (cached_vote);
			}
		}
		else
		{
			// Check vote spacing for live votes
			if (spacing.votable (root, hash))
			{
				spacing.flag (root, hash);
				valid_candidates.push_back (candidate);
			}
			else
			{
				stats.inc (stat_type, nano::stat::detail::generator_spacing);
			}
		}
	}

	stats.add (stat_type, nano::stat::detail::generated_hashes, {}, valid_candidates.size ());

	auto generated_votes = generate_votes (valid_candidates);

	stats.add (stat_type, nano::stat::detail::generated_votes, {}, generated_votes.size ());

	// Merge cached and generated votes
	std::vector<std::shared_ptr<nano::vote>> votes;
	votes.reserve (cached_votes.size () + generated_votes.size ());
	votes.insert (votes.end (), cached_votes.begin (), cached_votes.end ());
	votes.insert (votes.end (), generated_votes.begin (), generated_votes.end ());
	return votes;
}

std::vector<std::shared_ptr<nano::vote>> nano::wallet_voter::generate_votes (const std::vector<candidate_t> & candidates)
{
	debug_assert (candidates.size () <= nano::network::confirm_ack_hashes_max);
	if (candidates.empty ())
	{
		return {};
	}

	std::vector<nano::block_hash> hashes;
	hashes.reserve (candidates.size ());
	for (auto & [root, hash] : candidates)
	{
		hashes.push_back (hash);
	}

	// Each local representative generates a single vote
	std::vector<std::shared_ptr<nano::vote>> result;

	auto timestamp = is_final ? nano::vote::timestamp_max : nano::milliseconds_since_epoch ();
	auto duration = is_final ? nano::vote::duration_max : /*8192ms*/ 0x9;

	wallets.foreach_representative ([&hashes, &result, timestamp, duration] (nano::public_key const & pub_a, nano::raw_key const & prv_a) {
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

/*
 * broadcast_voter
 */

nano::broadcast_voter::broadcast_voter (nano::wallet_voter & voter_a, nano::network & network_a, nano::vote_processor & vote_processor_a, nano::stat & stats_a, std::chrono::milliseconds max_delay_a, nano::stat::type stat_type_a, nano::buffer_drop_policy drop_policy_a) :
	voter{ voter_a },
	network{ network_a },
	vote_processor{ vote_processor_a },
	stats{ stats_a },
	max_delay{ max_delay_a },
	stat_type{ stat_type_a },
	drop_policy{ drop_policy_a }
{
}

nano::broadcast_voter::~broadcast_voter ()
{
	// Thread should be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::broadcast_voter::start ()
{
	debug_assert (!thread.joinable ());
	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::voting);
		run ();
	});
}

void nano::broadcast_voter::stop ()
{
	stopped = true;
	condition.notify_all ();

	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::broadcast_voter::submit (nano::root root, nano::block_hash hash)
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	if (candidates_m.size () < max_candidates)
	{
		candidates_m.emplace_back (root, hash);

		// Wake up thread if enough candidates to fully fill one vote
		if (candidates_m.size () >= nano::network::confirm_ack_hashes_max)
		{
			lock.unlock ();
			condition.notify_all ();
		}
	}
	else
	{
		stats.inc (stat_type, nano::stat::detail::overfill);
	}
}

void nano::broadcast_voter::run ()
{
	std::chrono::steady_clock::time_point last_broadcast{};

	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		// Wait until candidates reaches max number of entries in a single vote or deadline expires
		if (candidates_m.size () >= nano::network::confirm_ack_hashes_max || (last_broadcast + max_delay < std::chrono::steady_clock::now ()))
		{
			if (!candidates_m.empty ())
			{
				run_broadcast (lock);
			}
			last_broadcast = std::chrono::steady_clock::now ();
		}
		else
		{
			condition.wait_for (lock, max_delay / 2, [this] () {
				return candidates_m.size () >= nano::network::confirm_ack_hashes_max;
			});
		}
	}
}

void nano::broadcast_voter::run_broadcast (nano::unique_lock<nano::mutex> & lock)
{
	debug_assert (lock.owns_lock ());

	// Candidates are verified inside `process_broadcast_request`
	// This pops up to `nano::network::confirm_ack_hashes_max` candidates
	auto votes = voter.vote (candidates_m);

	lock.unlock ();
	for (auto & vote : votes)
	{
		send_broadcast (vote);
	}
	lock.lock ();
}

void nano::broadcast_voter::send_broadcast (std::shared_ptr<nano::vote> const & vote_a) const
{
	stats.inc (stat_type, nano::stat::detail::send_broadcast, nano::stat::dir::out);

	network.flood_vote_pr (vote_a);
	network.flood_vote (vote_a, 2.0f, drop_policy);
	vote_processor.vote (vote_a, std::make_shared<nano::transport::inproc::channel> (network.node, network.node));
}

std::unique_ptr<nano::container_info_component> nano::broadcast_voter::collect_container_info (std::string const & name)
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "candidates", candidates_m.size (), sizeof (decltype (candidates_m)::value_type) }));
	return composite;
}

/*
 * normal_vote_generator
 */

nano::normal_vote_generator::normal_vote_generator (nano::node_config const & config_a, nano::ledger & ledger_a, nano::store & store_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a, nano::stat & stats_a) :
	config{ config_a },
	ledger{ ledger_a },
	store{ store_a },
	wallets{ wallets_a },
	vote_processor{ vote_processor_a },
	history{ history_a },
	network{ network_a },
	stats{ stats_a },
	spacing{ config.network_params.voting.delay },
	voter{ wallets, spacing, history, stats, is_final, nano::stat::type::vote_generator_normal },
	broadcaster{ voter, network, vote_processor, stats, config.vote_generator_delay, nano::stat::type::vote_generator_normal, /* throttle voting */ nano::buffer_drop_policy::limiter },
	broadcast_requests{ stats, nano::stat::type::vote_generator_normal, nano::thread_role::name::vote_generator_queue, /* single threaded */ 1, /* max queue size */ 1024 * 32, /* max batch size */ 1024 }
{
	broadcast_requests.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};
}

nano::normal_vote_generator::~normal_vote_generator ()
{
	stop ();
}

void nano::normal_vote_generator::start ()
{
	broadcaster.start ();
	broadcast_requests.start ();
}

void nano::normal_vote_generator::stop ()
{
	broadcast_requests.stop ();
	broadcaster.stop ();
}

void nano::normal_vote_generator::broadcast (const nano::root & root, const nano::block_hash & hash)
{
	broadcast_requests.add (std::make_pair (root, hash));
}

void nano::normal_vote_generator::process_batch (std::deque<broadcast_request_t> & batch)
{
	auto transaction = ledger.store.tx_begin_read ();

	for (auto & [root, hash] : batch)
	{
		process (transaction, root, hash);
	}
}

void nano::normal_vote_generator::process (nano::transaction const & transaction, nano::root const & root, nano::block_hash const & hash)
{
	if (should_vote (transaction, root, hash))
	{
		broadcaster.submit (root, hash);
	}
	else
	{
		stats.inc (nano::stat::type::vote_generator_normal, nano::stat::detail::non_votable);
	}
}

bool nano::normal_vote_generator::should_vote (const nano::transaction & transaction, const nano::root & root, const nano::block_hash & hash)
{
	auto block = store.block.get (transaction, hash);
	if (block == nullptr)
	{
		return false;
	}
	debug_assert (block);
	debug_assert (block->root () == root);

	// Allow generation of non-final votes for active elections only when dependent blocks are already confirmed
	if (ledger.dependents_confirmed (transaction, *block))
	{
		return true;
	}
	return false;
}

std::unique_ptr<nano::container_info_component> nano::normal_vote_generator::collect_container_info (std::string const & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (broadcaster.collect_container_info ("broadcaster"));
	composite->add_component (broadcast_requests.collect_container_info ("broadcast_requests"));
	return composite;
}

/*
 * final_vote_generator
 */

nano::final_vote_generator::final_vote_generator (nano::node_config const & config_a, nano::ledger & ledger_a, nano::store & store_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a, nano::stat & stats_a) :
	config{ config_a },
	ledger{ ledger_a },
	store{ store_a },
	wallets{ wallets_a },
	vote_processor{ vote_processor_a },
	history{ history_a },
	network{ network_a },
	stats{ stats_a },
	spacing{ config.network_params.voting.delay },
	voter{ wallets, spacing, history, stats, is_final, nano::stat::type::vote_generator_final },
	broadcaster{ voter, network, vote_processor, stats, config.vote_generator_delay, nano::stat::type::vote_generator_final, /* do not throttle final votes */ nano::buffer_drop_policy::no_limiter_drop },
	broadcast_requests{ stats, nano::stat::type::vote_generator_final, nano::thread_role::name::vote_generator_queue, /* single threaded */ 1, /* max queue size */ 1024 * 32, /* max batch size */ 1024 }
{
	broadcast_requests.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};
}

nano::final_vote_generator::~final_vote_generator ()
{
	stop ();
}

void nano::final_vote_generator::start ()
{
	broadcaster.start ();
	broadcast_requests.start ();
}

void nano::final_vote_generator::stop ()
{
	broadcast_requests.stop ();
	broadcaster.stop ();
}

void nano::final_vote_generator::broadcast (const nano::root & root, const nano::block_hash & hash)
{
	broadcast_requests.add (std::make_pair (root, hash));
}

void nano::final_vote_generator::process_batch (std::deque<broadcast_request_t> & batch)
{
	auto transaction = ledger.store.tx_begin_write ({ tables::final_votes });

	for (auto & [root, hash] : batch)
	{
		process (transaction, root, hash);
	}
}

void nano::final_vote_generator::process (nano::write_transaction const & transaction, nano::root const & root, nano::block_hash const & hash)
{
	if (should_vote (transaction, root, hash))
	{
		broadcaster.submit (root, hash);
	}
	else
	{
		stats.inc (nano::stat::type::vote_generator_final, nano::stat::detail::non_votable);
	}
}

bool nano::final_vote_generator::should_vote (const nano::write_transaction & transaction, const nano::root & root, const nano::block_hash & hash)
{
	auto block = store.block.get (transaction, hash);
	if (block == nullptr)
	{
		return false;
	}
	debug_assert (block);
	debug_assert (block->root () == root);

	// Allow generation of final votes for active elections only when dependent blocks are already confirmed and final votes database contains current hash
	if (ledger.dependents_confirmed (transaction, *block))
	{
		// `final_vote.check_and_put` will return false if there is an existing hash for block root, and it does not match our `hash`
		if (store.final_vote.check_and_put (transaction, block->qualified_root (), hash))
		{
			return true;
		}
	}
	return false;
}

std::unique_ptr<nano::container_info_component> nano::final_vote_generator::collect_container_info (std::string const & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (broadcaster.collect_container_info ("broadcaster"));
	composite->add_component (broadcast_requests.collect_container_info ("broadcast_requests"));
	return composite;
}

/*
 * reply_vote_generator
 */

nano::reply_vote_generator::reply_vote_generator (nano::node_config const & config_a, nano::ledger & ledger_a, nano::store & store_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a, nano::stat & stats_a) :
	config{ config_a },
	ledger{ ledger_a },
	store{ store_a },
	wallets{ wallets_a },
	vote_processor{ vote_processor_a },
	history{ history_a },
	network{ network_a },
	stats{ stats_a },
	spacing{ std::chrono::milliseconds (0) }, // Disable spacing
	voter{ wallets, spacing, history, stats, is_final, nano::stat::type::vote_generator_reply },
	reply_requests{ stats, nano::stat::type::vote_generator_reply, nano::thread_role::name::vote_generator_queue, /* single threaded */ 1, /* max queue size */ 1024 * 32, /* max batch size */ 1024 }
{
	reply_requests.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};
}

nano::reply_vote_generator::~reply_vote_generator ()
{
	reply_requests.stop ();
}

void nano::reply_vote_generator::start ()
{
	reply_requests.start ();
}

void nano::reply_vote_generator::stop ()
{
	reply_requests.stop ();
}

void nano::reply_vote_generator::request (const std::vector<std::pair<nano::root, nano::block_hash>> & candidates, const std::shared_ptr<nano::transport::channel> & channel)
{
	// If channel is full our response will be dropped anyway, so filter that early
	if (!channel->max ())
	{
		reply_requests.add (std::make_pair (candidates, channel));
	}
	else
	{
		stats.inc (nano::stat::type::vote_generator_reply, nano::stat::detail::channel_full, nano::stat::dir::in);
	}
}

void nano::reply_vote_generator::process_batch (std::deque<reply_request_t> & batch)
{
	auto transaction = store.tx_begin_read ();

	for (auto & [candidates, channel] : batch)
	{
		if (!channel->max ())
		{
			auto votes = process (transaction, candidates);
			if (!votes.empty ())
			{
				stats.inc (nano::stat::type::vote_generator_reply, nano::stat::detail::reply);

				for (auto & vote : votes)
				{
					send (vote, channel);
				}
			}
			else
			{
				stats.inc (nano::stat::type::vote_generator_reply, nano::stat::detail::empty_reply);
			}
		}
		else
		{
			stats.inc (nano::stat::type::vote_generator_reply, nano::stat::detail::channel_full, nano::stat::dir::out);
		}
	}
}

std::vector<std::shared_ptr<nano::vote>> nano::reply_vote_generator::process (const nano::transaction & transaction, const std::vector<candidate_t> & candidates)
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
		if (should_vote (transaction, root, hash))
		{
			valid_candidates.emplace_back (candidate);
		}
	}

	stats.add (nano::stat::type::vote_generator_reply, nano::stat::detail::candidates, {}, candidates.size ());
	stats.add (nano::stat::type::vote_generator_reply, nano::stat::detail::valid_candidates, {}, valid_candidates.size ());

	auto votes = voter.vote (valid_candidates);
	debug_assert (valid_candidates.empty ()); // Ensure all candidates were consumed
	return votes;
}

bool nano::reply_vote_generator::should_vote (const nano::transaction & transaction, const nano::root & root, const nano::block_hash & hash) const
{
	if (ledger.block_confirmed (transaction, hash))
	{
		return true;
	}
	return false;
}

void nano::reply_vote_generator::send (const std::shared_ptr<nano::vote> & vote, std::shared_ptr<nano::transport::channel> & channel)
{
	stats.inc (nano::stat::type::vote_generator_reply, nano::stat::detail::send, nano::stat::dir::out);

	nano::confirm_ack confirm{ config.network_params.network, vote };
	channel->send (confirm, nullptr, nano::buffer_drop_policy::limiter, nano::bandwidth_limit_type::voting_replies);
}

std::unique_ptr<nano::container_info_component> nano::reply_vote_generator::collect_container_info (std::string const & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (reply_requests.collect_container_info ("reply_requests"));
	return composite;
}