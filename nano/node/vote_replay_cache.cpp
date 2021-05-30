#include <nano/lib/stats.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/active_transactions.hpp>
#include <nano/node/common.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/vote_replay_cache.hpp>
#include <nano/secure/blockstore.hpp>
#include <nano/secure/ledger.hpp>

nano::vote_replay_cache::vote_replay_cache (nano::node & node_a) :
	node ( node_a ),
	stats ( node_a.stats ),
	ledger ( node_a.ledger ),
	active ( node_a.active ),
	store (node_a.vote_store),
	replay_vote_weight_minimum (node_a.config.replay_vote_weight_minimum.number ()),
	replay_unconfirmed_vote_weight_minimum (node_a.config.replay_unconfirmed_vote_weight_minimum.number ()),
	thread_seed_votes ([this] ()
	{ run_aec_vote_seeding (); }),
	thread_rebroadcast ([this] ()
	{ run_rebroadcast (); }),
	/*thread_rebroadcast_2 ([this] ()
	{ run_rebroadcast (); }),*/
	thread_rebroadcast_random ([this] ()
	{ run_rebroadcast_random (); })
{
}

void nano::vote_replay_cache::stop ()
{
	{
		nano::lock_guard<nano::mutex> guard (mutex);
		nano::lock_guard<nano::mutex> guard_candidates (mutex_candidates);
		stopped = true;
	}
	condition.notify_all ();
	condition_candidates.notify_all ();
	if (thread_seed_votes.joinable ())
	{
		thread_seed_votes.join ();
	}
	if (thread_rebroadcast.joinable ())
	{
		thread_rebroadcast.join ();
	}
	if (thread_rebroadcast_random.joinable ())
	{
		thread_rebroadcast_random.join ();
	}
}

void nano::vote_replay_cache::add (const nano::block_hash hash_a, const std::vector<std::shared_ptr<nano::vote>> vote_l)
{
	const int max_candidates_size = 1024;

	nano::unique_lock<nano::mutex> lock (mutex_candidates);

	if (replay_candidates.size () < max_candidates_size && replay_candidates_hashes.find (hash_a) == replay_candidates_hashes.end())
	{
		replay_candidates.emplace_back (vote_l);
		replay_candidates_hashes.emplace (hash_a);
		lock.unlock ();
		condition_candidates.notify_all ();
	}
	else
	{
		stats.inc (nano::stat::type::vote_replay_rebroadcast, nano::stat::detail::vote_overflow, nano::stat::dir::in);
	}
}

bool nano::vote_replay_cache::add_vote_to_db (nano::write_transaction const & transaction_a, std::shared_ptr<nano::vote> const & vote_a)
{
	return store.vote_replay_put (transaction_a, vote_a);
}

nano::vote_replay_cache::vote_cache_result nano::vote_replay_cache::get_vote_replay_cached_votes_for_hash (nano::transaction const & transaction_a, nano::block_hash hash_a, nano::uint128_t minimum_weight) const
{
	auto votes_l = store.vote_replay_get (transaction_a, hash_a);

	nano::uint128_t weight = 0;
	for (auto const & vote : votes_l)
	{
		auto rep_weight (ledger.weight (vote->account));
		weight += rep_weight;
	}

	nano::vote_replay_cache::vote_cache_result result;

	if (weight >= minimum_weight)
	{
		result = std::make_pair(hash_a, votes_l);
	}

	return result;
}

nano::vote_replay_cache::vote_cache_result nano::vote_replay_cache::get_vote_replay_cached_votes_for_hash_or_conf_frontier (nano::transaction const & transaction_a, nano::transaction const & transaction_vote_cache_a, nano::block_hash hash_a) const
{
	nano::vote_replay_cache::vote_cache_result result;

	if (ledger.block_confirmed (transaction_a, hash_a))
	{
		auto account = ledger.account (transaction_a, hash_a);
		if (!account.is_zero ())
		{
			stats.inc (nano::stat::type::vote_replay, nano::stat::detail::block_confirmed);

			nano::confirmation_height_info conf_info;
			ledger.store.confirmation_height_get (transaction_a, account, conf_info);

			if (conf_info.frontier != 0 && conf_info.frontier != hash_a)
			{
				result = get_vote_replay_cached_votes_for_hash (transaction_vote_cache_a, conf_info.frontier, replay_vote_weight_minimum);
				if (result)
				{
					stats.inc (nano::stat::type::vote_replay, nano::stat::detail::frontier_confirmation_successful);
				}
			}

			if (!result)
			{
				result = get_vote_replay_cached_votes_for_hash (transaction_vote_cache_a, hash_a, replay_vote_weight_minimum);
			}

			if (!result)
			{
				stats.inc (nano::stat::type::vote_replay, nano::stat::detail::vote_invalid);
			}
		}
	}
	else
	{
		stats.inc (nano::stat::type::vote_replay, nano::stat::detail::block_not_confirmed);
	}

	if (result)
	{
		stats.inc (nano::stat::type::vote_replay, nano::stat::detail::vote_replay);
	}

	return result;
}

nano::vote_replay_cache::vote_cache_result nano::vote_replay_cache::get_vote_replay_cached_votes_for_conf_frontier (nano::transaction const & transaction_a, nano::transaction const & transaction_vote_cache_a, nano::block_hash hash_a) const
{
	nano::vote_replay_cache::vote_cache_result result;

	if (ledger.block_or_pruned_exists (transaction_a, hash_a))
	{
		auto account = ledger.account (transaction_a, hash_a);
		if (!account.is_zero ())
		{
			nano::confirmation_height_info conf_info;
			ledger.store.confirmation_height_get (transaction_a, account, conf_info);

			if (conf_info.frontier != 0)
			{
				result = get_vote_replay_cached_votes_for_hash (transaction_vote_cache_a, conf_info.frontier, replay_vote_weight_minimum);
			}
		}
	}

	return result;
}

void nano::vote_replay_cache::run_aec_vote_seeding () const
{
	nano::thread_role::set (nano::thread_role::name::seed_votes);

	node.node_initialized_latch.wait ();

	const nano::uint128_t minimum_weight = replay_unconfirmed_vote_weight_minimum;

	while (!stopped && node.config.enable_vote_replay_aec_seeding)
	{
		nano::block_hash hash;
		nano::random_pool::generate_block (hash.bytes.data (), hash.bytes.size ());

		nano::votes_replay_key prev_key (hash, 0);

		{
			auto transaction (store.tx_begin_read ());

			int k = 0;
			for (auto i = store.vote_replay_begin (transaction, prev_key), n = store.vote_replay_end (); i != n && k < 50000 && !stopped; ++i, ++k)
			{
				if (i->first.block_hash () != prev_key.block_hash ())
				{
					prev_key = i->first;

					auto vote_a = std::make_shared<nano::vote> (i->second);

					for (auto vote_block : vote_a->blocks)
					{
						debug_assert (vote_block.which ());

						auto const & block_hash = boost::get<nano::block_hash> (vote_block);

						if (!ledger.block_confirmed (transaction, block_hash))
						{
							auto cache_result = get_vote_replay_cached_votes_for_hash (transaction, block_hash, minimum_weight);
							if (cache_result)
							{
								const auto & [cached_hash, cached] = (*cache_result);
								for (auto const & vote_b : cached)
								{
									active.vote (vote_b);
								}

								stats.inc (nano::stat::type::vote_replay_seed, nano::stat::detail::election_start);
							}
						}
					}
				}
			}
		}

		std::this_thread::sleep_for (std::chrono::milliseconds (100));
		std::this_thread::yield ();
	}
}

void nano::vote_replay_cache::run_rebroadcast ()
{
	nano::thread_role::set (nano::thread_role::name::rebroadcast_votes);

	node.node_initialized_latch.wait ();

	const nano::uint128_t minimum_weight = replay_unconfirmed_vote_weight_minimum;
	const int max_rebroadcasts_per_loop = 10;

	nano::unique_lock<nano::mutex> lock (mutex_candidates);

	while (!stopped)
	{
		if (!replay_candidates.empty ())
		{
			decltype (replay_candidates) replay_candidates_l;
			replay_candidates_l.swap (replay_candidates);

			const int max_replay_history_size = 1024 * 1024;
			if (replay_candidates_hashes.size () > max_replay_history_size)
			{
				replay_candidates_hashes.clear ();
			}

			lock.unlock ();

			while (!replay_candidates_l.empty () && !stopped)
			{
				int done_this_loop = 0;
				while (!replay_candidates_l.empty () && done_this_loop < max_rebroadcasts_per_loop)
				{
					auto const & vote_l = replay_candidates_l.front ();

					node.network.flood_vote_list_all (vote_l);

					//This didn't work that well 
					//node.network.flood_vote_list_pr (vote_l);
					//node.network.flood_vote_list (vote_l, 2);

					++done_this_loop;
					stats.inc (nano::stat::type::vote_replay_rebroadcast, nano::stat::detail::republish_vote, nano::stat::dir::out);

					replay_candidates_l.pop_front ();
				}

				std::this_thread::yield ();
			}

			lock.lock ();
		}
		else
		{
			condition_candidates.wait (lock);
		}
	}
}

void nano::vote_replay_cache::run_rebroadcast_random () const
{
	nano::thread_role::set (nano::thread_role::name::rebroadcast_votes);

	node.node_initialized_latch.wait ();

	const nano::uint128_t minimum_weight = replay_unconfirmed_vote_weight_minimum;
	const int max_rebroadcasts_per_loop = 10;

	while (!stopped && node.config.enable_random_vote_replay)
	{
		nano::block_hash initial_hash;
		nano::random_pool::generate_block (initial_hash.bytes.data (), initial_hash.bytes.size ());

		nano::votes_replay_key prev_key (initial_hash, 0);

		{
			auto transaction (ledger.store.tx_begin_read ());
			auto transaction_vote_cache (store.tx_begin_read ());

			int rebroadcasts_done = 0;
			int k = 0;
			for (auto i = store.vote_replay_begin (transaction_vote_cache, prev_key), n = store.vote_replay_end (); i != n && k < 50000; ++i, ++k)
			{
				if (i->first.block_hash () != prev_key.block_hash ())
				{
					prev_key = i->first;

					auto vote_a = std::make_shared<nano::vote> (i->second);

					auto cache_result = get_vote_replay_cached_votes_for_conf_frontier (transaction, transaction_vote_cache, i->first.block_hash ());
					if (cache_result)
					{
						const auto & [cached_hash, cached] = (*cache_result);
						node.network.flood_vote_list_all (cached);

						stats.inc (nano::stat::type::vote_replay_rebroadcast, nano::stat::detail::republish_vote, nano::stat::dir::out);

						++rebroadcasts_done;
						if (rebroadcasts_done >= max_rebroadcasts_per_loop)
						{
							break;
						}
					}
				}
			}
		}

		std::this_thread::sleep_for (std::chrono::milliseconds (50));
		std::this_thread::yield ();
	}
}
