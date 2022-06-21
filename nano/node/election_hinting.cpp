#include <nano/node/election_hinting.hpp>
#include <nano/node/node.hpp>

nano::election_hinting::election_hinting (nano::node & node) :
	node{ node },
	stopped{ false },
	thread{ [this] () { run (); } }
{
}

nano::election_hinting::~election_hinting ()
{
	stop ();
	thread.join ();
}

void nano::election_hinting::stop ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	stopped = true;
	notify ();
}

void nano::election_hinting::vote (const std::shared_ptr<nano::vote> & vote)
{
	debug_assert (vote != nullptr);

	auto weight = node.ledger.weight (vote->account);

	for (auto const & hash : *vote)
	{
		vote_impl (hash, vote->account, vote->timestamp (), weight);
	}
}

void nano::election_hinting::vote_impl (const nano::block_hash & hash, const nano::account & representative, uint64_t const & timestamp, const nano::uint128_t & rep_weight)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	auto & cache_by_hash = cache.get<tag_hash> ();
	if (auto existing = cache_by_hash.find (hash); existing != cache_by_hash.end ())
	{
		cache_by_hash.modify (existing, [&hash, &representative, &timestamp, &rep_weight] (inactive_cache_entry & entry) {
			entry.vote (representative, timestamp, rep_weight);
		});
	}
	else
	{
		inactive_cache_entry entry{ nano::milliseconds_since_epoch (), hash };
		entry.vote (representative, timestamp, rep_weight);
		cache.get<tag_hash> ().insert (std::move (entry));

		// When cache overflown remove the oldest entry
		if (cache.size () > node.flags.inactive_votes_cache_size)
		{
			cache.get<tag_random_access> ().pop_front ();
		}
	}

	notify ();
}

void nano::election_hinting::flush ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	condition.wait (lock, [this] () {
		return stopped || empty_locked () || node.active.vacancy () <= 0;
	});
}

bool nano::election_hinting::empty_locked () const
{
	return cache.empty ();
}

bool nano::election_hinting::empty () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return empty_locked ();
}

std::size_t nano::election_hinting::size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return cache.size ();
}

void nano::election_hinting::notify ()
{
	condition.notify_all ();
}

bool nano::election_hinting::cache_predicate () const
{
	if (!cache.empty () && node.active.vacancy () > 0)
	{
		auto & cache_by_tally = cache.get<tag_tally> ();
		const auto & it = cache_by_tally.begin ();
		return it->voters.size () >= election_start_voters_min && it->tally >= election_start_tally_min;
	}
	return false;
}

void nano::election_hinting::run ()
{
	nano::thread_role::set (nano::thread_role::name::election_hinting);
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait (lock, [this] () {
			return stopped || cache_predicate ();
		});
		debug_assert ((std::this_thread::yield (), true)); // Introduce some random delay in debug builds
		if (!stopped)
		{
			if (cache_predicate ())
			{
				auto & cache_by_tally = cache.get<tag_tally> ();
				const auto & it = cache_by_tally.begin ();
				inactive_cache_entry entry = *it;
				cache_by_tally.erase (it);
				lock.unlock ();

				auto transaction (node.store.tx_begin_read ());
				auto block = node.store.block.get (transaction, entry.hash);
				if (block != nullptr)
				{
					if (!node.block_confirmed_or_being_confirmed (transaction, entry.hash))
					{
						auto election = node.active.insert_hinted (block);
						// Although we check for vacancy, a race condition where both election scheduler and election hinter both insert the same election is possible
						if (election != nullptr)
						{
							entry.fill (*election);
						}
					}
				}
				else
				{
					// Missing block in ledger to start an election
					// TODO: When new bootstrapper is ready add logic to queue this block
				}
			}
			notify ();
			lock.lock ();
		}
	}
}

nano::election_hinting::inactive_cache_entry::inactive_cache_entry (const nano::millis_t & arrival, const nano::block_hash & hash) :
	arrival{ arrival },
	hash{ hash }
{
}

bool nano::election_hinting::inactive_cache_entry::vote (const nano::account & representative, const uint64_t & timestamp, const nano::uint128_t & rep_weight)
{
	auto existing = std::find_if (voters.begin (), voters.end (), [&representative] (auto const & item) { return item.first == representative; });
	if (existing != voters.end ())
	{
		// Update timestamp but tally remains unchanged as we already counter this rep
		if (timestamp > existing->second)
		{
			existing->second = timestamp;
		}
		return false;
	}
	else
	{
		// Vote from an unseend representative, add to list and update tally
		if (voters.size () < max_voters)
		{
			voters.emplace_back (representative, timestamp);
			tally += rep_weight;
			return true;
		}
		else
		{
			return false;
		}
	}
}

void nano::election_hinting::inactive_cache_entry::fill (nano::election & election)
{
	for (auto & voter : voters)
	{
		election.vote (voter.first, voter.second, hash);
	}
}