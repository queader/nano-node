#include <nano/node/block_arrival.hpp>
#include <nano/node/block_broadcast.hpp>
#include <nano/node/blockprocessor.hpp>
#include <nano/node/network.hpp>

nano::block_broadcast::block_broadcast (nano::block_processor & block_processor, nano::network & network, nano::block_arrival & block_arrival, bool enabled) :
	block_processor{ block_processor },
	network{ network },
	block_arrival{ block_arrival },
	enabled{ enabled }
{
	if (!enabled)
	{
		return;
	}

	block_processor.processed.add ([this] (auto const & result, auto const & block) {
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
}

void nano::block_broadcast::observe (std::shared_ptr<nano::block> const & block)
{
	bool is_local = local.contains (block->hash ());
	if (is_local)
	{
		// Block created on this node
		// Perform more agressive initial flooding
		network.flood_block_initial (block);
	}
	else
	{
		if (block_arrival.recent (block->hash ()))
		{
			// Block arrived from realtime traffic, do normal gossip.
			network.flood_block (block, nano::transport::buffer_drop_policy::limiter);
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