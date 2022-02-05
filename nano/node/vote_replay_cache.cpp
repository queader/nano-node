#include <nano/lib/stats.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/active_transactions.hpp>
#include <nano/node/common.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/vote_replay_cache.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/store.hpp>

nano::vote_replay_cache::vote_replay_cache (nano::node & node_a) :
	node (node_a),
	stats (node_a.stats),
	ledger (node_a.ledger),
	active (node_a.active),
	store (node_a.vote_store),
	replay_vote_weight_minimum (node_a.config.replay_vote_weight_minimum.number ()),
	thread_prune ([this] () { run_prunning (); })
{
}

void nano::vote_replay_cache::stop ()
{
	{
		nano::lock_guard<nano::mutex> guard (mutex);
		stopped = true;
	}
	condition.notify_all ();
	if (thread_prune.joinable ())
	{
		thread_prune.join ();
	}
}

bool nano::vote_replay_cache::add_vote_to_db (nano::write_transaction const & transaction_a, std::shared_ptr<nano::vote> const & vote_a)
{
	return store.vote_storage.put (transaction_a, vote_a);
}

nano::vote_replay_cache::vote_cache_result nano::vote_replay_cache::get_vote_replay_cached_votes_for_hash (nano::transaction const & transaction_a, nano::block_hash hash_a) const
{
	auto votes_l = store.vote_storage.get (transaction_a, hash_a);

	nano::uint128_t weight = 0;
	for (auto const & vote : votes_l)
	{
		auto rep_weight (ledger.weight (vote->account));
		weight += rep_weight;
	}

	nano::vote_replay_cache::vote_cache_result result;

	if (weight >= replay_vote_weight_minimum)
	{
		result = std::make_pair (hash_a, votes_l);
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
			ledger.store.confirmation_height.get (transaction_a, account, conf_info);

			if (conf_info.frontier != 0 && conf_info.frontier != hash_a)
			{
				result = get_vote_replay_cached_votes_for_hash (transaction_vote_cache_a, conf_info.frontier);
				if (result)
				{
					stats.inc (nano::stat::type::vote_replay, nano::stat::detail::frontier_confirmation_successful);
				}
			}

			if (!result)
			{
				result = get_vote_replay_cached_votes_for_hash (transaction_vote_cache_a, hash_a);
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
			ledger.store.confirmation_height.get (transaction_a, account, conf_info);

			if (conf_info.frontier != 0)
			{
				result = get_vote_replay_cached_votes_for_hash (transaction_vote_cache_a, conf_info.frontier);
			}
		}
	}

	return result;
}

void nano::vote_replay_cache::run_prunning ()
{
	nano::thread_role::set (nano::thread_role::name::prune_votes);

	node.node_initialized_latch.wait ();

	const int max_ops_per_loop = 50000;

	while (!stopped)
	{
		nano::block_hash initial_hash;
		nano::random_pool::generate_block (initial_hash.bytes.data (), initial_hash.bytes.size ());

		nano::votes_replay_key prev_key (initial_hash, 0);

		{
			auto transaction (ledger.store.tx_begin_read ());
			auto transaction_vote_cache = store.tx_begin_write ({ tables::votes_replay });

			int done_this_loop = 0;
			int k = 0;
			for (auto i = store.vote_storage.begin (transaction_vote_cache, prev_key), n = store.vote_storage.end (); i != n && k < 50000; ++i, ++k)
			{
				const auto current_hash = i->first.block_hash ();

				if (current_hash != prev_key.block_hash ())
				{
					prev_key = i->first;

					bool prune = true;

					if (ledger.block_or_pruned_exists (transaction, current_hash))
					{
						auto account = ledger.account (transaction, current_hash);
						if (!account.is_zero ())
						{
							nano::confirmation_height_info conf_info;
							ledger.store.confirmation_height.get (transaction, account, conf_info);

							if (ledger.store.block.account_height (transaction, current_hash) >= conf_info.height)
							{
								prune = false;
							}
						}
					}

					if (prune)
					{
						store.vote_storage.del (transaction_vote_cache, current_hash);

						stats.inc (nano::stat::type::vote_replay, nano::stat::detail::outdated_version);

						++done_this_loop;
						if (done_this_loop >= max_ops_per_loop)
						{
							break;
						}
					}
				}
			}
		}

		std::this_thread::sleep_for (std::chrono::milliseconds (25));
//		std::this_thread::yield ();
	}
}
