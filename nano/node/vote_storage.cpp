#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/vote_storage.hpp>

nano::vote_storage::vote_storage (nano::node & node_a, nano::store::component & vote_store_a, nano::network & network_a, nano::ledger & ledger_a, nano::stats & stats_a) :
	node{ node_a },
	vote_store{ vote_store_a },
	network{ network_a },
	ledger{ ledger_a },
	stats{ stats_a },
	store_queue{ stats, nano::stat::type::vote_storage_write, nano::thread_role::name::vote_storage, /* single threaded */ 1, 1024 * 64, 1024 },
	broadcast_queue{ stats, nano::stat::type::vote_storage_broadcast, nano::thread_role::name::vote_storage, /* threads */ 1, 1024 * 4, 512 }
{
	store_queue.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};

	broadcast_queue.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};
}

nano::vote_storage::~vote_storage ()
{
	// All threads should be stopped before destruction
	debug_assert (!store_queue.joinable ());
	debug_assert (!broadcast_queue.joinable ());
}

void nano::vote_storage::start ()
{
	store_queue.start ();
	broadcast_queue.start ();
}

void nano::vote_storage::stop ()
{
	store_queue.stop ();
	broadcast_queue.stop ();
}

void nano::vote_storage::vote (std::shared_ptr<nano::vote> vote)
{
	if (ledger.weight (vote->account) >= rep_weight_threshold)
	{
		store_queue.add (vote);
	}
}

void nano::vote_storage::trigger (const nano::block_hash & hash, const std::shared_ptr<nano::transport::channel> & channel)
{
	if (!trigger_pr_only || node.rep_crawler.is_pr (*channel))
	{
		broadcast_queue.add (broadcast_entry_t{ hash, channel });
	}
}

void nano::vote_storage::process_batch (decltype (store_queue)::batch_t & batch)
{
	auto vote_transaction = vote_store.tx_begin_write ({ tables::vote_storage });

	for (auto & vote : batch)
	{
		auto result = vote_store.vote_storage.put (vote_transaction, vote);
		if (result > 0)
		{
			stats.inc (nano::stat::type::vote_storage_write, nano::stat::detail::stored);
			stats.add (nano::stat::type::vote_storage_write, nano::stat::detail::stored_votes, nano::stat::dir::in, result);
		}
	}
}

void nano::vote_storage::process_batch (decltype (broadcast_queue)::batch_t & batch)
{
	auto vote_transaction = vote_store.tx_begin_read ();
	//	auto ledger_transaction = ledger.store.tx_begin_read ();

	for (auto & [hash, channel] : batch)
	{
		// Check votes for specific hash
		{
			auto votes = query_hash (vote_transaction, hash);
			if (!votes.empty ())
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::reply);

				reply (votes, hash, channel);

				if (enable_broadcast)
				{
					broadcast (votes, hash);
				}
			}
			else
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::empty);
			}
		}

		// Check votes for frontier
		//		if (enable_query_frontier)
		//		{
		//			auto [frontier_votes, frontier_hash] = query_frontier (ledger_transaction, vote_transaction, hash);
		//			if (!frontier_votes.empty ())
		//			{
		//				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::frontier);
		//
		//				reply (frontier_votes, channel);
		//
		//				if (enable_broadcast)
		//				{
		//					broadcast (frontier_votes, frontier_hash);
		//				}
		//			}
		//			else
		//			{
		//				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::frontier_empty);
		//			}
		//		}
	}
}

void nano::vote_storage::reply (const nano::vote_storage::vote_list_t & votes, const nano::block_hash & hash, const std::shared_ptr<nano::transport::channel> & channel)
{
	if (channel->max (nano::transport::traffic_type::vote_storage)) // TODO: Scrutinize this
	{
		stats.inc (nano::stat::type::vote_storage, nano::stat::detail::reply_channel_full, nano::stat::dir::out);
		return;
	}

	if (recently_broadcasted (channel, hash))
	{
		stats.inc (nano::stat::type::vote_storage, nano::stat::detail::reply_duplicate, nano::stat::dir::out);
		return;
	}

	stats.inc (nano::stat::type::vote_storage, nano::stat::detail::reply, nano::stat::dir::out);
	stats.add (nano::stat::type::vote_storage, nano::stat::detail::reply_vote, nano::stat::dir::out, votes.size ());

	for (auto & vote : votes)
	{
		nano::confirm_ack message{ node.network_params.network, vote };

		channel->send (
		message, [this] (auto & ec, auto size) {
			if (ec)
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::write_error, nano::stat::dir::out);
			}
		},
		nano::transport::buffer_drop_policy::no_socket_drop, nano::transport::traffic_type::vote_storage);
	}
}

bool nano::vote_storage::recently_broadcasted (const std::shared_ptr<nano::transport::channel> & channel, const nano::block_hash & hash)
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	std::chrono::seconds constexpr cleanup_interval{ 30 };
	std::chrono::seconds constexpr rebroadcast_interval{ 60 };

	// Cleanup
	if (nano::elapse (last_cleanup, cleanup_interval))
	{
		for (auto & [channel, entries] : recently_broadcasted_map)
		{
			erase_if (entries, [&] (auto const & entry) {
				auto const & [hash, time] = entry;
				return nano::elapsed (time, rebroadcast_interval);
			});
		}

		erase_if (recently_broadcasted_map, [&] (auto const & pair) {
			auto const & [channel, entries] = pair;
			if (!channel->alive ())
			{
				return true; // Erase
			}
			if (entries.empty ())
			{
				return true; // Erase
			}
			return false;
		});
	}

	if (auto it = recently_broadcasted_map.find (channel); it != recently_broadcasted_map.end ())
	{
		auto & entries = it->second;
		if (entries.contains (hash))
		{
			return true;
		}
	}
	recently_broadcasted_map[channel].emplace (hash, std::chrono::steady_clock::now ());
	return false;
}

void nano::vote_storage::broadcast (const nano::vote_storage::vote_list_t & votes, const nano::block_hash & hash)
{
	stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast, nano::stat::dir::out);

	if (enable_pr_broadcast)
	{
		auto pr_nodes = node.rep_crawler.principal_representatives ();
		for (auto const & rep : pr_nodes)
		{
			if (rep.channel->max (nano::transport::traffic_type::vote_storage)) // TODO: Scrutinize this
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_channel_full, nano::stat::dir::out);
				continue;
			}

			if (recently_broadcasted (rep.channel, hash))
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_duplicate, nano::stat::dir::out);
				continue;
			}

			stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_rep, nano::stat::dir::out);
			stats.add (nano::stat::type::vote_storage, nano::stat::detail::broadcast_vote, nano::stat::dir::out, votes.size ());

			for (auto & vote : votes)
			{
				nano::confirm_ack message{ node.network_params.network, vote };

				rep.channel->send (
				message, [this] (auto & ec, auto size) {
					if (ec)
					{
						stats.inc (nano::stat::type::vote_storage, nano::stat::detail::write_error_broadcast, nano::stat::dir::out);
					}
				},
				nano::transport::buffer_drop_policy::no_socket_drop, nano::transport::traffic_type::vote_storage);
			}
		}
	}

	if (enable_random_broadcast)
	{
		auto random_nodes = network.list (network.fanout ());
		for (auto const & channel : random_nodes)
		{
			if (channel->max (nano::transport::traffic_type::vote_storage)) // TODO: Scrutinize this
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_channel_full, nano::stat::dir::in);
				continue;
			}

			if (recently_broadcasted (channel, hash))
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_duplicate, nano::stat::dir::in);
				continue;
			}

			stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_random, nano::stat::dir::in);
			stats.add (nano::stat::type::vote_storage, nano::stat::detail::broadcast_vote, nano::stat::dir::in, votes.size ());

			for (auto & vote : votes)
			{
				nano::confirm_ack message{ node.network_params.network, vote };

				channel->send (
				message, [this] (auto & ec, auto size) {
					if (ec)
					{
						stats.inc (nano::stat::type::vote_storage, nano::stat::detail::write_error_broadcast, nano::stat::dir::in);
					}
				},
				nano::transport::buffer_drop_policy::no_socket_drop, nano::transport::traffic_type::vote_storage);
			}
		}
	}
}

nano::uint128_t nano::vote_storage::weight (const nano::vote_storage::vote_list_t & votes) const
{
	nano::uint128_t result = 0;
	for (auto const & vote : votes)
	{
		result += ledger.weight (vote->account);
	}
	return result;
}

nano::uint128_t nano::vote_storage::weight_final (const nano::vote_storage::vote_list_t & votes) const
{
	nano::uint128_t result = 0;
	for (auto const & vote : votes)
	{
		if (vote->is_final ())
		{
			result += ledger.weight (vote->account);
		}
	}
	return result;
}

nano::vote_storage::vote_list_t nano::vote_storage::filter (const nano::vote_storage::vote_list_t & votes) const
{
	nano::vote_storage::vote_list_t result;
	for (auto const & vote : votes)
	{
		if (ledger.weight (vote->account) >= rep_weight_threshold)
		{
			result.push_back (vote);
		}
	}
	return votes;
}

nano::vote_storage::vote_list_t nano::vote_storage::query_hash (const nano::store::transaction & vote_transaction, const nano::block_hash & hash, std::size_t count_threshold)
{
	auto votes = vote_store.vote_storage.get (vote_transaction, hash);
	if (!votes.empty ())
	{
		if (count_threshold == 0 || votes.size () >= count_threshold)
		{
			auto should_pass = [this] (auto const & votes) {
				if (vote_final_weight_threshold > 0)
				{
					return weight_final (votes) >= vote_final_weight_threshold;
				}
				if (vote_weight_threshold > 0)
				{
					return weight (votes) >= vote_weight_threshold;
				}
				return true;
			};

			if (should_pass (votes))
			{
				auto filtered = filter (votes);
				return filtered.size () >= count_threshold ? filtered : votes;
			}
			else
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::low_weight);
			}
		}
	}
	return {};
}

std::pair<nano::vote_storage::vote_list_t, nano::block_hash> nano::vote_storage::query_frontier (nano::store::transaction const & ledger_transaction, nano::store::transaction const & vote_transaction, const nano::block_hash & hash)
{
	auto account = ledger.account_safe (ledger_transaction, hash);
	if (account.is_zero ())
	{
		return {};
	}

	auto account_info = ledger.account_info (ledger_transaction, account);
	if (!account_info)
	{
		return {};
	}

	auto frontier = account_info->head;

	const int max_retries = 128;
	for (int n = 0; n < max_retries && !frontier.is_zero () && frontier != hash; ++n)
	{
		auto votes = query_hash (vote_transaction, frontier, /* needed for v23 vote hinting */ rep_count_threshold);
		if (!votes.empty ())
		{
			return { votes, frontier };
		}

		// TODO: Create `ledger.previous(hash)` helper
		auto block = ledger.store.block.get (ledger_transaction, frontier);
		if (block)
		{
			frontier = block->previous ();
		}
		else
		{
			frontier = { 0 };
		}
	}

	return {};
}

std::unique_ptr<nano::container_info_component> nano::vote_storage::collect_container_info (const std::string & name) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	auto total_size = std::accumulate (recently_broadcasted_map.begin (), recently_broadcasted_map.end (), std::size_t{ 0 }, [] (auto total, auto const & entry) {
		return total + entry.second.size ();
	});

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "recently_broadcasted", recently_broadcasted_map.size (), sizeof (decltype (recently_broadcasted_map)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "recently_broadcasted_total", total_size, sizeof (decltype (recently_broadcasted_map)::value_type::second_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "store_queue", store_queue.size (), 0 }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "broadcast_queue", broadcast_queue.size (), 0 }));
	return composite;
}