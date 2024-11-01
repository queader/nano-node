#pragma once

#include <nano/consensus/consensus.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/node/fwd.hpp>

#include <chrono>
#include <memory>

namespace nano
{
struct vote_info
{
	nano::block_hash hash;
	nano::vote_timestamp timestamp;
	std::chrono::steady_clock::time_point time;

	bool is_final () const
	{
		return timestamp == nano::consensus::vote::final_timestamp;
	}
};

// Map of vote weight per block, ordered in descending order
using election_tally = std::map<nano::amount::underlying_type, nano::block_hash, std::greater<>>;

/* Defines the possible states for an election to stop in */
enum class election_status_type : uint8_t
{
	ongoing = 0,
	active_confirmed_quorum = 1,
	active_confirmation_height = 2,
	inactive_confirmation_height = 3,
	stopped = 5,
};

nano::stat::detail to_stat_detail (election_status_type);

/* Holds a summary of an election */
struct election_status
{
	election_status_type type;
	std::shared_ptr<nano::block> winner; // Winner of the election if quorum is reached, nullptr otherwise
	nano::election_tally tally; // Tally of votes for blocks (normal + final)
	nano::election_tally final_tally; // Tally of votes for blocks (final only)
	nano::amount tally_weight{ 0 }; // Total weight of votes (normal + final)
	nano::amount final_tally_weight{ 0 }; // Total weight of votes (final only)
	std::chrono::milliseconds time_started{}; // Since epoch
	std::chrono::milliseconds time_ended{ nano::milliseconds (std::chrono::steady_clock::now ()) }; // Since epoch, only valid for finished elections
	std::chrono::milliseconds duration{};
	unsigned confirmation_request_count{ 0 };
	size_t block_count{ 0 };
	size_t voter_count{ 0 };
	std::unordered_map<nano::account, nano::vote_info> votes;
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> blocks;

	void operator() (nano::object_stream &) const;
};
}
