#pragma once

#include <nano/lib/assert.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace mi = boost::multi_index;

namespace nano::consensus
{
struct vote final
{
	nano::block_hash hash;
	nano::account representative;
	nano::amount weight;
	nano::vote_timestamp timestamp;

	static nano::vote_timestamp constexpr final_timestamp = std::numeric_limits<nano::vote_timestamp>::max ();
};

inline bool is_final_timestamp (nano::vote_timestamp timestamp)
{
	return timestamp == nano::consensus::vote::final_timestamp;
}

// Query multiple weights atomically
using weights = std::unordered_map<nano::account, nano::amount::underlying_type>;
using weights_query = std::function<weights (std::unordered_set<nano::account> const &)>;

class vote_index final
{
public:
	bool vote (consensus::vote const & vote)
	{
		if (auto existing = votes.get<tag_representative> ().find (vote.representative); existing != votes.get<tag_representative> ().end ())
		{
			// Only update if the new vote is newer
			if (existing->timestamp < vote.timestamp)
			{
				votes.get<tag_representative> ().modify (existing, [&vote] (auto & existing_vote) {
					existing_vote = vote;
				});
				return true; // Updated
			}
		}
		else
		{
			votes.insert (vote);
			return true; // Inserted
		}
		return false; // Ignored
	}

	// Find the block with the most votes or if there is a tie, the block with the lowest hash
	std::optional<nano::block_hash> leader () const
	{
		auto tally = summarize_tally ();
		// If several elements in the range are equivalent to the greatest element, returns the iterator to the first such element
		auto max = std::max_element (tally.begin (), tally.end (), [] (auto const & lhs, auto const & rhs) {
			return lhs.second < rhs.second;
		});
		if (max != tally.end ())
		{
			return max->first;
		}
		return std::nullopt;
	}

	std::optional<nano::block_hash> reached_quorum (nano::amount quorum_delta) const
	{
		release_assert (quorum_delta > 0);

		auto tally = summarize_tally ();
		for (auto const & [hash, amount] : tally)
		{
			if (amount >= quorum_delta)
			{
				return hash;
			}
		}
		return std::nullopt;
	}

	std::optional<nano::block_hash> reached_final_quorum (nano::amount quorum_delta) const
	{
		release_assert (quorum_delta > 0);

		// Count only final votes
		auto tally = summarize_tally (nano::consensus::vote::final_timestamp);
		for (auto const & [hash, amount] : tally)
		{
			if (amount >= quorum_delta)
			{
				return hash;
			}
		}
		return std::nullopt;
	}

	std::map<nano::account, nano::block_hash> summarize_participants (nano::vote_timestamp timestamp_cutoff = 0) const
	{
		std::map<nano::account, nano::block_hash> result;
		for (auto const & vote : votes)
		{
			if (vote.timestamp >= timestamp_cutoff)
			{
				result.insert ({ vote.representative, vote.hash });
			}
		}
		return result;
	}

	std::map<nano::block_hash, nano::amount> summarize_tally (nano::vote_timestamp timestamp_cutoff = 0) const
	{
		std::map<nano::block_hash, nano::amount> result;
		for (auto const & vote : votes)
		{
			if (vote.timestamp >= timestamp_cutoff)
			{
				result[vote.hash] = result[vote.hash].number () + vote.weight.number ();
			}
		}
		return result;
	}

	std::deque<nano::consensus::vote> all_votes () const
	{
		std::deque<nano::consensus::vote> result;
		for (auto const & vote : votes)
		{
			result.push_back (vote);
		}
		return result;
	}

	std::optional<nano::consensus::vote> find_vote (nano::account const & account) const
	{
		if (auto existing = votes.get<tag_representative> ().find (account); existing != votes.get<tag_representative> ().end ())
		{
			return *existing;
		}
		return std::nullopt;
	}

	nano::amount total_weight () const
	{
		nano::amount::underlying_type result{ 0 };
		for (auto const & vote : votes)
		{
			result += vote.weight.number ();
		}
		return result;
	}

	size_t size () const
	{
		return votes.size ();
	}

	bool contains (nano::block_hash const & hash) const
	{
		return votes.get<tag_hash> ().contains (hash);
	}

	bool contains (nano::account const & account) const
	{
		return votes.get<tag_representative> ().contains (account);
	}

public:
	// clang-format off
	class tag_representative {};
	class tag_hash {};
	class tag_timestamp {};

	using ordered_votes = boost::multi_index_container<consensus::vote,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_representative>,
			mi::member<nano::consensus::vote, nano::account, &nano::consensus::vote::representative>>,
		mi::ordered_non_unique<mi::tag<tag_hash>,
			mi::member<nano::consensus::vote, nano::block_hash, &nano::consensus::vote::hash>>,
		mi::ordered_non_unique<mi::tag<tag_timestamp>,
			mi::member<nano::consensus::vote, nano::vote_timestamp, &nano::consensus::vote::timestamp>>
	>>;
	// clang-format on

	ordered_votes votes;
};

enum class election_state
{
	// No quorum reached yet, vote with normal vote on block with the highest tally
	no_quorum,
	// Normal quorum reached, lock the candidate with the highest tally and keep voting on it with final votes
	quorum_reached,
	// Final quorum reached, election is decided, the winner is the candidate with the highest tally (might not be the same as our locked candidate)
	final_quorum_reached,
};

struct vote_request
{
	nano::block_hash hash;
	nano::vote_timestamp timestamp;

	bool is_final () const
	{
		return consensus::is_final_timestamp (timestamp);
	}
};

class election final
{
private: // States
	// State where one block has reached final quorum
	// Election is decided, the winner might be different from the candidate
	// Vote on the candidate only if the candidate is present in the ledger
	struct final_quorum_reached_state
	{
		static election_state constexpr state = election_state::final_quorum_reached;

		nano::block_hash candidate; // Our final candidate, might not be the same as the winner
		nano::block_hash winner; // The winner of the election (reached final vote quorum)
	};

	// State where one block has reached quorum but not final quorum yet
	// Attempt to force the winning fork into the ledger
	// Vote on the candidate only if the candidate is present in the ledger
	struct quorum_reached_state
	{
		static election_state constexpr state = election_state::quorum_reached;

		nano::block_hash candidate; // Our final candidate, the block that first reached non-final quorum
	};

	// State where no block has reached quorum yet (final or non-final)
	// Vote on the block currently held in the ledger
	struct no_quorum_state
	{
		static election_state constexpr state = election_state::no_quorum;

		// No quorum reached yet, no candidate locked
	};

	using state_variant = std::variant<final_quorum_reached_state, quorum_reached_state, no_quorum_state>;

	// Define processing vote behavior for each election state
	struct vote_visitor
	{
		using result = std::pair<bool, std::optional<state_variant>>; // <vote processed, new state (optional)>

		consensus::vote_index & votes;
		consensus::vote const & vote;
		nano::amount quorum_delta;

		result operator() (final_quorum_reached_state const & state) const
		{
			return { false, std::nullopt }; // Election is already decided, ignore any additional votes
		}

		result operator() (quorum_reached_state const & state) const
		{
			bool updated = votes.vote (vote);
			if (!updated)
			{
				return { false, std::nullopt }; // Not a new vote, ignore
			}

			// Check if final quorum reached
			if (auto winner = votes.reached_final_quorum (quorum_delta))
			{
				return { true, final_quorum_reached_state{ /* our final candidate should be locked */ state.candidate, winner.value () } };
			}
			// Check if winner of quorum changed
			if (auto winner = votes.reached_quorum (quorum_delta))
			{
				return { true, quorum_reached_state{ winner.value () } };
			}

			return { true, std::nullopt }; // Vote processed without changing state
		}

		result operator() (no_quorum_state const & state) const
		{
			bool updated = votes.vote (vote);
			if (!updated)
			{
				return { false, std::nullopt }; // Not a new vote, ignore
			}

			// First check final quorum in case final quorum is reached before normal quorum
			if (auto winner = votes.reached_final_quorum (quorum_delta))
			{
				return { true, final_quorum_reached_state{ winner.value (), winner.value () } };
			}
			if (auto winner = votes.reached_quorum (quorum_delta))
			{
				return { true, quorum_reached_state{ winner.value () } };
			}

			return { true, std::nullopt }; // Vote processed without changing state
		}
	};

	// Define vote generation behavior for each election state
	// Only votes for blocks that are or were present (checked successfully) in the ledger
	struct request_visitor
	{
		using result = std::optional<vote_request>;

		consensus::vote_index const & votes;
		nano::block_hash const & current; // Block currently held in the ledger
		nano::vote_timestamp round;

		result operator() (final_quorum_reached_state const & state) const
		{
			// Keep voting for our candidate with final votes only if it's present in the ledger
			if (current == state.candidate)
			{
				return vote_request{ state.candidate, consensus::vote::final_timestamp };
			}
			return std::nullopt;
		}

		result operator() (quorum_reached_state const & state) const
		{
			// Keep voting for our candidate with final votes only if it's present in the ledger
			if (current == state.candidate)
			{
				return vote_request{ state.candidate, consensus::vote::final_timestamp };
			}
			return std::nullopt;
		}

		result operator() (no_quorum_state const & state) const
		{
			// No quorum reached yet, vote on the block currently held in the ledger
			return vote_request{ current, round };
		}
	};

	// Define candidate behavior for each election state
	// Candidate might be a block we haven't checked yet
	struct candidate_visitor
	{
		using result = std::optional<nano::block_hash>;

		consensus::vote_index const & votes;

		result operator() (final_quorum_reached_state const & state) const
		{
			return state.candidate;
		}

		result operator() (quorum_reached_state const & state) const
		{
			return state.candidate;
		}

		result operator() (no_quorum_state const & state) const
		{
			// No quorum reached yet, no candidate locked, return the block with the highest tally
			return votes.leader ();
		}
	};

	// Winner can only be determined if final quorum is reached
	struct winner_visitor
	{
		std::optional<nano::block_hash> operator() (final_quorum_reached_state const & state) const
		{
			return state.winner;
		}
		std::optional<nano::block_hash> operator() (quorum_reached_state const & state) const
		{
			return std::nullopt;
		}
		std::optional<nano::block_hash> operator() (no_quorum_state const & state) const
		{
			return std::nullopt;
		}
	};

public:
	// Process incoming votes
	bool vote (consensus::vote const & vote, nano::amount quorum_delta)
	{
		auto [processed, new_state] = std::visit (vote_visitor{ votes, vote, quorum_delta }, state_var);
		debug_assert (!new_state || processed); // If not processed, the state should not change
		if (new_state)
		{
			state_var = *new_state;
		}
		return processed;
	}

	// Generate vote request
	std::optional<vote_request> request (nano::block_hash const & current, nano::vote_timestamp round) const
	{
		auto result = std::visit (request_visitor{ votes, current, round }, state_var);
		release_assert (!result || result->hash == current); // Only allow voting on the currently checked block
		return result;
	}

	// Candidate is a block we might want to switch our ledger to
	std::optional<nano::block_hash> candidate () const
	{
		return std::visit (candidate_visitor{ votes }, state_var);
	}

	// Final winner of the election
	std::optional<nano::block_hash> winner () const
	{
		return std::visit (winner_visitor{}, state_var);
	}

	election_state state () const
	{
		return std::visit ([] (auto const & state) { return state.state; }, state_var);
	}

	auto leader () const
	{
		return votes.leader ();
	}
	auto all_votes () const
	{
		return votes.all_votes ();
	}
	auto find_vote (nano::account const & account) const
	{
		return votes.find_vote (account);
	}
	auto total_weight () const
	{
		return votes.total_weight ();
	}
	auto tally () const
	{
		return votes.summarize_tally ();
	}
	auto final_tally () const
	{
		return votes.summarize_tally (consensus::vote::final_timestamp);
	}
	auto participants () const
	{
		return votes.summarize_participants ();
	}
	auto final_participants () const
	{
		return votes.summarize_participants (consensus::vote::final_timestamp);
	}
	auto size () const
	{
		return votes.size ();
	}
	auto contains (nano::block_hash const & hash) const
	{
		return votes.contains (hash);
	}
	auto contains (nano::account const & account) const
	{
		return votes.contains (account);
	}

private:
	consensus::vote_index votes;
	state_variant state_var{ no_quorum_state{} };
};
}