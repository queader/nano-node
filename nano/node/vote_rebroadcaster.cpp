#include <nano/node/node.hpp>
#include <nano/node/vote_rebroadcaster.hpp>

nano::vote_rebroadcaster::vote_rebroadcaster (nano::node & node) :
	config{ node.config.vote_rebroadcaster },
	node{ node },
	stats{ node.stats }
{
}

nano::vote_rebroadcaster::~vote_rebroadcaster ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::vote_rebroadcaster::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::vote_rebroadcasting);
		run ();
	} };
}

void nano::vote_rebroadcaster::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	nano::join_or_pass (thread);
}

bool nano::vote_rebroadcaster::rebroadcast (const std::shared_ptr<nano::vote> & vote, nano::rep_tier tier)
{
	debug_assert (tier != nano::rep_tier::none); // Should be filtered before calling this function

	nano::lock_guard<nano::mutex> lock{ mutex };

	bool const vote_is_final = vote->is_final ();
	auto & recent = recently_broadcasted[vote->account];

	auto check_and_update = [&recent, vote_is_final] (const auto & hash) {
		bool is_new = false;
		// Keep reference to avoid multiple lookups
		auto & status = recent.votes[hash];
		// Vote is considered new if it's the first time we see it or if it's final and we haven't seen it as final before
		is_new = (status == vote_state::empty || (status == vote_state::non_final && vote_is_final));
		status = vote_is_final ? vote_state::final : vote_state::non_final;
		return is_new;
	};

	int new_hashes = 0;
	for (auto const & hash : vote->hashes)
	{
		if (check_and_update (hash))
		{
			++new_hashes;
		}
	}

	stats.add (nano::stat::type::vote_rebroadcaster, nano::stat::detail::new_hashes, nano::stat::dir::in, new_hashes);

	if (new_hashes > 0)
	{
		// TODO: Insert into fair queue

		return true;
	}
	else
	{
		return false;
	}
}