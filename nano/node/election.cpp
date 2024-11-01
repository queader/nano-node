#include <nano/lib/blocks.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/confirmation_solicitor.hpp>
#include <nano/node/election.hpp>
#include <nano/node/local_vote_history.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/vote_generator.hpp>
#include <nano/node/vote_router.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/vote.hpp>

using namespace std::chrono;

std::chrono::milliseconds nano::election::base_latency () const
{
	return node.network_params.network.is_dev_network () ? 25ms : 1000ms;
}

std::chrono::milliseconds nano::election::confirm_req_time () const
{
	switch (behavior ())
	{
		case election_behavior::manual:
		case election_behavior::priority:
		case election_behavior::hinted:
			return base_latency () * 5;
		case election_behavior::optimistic:
			return base_latency () * 2;
	}
	debug_assert (false);
	return {};
}

std::chrono::milliseconds nano::election::time_to_live () const
{
	switch (behavior ())
	{
		case election_behavior::manual:
		case election_behavior::priority:
			return std::chrono::milliseconds (5 * 60 * 1000);
		case election_behavior::hinted:
		case election_behavior::optimistic:
			return std::chrono::milliseconds (30 * 1000);
	}
	debug_assert (false);
	return {};
}

std::chrono::seconds nano::election::cooldown_time (nano::uint128_t weight) const
{
	auto online_stake = node.online_reps.trended ();
	if (weight > online_stake / 20) // Reps with more than 5% weight
	{
		return std::chrono::seconds{ 1 };
	}
	if (weight > online_stake / 100) // Reps with more than 1% weight
	{
		return std::chrono::seconds{ 5 };
	}
	// The rest of smaller reps
	return std::chrono::seconds{ 15 };
}

/*
 * election
 */

nano::election::election (nano::node & node_a, std::shared_ptr<nano::block> const & block_a, std::function<void (std::shared_ptr<nano::block> const &)> const & confirmation_action_a, std::function<void (nano::account const &)> const & live_vote_action_a, nano::election_behavior election_behavior_a) :
	confirmation_action (confirmation_action_a),
	live_vote_action (live_vote_action_a),
	node (node_a),
	behavior_m (election_behavior_a),
	height (block_a->sideband ().height),
	root (block_a->root ()),
	qualified_root (block_a->qualified_root ()),
	current_block{ block_a }
{
	blocks[block_a->hash ()] = block_a;
}

bool nano::election::confirm_once (std::shared_ptr<nano::block> const & winner)
{
	debug_assert (!mutex.try_lock ());
	release_assert (winner);

	if (state.change (nano::election_state::confirmed))
	{
		winner_block = winner;

		node.active.recently_confirmed.put (qualified_root, winner->hash ());

		auto const status = current_status_impl ();
		debug_assert (status.winner);
		debug_assert (status.winner->hash () == winner->hash ());

		node.stats.inc (nano::stat::type::election, nano::stat::detail::confirm_once);
		node.logger.trace (nano::log::type::election, nano::log::detail::election_confirmed,
		nano::log::arg{ "id", id },
		nano::log::arg{ "qualified_root", qualified_root },
		nano::log::arg{ "status", status });

		node.logger.debug (nano::log::type::election, "Election confirmed with winner: {} (behavior: {}, state: {}, voters: {}, blocks: {}, duration: {}ms, confirmation requests: {})",
		winner->hash ().to_string (),
		to_string (behavior_m),
		to_string (state.state ()),
		status.voter_count,
		status.block_count,
		nano::log::milliseconds (status.duration),
		status.confirmation_request_count);

		node.confirming_set.add (winner->hash (), shared_from_this ());

		node.election_workers.post ([winner, confirmation_action_l = confirmation_action] () {
			if (confirmation_action_l)
			{
				confirmation_action_l (winner);
			}
		});

		return true; // Confirmed
	}
	else
	{
		node.stats.inc (nano::stat::type::election, nano::stat::detail::confirm_once_failed);
	}
	return false; // No confirmation occurred
}

bool nano::election::confirm_if_quorum ()
{
	debug_assert (!mutex.try_lock ());

	// Process confirmation if quorum was reached, winner never changes after this point
	if (auto winner = consensus.winner ())
	{
		// We might receive votes before the block
		if (auto block = blocks[winner.value ()])
		{
			// If winner of the election is one of the forks, force process that fork block into the ledger
			if (winner != current_block->hash ())
			{
				node.block_processor.force (block);
			}

			return confirm_once (block);
		}
	}
	// If no quorum reached yet, check if our candidate needs updating
	if (auto candidate = consensus.candidate ())
	{
		// We might receive votes before the block
		if (auto block = blocks[candidate.value ()])
		{
			// If the current candidate is one of the forks, force process that fork block into the ledger
			if (candidate != current_block->hash ())
			{
				node.block_processor.force (block);
			}
		}
	}
	return false; // No quorum
}

bool nano::election::try_confirm (nano::block_hash const & hash)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	if (!confirmed_impl ())
	{
		if (blocks.contains (hash))
		{
			return confirm_once (blocks.at (hash));
		}
	}
	return false; // Could not confirm
}

bool nano::election::force_confirm ()
{
	release_assert (node.network_params.network.is_dev_network ());
	nano::lock_guard<nano::mutex> lock{ mutex };
	return confirm_once (current_block);
}

nano::vote_code nano::election::vote (nano::account const & representative, nano::vote_timestamp timestamp, nano::block_hash const & hash, nano::vote_source source)
{
	// Low weight votes should be filtered out earlier
	debug_assert (node.network_params.network.is_dev_network () || node.ledger.weight (representative) <= node.minimum_principal_weight ());

	auto const weight = node.ledger.weight (representative);
	auto const quorum = node.online_reps.delta ();

	nano::unique_lock<nano::mutex> lock{ mutex };

	nano::consensus::vote const vote{
		.hash = hash,
		.representative = representative,
		.weight = weight,
		.timestamp = timestamp
	};

	bool updated = consensus.vote (vote, quorum);
	if (!updated)
	{
		// This vote did not change the election
		return vote_code::replay;
	}

	timestamps[representative] = std::chrono::steady_clock::now ();

	// Notify observers about representative activity
	if (source != vote_source::cache)
	{
		live_vote_action (representative);
	}

	node.stats.inc (nano::stat::type::election, nano::stat::detail::vote);
	node.stats.inc (nano::stat::type::election_vote, to_stat_detail (source));

	node.logger.trace (nano::log::type::election, nano::log::detail::vote_processed,
	nano::log::arg{ "id", id },
	nano::log::arg{ "qualified_root", qualified_root },
	nano::log::arg{ "representative", representative },
	nano::log::arg{ "hash", hash },
	nano::log::arg{ "final", nano::vote::is_final_timestamp (timestamp) },
	nano::log::arg{ "timestamp", timestamp },
	nano::log::arg{ "vote_source", source },
	nano::log::arg{ "weight", weight });

	// Check if we have quorum and final winner
	if (!confirmed_impl ())
	{
		confirm_if_quorum ();
	}

	return vote_code::vote;
}

bool nano::election::process (std::shared_ptr<nano::block> const & block, nano::block_status block_status)
{
	debug_assert (block_status == nano::block_status::progress || block_status == nano::block_status::fork);
	debug_assert (block->qualified_root () == qualified_root);

	nano::unique_lock<nano::mutex> lock{ mutex };

	// Do not insert new blocks if already confirmed
	if (confirmed_impl ())
	{
		return false;
	}

	// TODO: Bound number of blocks
	blocks[block->hash ()] = block;

	if (block_status == nano::block_status::progress)
	{
		current_block = block;
	}

	return true; // Processed
}

bool nano::election::transition_time (nano::confirmation_solicitor & solicitor)
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	bool done = false;

	switch (state.state ())
	{
		case nano::election_state::passive:
		{
			confirm_if_quorum ();

			// Change to active after enough time has passed
			if (base_latency () * passive_duration_factor < state.duration ())
			{
				state.change (nano::election_state::active);
			}
		}
		break;
		case nano::election_state::active:
		{
			confirm_if_quorum ();
			broadcast_vote ();
			broadcast_block (solicitor);
			request_confirmations (solicitor);
		}
		break;
		case nano::election_state::confirmed:
		{
			done = true; // Return true to indicate this election should be cleaned up
			broadcast_vote (); // Ensure vote is always broadcasted
			broadcast_block (solicitor); // Ensure election winner is broadcasted
			state.change (nano::election_state::expired_confirmed);
		}
		break;
		case nano::election_state::expired_unconfirmed:
		case nano::election_state::expired_confirmed:
			// Already completed elections should not be updated
			debug_assert (false);
			break;
		case nano::election_state::cancelled:
			return true; // Clean up cancelled elections immediately
	}

	// Check if election has expired
	if (!confirmed_impl () && time_to_live () < duration ())
	{
		bool changed = state.change (nano::election_state::expired_unconfirmed);
		release_assert (changed);

		node.logger.trace (nano::log::type::election, nano::log::detail::election_expired,
		nano::log::arg{ "id", id },
		nano::log::arg{ "qualified_root", qualified_root },
		nano::log::arg{ "status", current_status_impl () });

		done = true; // Return true to indicate this election should be cleaned up
	}

	if (done)
	{
		election_end = std::chrono::steady_clock::now ();
	}

	return done; // Return whether this election has finished
}

void nano::election::transition_active ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	state.change (nano::election_state::passive, nano::election_state::active);
}

void nano::election::cancel ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	state.change (nano::election_state::cancelled);
}

void nano::election::request_confirmations (nano::confirmation_solicitor & solicitor)
{
	debug_assert (!mutex.try_lock ());
	release_assert (current_block);

	if (confirm_req_time () < (std::chrono::steady_clock::now () - last_req))
	{
		if (solicitor.request (current_block, all_votes_impl ()) > 0)
		{
			last_req = std::chrono::steady_clock::now ();
			++confirmation_request_count;
		}
	}
}

bool nano::election::broadcast_block_predicate () const
{
	debug_assert (!mutex.try_lock ());
	release_assert (current_block);

	// Broadcast the block if enough time has passed since the last broadcast (or it's the first broadcast)
	if (last_broadcast_time + node.config.network_params.network.block_broadcast_interval < std::chrono::steady_clock::now ())
	{
		return true;
	}
	// Or the current election winner has changed
	if (current_block->hash () != last_broadcast_hash)
	{
		return true;
	}
	return false;
}

void nano::election::broadcast_block (nano::confirmation_solicitor & solicitor)
{
	debug_assert (!mutex.try_lock ());
	release_assert (current_block);

	if (broadcast_block_predicate ())
	{
		if (!solicitor.broadcast (current_block, all_votes_impl ()))
		{
			node.stats.inc (nano::stat::type::election, last_broadcast_hash.is_zero () ? nano::stat::detail::broadcast_block_initial : nano::stat::detail::broadcast_block_repeat);

			last_broadcast_hash = current_block->hash ();
			last_broadcast_time = std::chrono::steady_clock::now ();
		}
	}
}

bool nano::election::voting () const
{
	return node.config.enable_voting && node.wallets.reps ().voting > 0;
}

void nano::election::broadcast_vote_immediate ()
{
	if (voting ())
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		broadcast_vote_impl ();
	}
}

bool nano::election::broadcast_vote_predicate () const
{
	debug_assert (!mutex.try_lock ());
	release_assert (current_block);

	// Skip voting if there are no reps configured
	if (!voting ())
	{
		return false;
	}
	// Broadcast the vote if enough time has passed since the last broadcast (or it's the first broadcast)
	if (last_vote_time + node.config.network_params.network.vote_broadcast_interval < std::chrono::steady_clock::now ())
	{
		return true;
	}
	if (auto request = consensus.request (current_block->hash (), 0))
	{
		// Or the current election candidate has changed
		if (request->hash != last_vote.hash)
		{
			return true;
		}
		// Or we switched to a final vote
		if (request->is_final () && !last_vote.is_final ())
		{
			return true;
		}
	}
	return false;
}

void nano::election::broadcast_vote ()
{
	debug_assert (!mutex.try_lock ());
	release_assert (current_block);

	if (broadcast_vote_predicate ())
	{
		broadcast_vote_impl ();
	}
}

void nano::election::broadcast_vote_impl ()
{
	debug_assert (!mutex.try_lock ());
	release_assert (current_block);

	auto maybe_request = consensus.request (current_block->hash (), nano::milliseconds_since_epoch ());
	if (!maybe_request)
	{
		return; // No candidate to vote for
	}
	auto request = maybe_request.value ();

	node.stats.inc (nano::stat::type::election, nano::stat::detail::broadcast_vote);

	if (request.is_final ())
	{
		node.stats.inc (nano::stat::type::election_broadcast, nano::stat::detail::vote_final);
		node.logger.trace (nano::log::type::election, nano::log::detail::broadcast_vote,
		nano::log::arg{ "id", id },
		nano::log::arg{ "qualified_root", qualified_root },
		nano::log::arg{ "hash", request.hash },
		nano::log::arg{ "type", "final" });

		// TODO: Pass current round timestamp to vote generator (request->timestamp)
		node.final_generator.add (root, request.hash); // Broadcasts vote to the network
	}
	else
	{
		node.stats.inc (nano::stat::type::election_broadcast, nano::stat::detail::vote_normal);
		node.logger.trace (nano::log::type::election, nano::log::detail::broadcast_vote,
		nano::log::arg{ "id", id },
		nano::log::arg{ "qualified_root", qualified_root },
		nano::log::arg{ "hash", request.hash },
		nano::log::arg{ "type", "normal" });

		// TODO: Pass current round timestamp to vote generator (request->timestamp)
		node.generator.add (root, request.hash); // Broadcasts vote to the network
	}

	last_vote = request;
	last_vote_time = std::chrono::steady_clock::now ();
}

nano::election_status nano::election::current_status () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return current_status_impl ();
}

nano::election_status nano::election::current_status_impl () const
{
	debug_assert (!mutex.try_lock ());

	auto decide_status_type = [this] () -> nano::election_status_type {
		switch (state.state ())
		{
			case nano::election_state::passive:
			case nano::election_state::active:
				return nano::election_status_type::ongoing;
			case nano::election_state::confirmed:
			case nano::election_state::expired_confirmed:
				// TODO: Differentiate between confirmed directly by quorum and confirmed indirectly by confimation height
				return nano::election_status_type::active_confirmed_quorum;
			case nano::election_state::expired_unconfirmed:
			case nano::election_state::cancelled:
				return nano::election_status_type::stopped;
		}
		debug_assert (false);
		return {};
	};

	auto const tally = consensus.tally ();
	auto const final_tally = consensus.final_tally ();

	auto sum_tally_weight = [this] (auto const & tally) {
		nano::amount::underlying_type result{ 0 };
		for (auto const & [hash, amount] : tally)
		{
			result += amount.number ();
		}
		return result;
	};

	nano::election_status status{
		.type = decide_status_type (),
		.winner = winner_impl (),
		.tally = to_election_tally (tally),
		.final_tally = to_election_tally (final_tally),
		.tally_weight = sum_tally_weight (tally),
		.final_tally_weight = sum_tally_weight (final_tally),
		.time_started = std::chrono::duration_cast<std::chrono::milliseconds> (election_start.time_since_epoch ()),
		.time_ended = std::chrono::duration_cast<std::chrono::milliseconds> (election_end.time_since_epoch ()),
		.duration = std::chrono::duration_cast<std::chrono::milliseconds> (election_end - election_start),
		.confirmation_request_count = confirmation_request_count,
		.block_count = blocks.size (),
		.voter_count = consensus.size (),
		.votes = all_votes_impl (),
		.blocks = blocks,
	};

	return status;
}

nano::election_tally nano::election::tally () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return to_election_tally (consensus.tally ());
}

nano::election_tally nano::election::final_tally () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return to_election_tally (consensus.final_tally ());
}

std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> nano::election::all_blocks () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return blocks;
}

std::unordered_map<nano::account, nano::vote_info> nano::election::all_votes () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return all_votes_impl ();
}

std::unordered_map<nano::account, nano::vote_info> nano::election::all_votes_impl () const
{
	debug_assert (!mutex.try_lock ());
	auto votes = consensus.all_votes ();
	std::unordered_map<nano::account, nano::vote_info> result;
	for (auto const & vote : votes)
	{
		nano::vote_info info{
			.hash = vote.hash,
			.timestamp = vote.timestamp,
			.time = find_or_empty (timestamps, vote.representative).value_or (std::chrono::steady_clock::time_point{})
		};
		result.emplace (vote.representative, info);
	}
	return result;
}

std::shared_ptr<nano::block> nano::election::find_block (nano::block_hash const & hash) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	if (auto existing = blocks.find (hash); existing != blocks.end ())
	{
		return existing->second;
	}
	return nullptr;
}

std::optional<nano::vote_info> nano::election::find_vote (nano::account const & representative) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	if (auto vote = consensus.find_vote (representative))
	{
		nano::vote_info info{
			.hash = vote->hash,
			.timestamp = vote->timestamp,
			.time = find_or_empty (timestamps, representative).value_or (std::chrono::steady_clock::time_point{})
		};
		return info;
	}
	return std::nullopt;
}

// TODO: Rework and use consensus tally
std::vector<nano::vote_with_weight_info> nano::election::votes_with_weight () const
{
	std::multimap<nano::uint128_t, nano::vote_with_weight_info, std::greater<nano::uint128_t>> sorted_votes;
	std::vector<nano::vote_with_weight_info> result;
	auto votes_l = all_votes ();
	for (auto const & vote_l : votes_l)
	{
		if (vote_l.first != nullptr)
		{
			auto const amount = node.ledger.cache.rep_weights.representation_get (vote_l.first);
			nano::vote_with_weight_info info{
				.representative = vote_l.first,
				.time = vote_l.second.time,
				.timestamp = vote_l.second.timestamp,
				.hash = vote_l.second.hash,
				.weight = amount
			};
			sorted_votes.emplace (amount, info);
		}
	}
	result.reserve (sorted_votes.size ());
	std::transform (sorted_votes.begin (), sorted_votes.end (), std::back_inserter (result), [] (auto const & entry) { return entry.second; });
	return result;
}

bool nano::election::confirmed () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return confirmed_impl ();
}

bool nano::election::confirmed_impl () const
{
	debug_assert (!mutex.try_lock ());
	return state.state () == nano::election_state::confirmed || state.state () == nano::election_state::expired_confirmed;
}

bool nano::election::failed () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return state.state () == nano::election_state::expired_unconfirmed;
}

bool nano::election::finished () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	switch (state.state ())
	{
		case nano::election_state::expired_unconfirmed:
		case nano::election_state::expired_confirmed:
		case nano::election_state::cancelled:
			return true;
		default:
			return false;
	}
}

// Might be nullptr if the block has not yet arrived
std::shared_ptr<nano::block> nano::election::winner () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return winner_impl ();
}

// Might be nullptr if the block has not yet arrived
std::shared_ptr<nano::block> nano::election::winner_impl () const
{
	debug_assert (!mutex.try_lock ());
	debug_assert (!confirmed_impl () || winner_block); // If confirmed, the winner block must be present
	return winner_block;
}

// Might be nullptr if the block has not yet arrived
std::shared_ptr<nano::block> nano::election::candidate () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	if (auto request = consensus.request (current_block->hash (), 0))
	{
		return find_or_empty (blocks, request->hash).value_or (nullptr);
	}
	return nullptr;
}

nano::block_hash nano::election::leader () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return consensus.leader ().value_or (0);
}

std::chrono::steady_clock::duration nano::election::duration () const
{
	return std::chrono::steady_clock::now () - election_start;
}

nano::election_behavior nano::election::behavior () const
{
	return behavior_m;
}

nano::election_state nano::election::current_state () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return state.state ();
}

bool nano::election::contains (nano::block_hash const & hash) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return blocks.contains (hash);
}

nano::election_tally nano::election::to_election_tally (std::map<nano::block_hash, nano::amount> const & tally)
{
	nano::election_tally result;
	for (auto const & [hash, amount] : tally)
	{
		result.emplace (amount.number (), hash);
	}
	return result;
}

// TODO: Remove the need for .to_string () calls
void nano::election::operator() (nano::object_stream & obs) const
{
	obs.write ("id", id);
	obs.write ("qualified_root", qualified_root.to_string ());
	obs.write ("behavior", behavior_m);
	obs.write ("height", height);
	obs.write ("status", current_status ());
}

void nano::election_status::operator() (nano::object_stream & obs) const
{
	obs.write ("winner", winner->hash ().to_string ());
	obs.write ("tally_amount", tally_weight.to_string_dec ());
	obs.write ("final_tally_amount", final_tally_weight.to_string_dec ());
	obs.write ("confirmation_request_count", confirmation_request_count);
	obs.write ("block_count", block_count);
	obs.write ("voter_count", voter_count);
	obs.write ("type", type);

	obs.write_range ("votes", votes, [] (auto const & entry, nano::object_stream & obs) {
		auto const & [account, info] = entry;
		obs.write ("account", account.to_account ());
		obs.write ("hash", info.hash.to_string ());
		obs.write ("final", nano::vote::is_final_timestamp (info.timestamp));
		obs.write ("timestamp", info.timestamp);
		// obs.write ("time", info.time.time_since_epoch ().count ());
	});

	obs.write_range ("blocks", blocks, [] (auto const & entry) {
		auto const & [hash, block] = entry;
		return block;
	});

	obs.write_range ("tally", tally, [] (auto const & entry, nano::object_stream & obs) {
		auto const & [amount, hash] = entry;
		obs.write ("hash", hash.to_string ());
		obs.write ("amount", amount);
	});
}

/*
 * election_state_guard
 */

bool nano::election_state_guard::change (nano::election_state desired)
{
	return change (state (), desired);
}

bool nano::election_state_guard::change (nano::election_state expected, nano::election_state desired)
{
	if (valid_change (expected, desired))
	{
		if (state () == expected)
		{
			current = { desired };
			return true; // Changed
		}
	}
	return false; // Not changed
}

bool nano::election_state_guard::valid_change (nano::election_state expected, nano::election_state desired)
{
	switch (expected)
	{
		case nano::election_state::passive:
			switch (desired)
			{
				case nano::election_state::active:
				case nano::election_state::confirmed:
				case nano::election_state::expired_unconfirmed:
				case nano::election_state::cancelled:
					return true; // Valid
				default:
					break;
			}
			break;
		case nano::election_state::active:
			switch (desired)
			{
				case nano::election_state::confirmed:
				case nano::election_state::expired_unconfirmed:
				case nano::election_state::cancelled:
					return true; // Valid
				default:
					break;
			}
			break;
		case nano::election_state::confirmed:
			switch (desired)
			{
				case nano::election_state::expired_confirmed:
					return true; // Valid
				default:
					break;
			}
			break;
		case nano::election_state::expired_unconfirmed:
		case nano::election_state::expired_confirmed:
		case nano::election_state::cancelled:
			// No transitions are valid from these states
			break;
	}
	return false; // Invalid
}

/*
 *
 */

std::string_view nano::to_string (nano::election_behavior behavior)
{
	return nano::enum_util::name (behavior);
}

nano::stat::detail nano::to_stat_detail (nano::election_behavior behavior)
{
	return nano::enum_util::cast<nano::stat::detail> (behavior);
}

std::string_view nano::to_string (nano::election_state state)
{
	return nano::enum_util::name (state);
}

nano::stat::detail nano::to_stat_detail (nano::election_state state)
{
	return nano::enum_util::cast<nano::stat::detail> (state);
}
