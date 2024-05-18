#include <nano/node/node.hpp>
#include <nano/node/vote_rebroadcaster.hpp>

nano::vote_rebroadcaster::vote_rebroadcaster (nano::node & node) :
	node{ node },
	queue{ node.stats, nano::stat::type::vote_rebroadcaster, nano::thread_role::name::vote_rebroadcaster, /* single threaded */ 1, /* max queue size */ 1024 * 8, /* max batch size */ 1024 }
{
	queue.process_batch = [this] (auto const & batch) {
		process_batch (batch);
	};
}

void nano::vote_rebroadcaster::start ()
{
	queue.start ();
}

void nano::vote_rebroadcaster::stop ()
{
	queue.stop ();
}

void nano::vote_rebroadcaster::put (std::shared_ptr<nano::vote> const & vote)
{
	queue.add (vote);
}

void nano::vote_rebroadcaster::process_batch (std::deque<std::shared_ptr<nano::vote>> const & batch)
{
	auto const reps = node.wallets.reps ();

	if (reps.have_half_rep ())
	{
		return;
	}

	for (auto const & vote : batch)
	{
		if (!reps.exists (vote->account))
		{
			node.stats.inc (nano::stat::type::vote_rebroadcaster, nano::stat::detail::rebroadcast);
			node.network.flood_vote (vote, 0.5f, /* rebroadcasted */ true);
		}
	}
}
