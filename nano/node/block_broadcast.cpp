#include <nano/lib/threading.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/block_arrival.hpp>
#include <nano/node/block_broadcast.hpp>
#include <nano/node/blockprocessor.hpp>
#include <nano/node/network.hpp>

nano::block_broadcast::block_broadcast (nano::block_processor & block_processor_a, nano::network & network_a, nano::block_arrival & block_arrival_a, nano::stats & stats_a, bool enabled_a) :
	block_processor{ block_processor_a },
	network{ network_a },
	block_arrival{ block_arrival_a },
	stats{ stats_a },
	enabled{ enabled_a },
	queue{ stats_a, nano::stat::type::block_broadcaster, nano::thread_role::name::block_broadcasting, /* single thread */ 1, max_size }
{
	if (!enabled)
	{
		return;
	}

	block_processor.processed.add ([this] (auto const & result, auto const & block, auto const & context) {
		switch (result.code)
		{
			case nano::process_result::progress:
				observe (block);
				break;
			default:
				break;
		}
		local.erase (block->hash ());
	});

	queue.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};
}

nano::block_broadcast::~block_broadcast ()
{
}

void nano::block_broadcast::start ()
{
	if (!enabled)
	{
		return;
	}

	queue.start ();
}

void nano::block_broadcast::stop ()
{
	queue.stop ();
}

void nano::block_broadcast::observe (std::shared_ptr<nano::block> const & block)
{
	bool is_local = local.contains (block->hash ());
	if (is_local)
	{
		// Block created on this node
		// Perform more agressive initial flooding
		queue.add (entry{ block, broadcast_strategy::aggressive });
	}
	else
	{
		if (block_arrival.recent (block->hash ()))
		{
			// Block arrived from realtime traffic, do normal gossip.
			queue.add (entry{ block, broadcast_strategy::normal });
		}
		else
		{
			// Block arrived from bootstrap
			// Don't broadcast blocks we're bootstrapping
		}
	}
}

void nano::block_broadcast::track_local (nano::block_hash const & hash)
{
	if (!enabled)
	{
		return;
	}
	local.add (hash);
}

void nano::block_broadcast::process_batch (queue_t::batch_t & batch)
{
	for (auto & [block, strategy] : batch)
	{
		switch (strategy)
		{
			case broadcast_strategy::normal:
			{
				stats.inc (nano::stat::type::block_broadcaster, nano::stat::detail::broadcast_normal, nano::stat::dir::out);
				network.flood_block (block, nano::transport::buffer_drop_policy::limiter);
			}
			break;
			case broadcast_strategy::aggressive:
			{
				stats.inc (nano::stat::type::block_broadcaster, nano::stat::detail::broadcast_aggressive, nano::stat::dir::out);
				network.flood_block_initial (block);
			}
			break;
		}
	}
}

/*
 * hash_tracker
 */

void nano::block_broadcast::hash_tracker::add (nano::block_hash const & hash)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	hashes.emplace_back (hash);
	while (hashes.size () > max_size)
	{
		// Erase oldest hashes
		hashes.pop_front ();
	}
}

void nano::block_broadcast::hash_tracker::erase (nano::block_hash const & hash)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	hashes.get<tag_hash> ().erase (hash);
}

bool nano::block_broadcast::hash_tracker::contains (nano::block_hash const & hash) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return hashes.get<tag_hash> ().find (hash) != hashes.get<tag_hash> ().end ();
}