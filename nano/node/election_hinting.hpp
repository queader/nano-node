#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/timer.hpp>
#include <nano/lib/utility.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <condition_variable>
#include <memory>
#include <queue>
#include <thread>
#include <vector>

namespace mi = boost::multi_index;

namespace nano
{
class node;
class vote;
class election;

/**
 *	A container holding votes that do not match any active or recently finished elections.
 */
class election_hinting final
{
	int election_start_voters_min{ 15 };
	nano::uint128_t election_start_tally_min{ 40000 * nano::Gxrb_ratio };

public:
	explicit election_hinting (nano::node & node);
	~election_hinting ();
	void stop ();
	void vote (std::shared_ptr<nano::vote> const & vote);
	void flush ();
	void notify ();
	std::size_t size () const;
	bool empty () const;

private:
	void vote_impl (nano::block_hash const & hash, nano::account const & representative, uint64_t const & timestamp, nano::uint128_t const & rep_weight);
	void run ();
	bool empty_locked () const;
	bool cache_predicate () const;

	nano::node & node;
	bool stopped;
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;

private:
	// clang-format off
	class tag_random_access {};
	class tag_tally {};
	class tag_hash {};
	// clang-format on

	class inactive_cache_entry final
	{
	public:
		constexpr static int max_voters = 40;

		inactive_cache_entry (nano::millis_t const & arrival, nano::block_hash const & hash);

		nano::millis_t arrival;
		nano::block_hash hash;
		std::vector<std::pair<nano::account, nano::millis_t>> voters; // <rep, timestamp> pair
		nano::uint128_t tally{ 0 };

		bool vote (nano::account const & representative, uint64_t const & timestamp, nano::uint128_t const & rep_weight);
		void fill (nano::election & election);
	};

	// clang-format off
	using ordered_cache = boost::multi_index_container<inactive_cache_entry,
	mi::indexed_by<
		mi::random_access<mi::tag<tag_random_access>>,
		mi::ordered_non_unique<mi::tag<tag_tally>,
			mi::member<inactive_cache_entry, nano::uint128_t, &inactive_cache_entry::tally>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<inactive_cache_entry, nano::block_hash, &inactive_cache_entry::hash>>>>;
	// clang-format on
	ordered_cache cache;
};
}