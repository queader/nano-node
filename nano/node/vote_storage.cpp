#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/vote_storage.hpp>
#include <nano/secure/store.hpp>

nano::vote_storage::vote_storage (nano::node & node_a, nano::store & store_a, nano::network & network_a, nano::ledger & ledger_a, nano::stat & stats_a) :
	node{ node_a },
	store{ store_a },
	network{ network_a },
	ledger{ ledger_a },
	stats{ stats_a },
	store_queue{ stats, nano::stat::type::vote_storage_store, nano::thread_role::name::vote_storage_store, /* single threaded */ 1, 1024 * 64, 1024 },
	broadcast_queue{ stats, nano::stat::type::vote_storage, nano::thread_role::name::vote_storage, /* threads */ 2, 1024 * 64, 1024 }
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

void nano::vote_storage::vote (std::shared_ptr<nano::vote> const & vote)
{
	store_queue.add (vote);
}

void nano::vote_storage::trigger (const nano::block_hash & hash, const std::shared_ptr<nano::transport::channel> & channel)
{
	broadcast_queue.add ({ hash, channel });
}

void nano::vote_storage::process_batch (decltype (store_queue)::batch_t & batch)
{
	auto transaction = store.tx_begin_write ({ tables::vote_storage });

	for (auto & vote : batch)
	{
		auto result = store.vote_storage.put (transaction, vote);
		if (result > 0)
		{
			stats.inc (nano::stat::type::vote_storage_store, nano::stat::detail::stored);
			stats.add (nano::stat::type::vote_storage_store, nano::stat::detail::stored_votes, result);
		}
	}
}

void nano::vote_storage::process_batch (decltype (broadcast_queue)::batch_t & batch)
{
	auto transaction = store.tx_begin_read ();

	for (auto & [hash, channel] : batch)
	{
		auto votes = store.vote_storage.get (transaction, hash);
		if (!votes.empty ())
		{
			if (weight (votes) > vote_weight_threshold)
			{
				votes = filter (votes);

				reply (votes, channel);

				nano::unique_lock<nano::mutex> lock{ mutex };
				if (recently_broadcasted.count (hash) > 0)
				{
					stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_duplicate);
					continue;
				}
				else
				{
					recently_broadcasted.insert (hash);
					if (recently_broadcasted.size () > 1024)
					{
						recently_broadcasted.clear ();
					}

					lock.unlock ();

					broadcast (votes);
				}
			}
			else
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::low_weight);
			}
		}
		else
		{
			stats.inc (nano::stat::type::vote_storage, nano::stat::detail::empty);
		}
	}
}

void nano::vote_storage::reply (const nano::vote_storage::vote_list_t & votes, const std::shared_ptr<nano::transport::channel> & channel)
{
	if (channel->max ())
	{
		stats.inc (nano::stat::type::vote_storage, nano::stat::detail::channel_full, nano::stat::dir::out);
		return;
	}

	stats.inc (nano::stat::type::vote_storage, nano::stat::detail::reply, nano::stat::dir::out);

	for (auto & vote : votes)
	{
		stats.inc (nano::stat::type::vote_storage, nano::stat::detail::reply_vote, nano::stat::dir::out);

		nano::confirm_ack message{ node.network_params.network, vote };
		channel->send (
		message, [this] (auto & ec, auto size) {
			if (ec)
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::write_error, nano::stat::dir::out);
			}
		},
		nano::buffer_drop_policy::no_limiter_drop, nano::bandwidth_limit_type::vote_storage);
	}
}

void nano::vote_storage::broadcast (const nano::vote_storage::vote_list_t & votes)
{
	stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast);

	auto pr_nodes = node.rep_crawler.principal_representatives ();
	auto random_nodes = network.list (network.fanout ());

	for (auto & vote : votes)
	{
		stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_vote);

		nano::confirm_ack message{ node.network_params.network, vote };

		// Send to all representatives
		for (auto const & rep : pr_nodes)
		{
			stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_vote_rep);

			rep.channel->send (
			message, [this] (auto & ec, auto size) {
				if (ec)
				{
					stats.inc (nano::stat::type::vote_storage, nano::stat::detail::write_error, nano::stat::dir::in);
				}
			},
			nano::buffer_drop_policy::no_limiter_drop, nano::bandwidth_limit_type::vote_storage);
		}

		//		// Send to some random nodes
		//		for (auto const & channel : random_nodes)
		//		{
		//			stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_vote_random);
		//
		//			channel->send (
		//			message, [this] (auto & ec, auto size) {
		//				if (ec)
		//				{
		//					stats.inc (nano::stat::type::vote_storage, nano::stat::detail::write_error, nano::stat::dir::in);
		//				}
		//			},
		//			nano::buffer_drop_policy::limiter, nano::bandwidth_limit_type::vote_storage);
		//		}
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
	return result;
}