#pragma once

#include <nano/consensus/consensus.hpp>
#include <nano/lib/id_dispenser.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/node/election_status.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/vote_with_weight_info.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/vote.hpp>

#include <atomic>
#include <chrono>
#include <memory>

namespace nano
{
enum class election_state
{
	passive, // only listening for incoming votes
	active, // actively request confirmations
	confirmed, // confirmed but still listening for votes
	expired_confirmed,
	expired_unconfirmed,
	cancelled,
};

std::string_view to_string (election_state);
nano::stat::detail to_stat_detail (election_state);

class election_state_guard final
{
public:
	explicit election_state_guard (nano::election_state initial) :
		current{ initial } {};

	// Prevent reassignment
	election_state_guard & operator= (election_state_guard) = delete;

	bool change (nano::election_state desired);
	bool change (nano::election_state expected, nano::election_state desired);
	static bool valid_change (nano::election_state expected, nano::election_state desired);

	nano::election_state state () const
	{
		return current.state;
	}
	std::chrono::steady_clock::time_point timestamp () const
	{
		return current.timestamp;
	}
	std::chrono::steady_clock::duration duration () const
	{
		return std::chrono::steady_clock::now () - timestamp ();
	}

private:
	struct state_wrapper
	{
		nano::election_state state;
		std::chrono::steady_clock::time_point timestamp{ std::chrono::steady_clock::now () };
	};
	state_wrapper current;
};

class election final : public std::enable_shared_from_this<election>
{
	nano::id_t const id{ nano::next_id () }; // Track individual objects when tracing

public: // Status
	std::atomic<unsigned> confirmation_request_count{ 0 };
	std::chrono::steady_clock::time_point const election_start{ std::chrono::steady_clock::now () };
	std::chrono::steady_clock::time_point election_end;

public: // Interface
	election (nano::node &, std::shared_ptr<nano::block> const & block, std::function<void (std::shared_ptr<nano::block> const &)> const & confirmation_action, std::function<void (nano::account const &)> const & vote_action, nano::election_behavior behavior);

	/// Process vote. If the election reaches consensus, it will be confirmed
	nano::vote_code vote (nano::account const & representative, vote_timestamp timestamp, nano::block_hash const & hash, nano::vote_source source);

	/// Process ledger updates. Keeps track of which fork is present in the ledger
	bool process (std::shared_ptr<nano::block> const & block, nano::block_status block_status);

	bool transition_time (nano::confirmation_solicitor &);
	void transition_active ();
	void cancel ();
	bool try_confirm (nano::block_hash const & hash);
	void broadcast_vote_immediate ();

	bool confirmed () const;
	bool failed () const;
	bool finished () const;
	std::chrono::steady_clock::duration duration () const;
	/// Block that won the election with final vote quorum, nullptr if not yet confirmed
	std::shared_ptr<nano::block> winner () const;
	/// Block that we are currently voting for, nullptr if no suitable candidate exists
	std::shared_ptr<nano::block> candidate () const;
	/// Block with the highest tally
	nano::block_hash leader () const;

	nano::election_behavior behavior () const;
	nano::election_status current_status () const;
	nano::election_state current_state () const;

	nano::election_tally tally () const;
	nano::election_tally final_tally () const;

	std::unordered_map<nano::account, nano::vote_info> all_votes () const;
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> all_blocks () const;
	std::shared_ptr<nano::block> find_block (nano::block_hash const & hash) const;
	std::optional<nano::vote_info> find_vote (nano::account const & representative) const;

	std::vector<nano::vote_with_weight_info> votes_with_weight () const;

public: // Events
	std::function<void (std::shared_ptr<nano::block> const &)> confirmation_action{ [] (auto const &) { /* ingore */ } };
	std::function<void (nano::account const &)> live_vote_action{ [] (auto const &) { /* ignore */ } };

private: // Dependencies
	nano::node & node;

public: // Information
	uint64_t const height;
	nano::root const root;
	nano::qualified_root const qualified_root;

	bool contains (nano::block_hash const &) const;

private:
	bool has_quorum_impl () const;
	bool confirmed_impl () const;
	std::shared_ptr<nano::block> winner_impl () const;
	nano::election_status current_status_impl () const;
	std::unordered_map<nano::account, nano::vote_info> all_votes_impl () const;

	bool confirm_once (std::shared_ptr<nano::block> const & winner);
	bool confirm_if_quorum ();

	void request_confirmations (nano::confirmation_solicitor &);

	bool broadcast_block_predicate () const;
	void broadcast_block (nano::confirmation_solicitor &);

	bool broadcast_vote_predicate () const;
	void broadcast_vote ();
	void broadcast_vote_impl ();

	bool voting () const;

	std::chrono::milliseconds time_to_live () const;
	/// Minimum time delay between subsequent votes when processing non-final votes
	std::chrono::seconds cooldown_time (nano::uint128_t weight) const;
	/// Time delay between broadcasting confirmation requests
	std::chrono::milliseconds confirm_req_time () const;

	static nano::election_tally to_election_tally (std::map<nano::block_hash, nano::amount> const & tally);

private:
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> blocks;
	std::shared_ptr<nano::block> current_block; // Block that is currently present in the ledger
	std::shared_ptr<nano::block> winner_block; // Block that won the election with final vote quorum or indirectly via dependent election
	std::unordered_map<nano::account, std::chrono::steady_clock::time_point> timestamps;

	nano::election_behavior const behavior_m;

	election_state_guard state{ election_state::passive };

	nano::consensus::election consensus;

	mutable nano::mutex mutex;

private:
	// Minimum time between broadcasts of the current winner of an election, as a backup to requesting confirmations
	std::chrono::milliseconds base_latency () const;

	std::chrono::steady_clock::time_point last_req{};
	// These are modified while not holding the mutex from transition_time only
	std::chrono::steady_clock::time_point last_broadcast_time{};
	nano::block_hash last_broadcast_hash{};
	/** The last time vote for this election was generated */
	std::chrono::steady_clock::time_point last_vote_time{};
	nano::consensus::vote_request last_vote{};

public: // Logging
	void operator() (nano::object_stream &) const;

private: // Constants
	// Max votes also limits the number of blocks in an election (1 vote per block)
	static size_t constexpr max_votes{ 1000 };

	static unsigned constexpr passive_duration_factor = 5;
	static unsigned constexpr active_request_count_min = 2;

	friend class active_elections;
	friend class confirmation_solicitor;

public: // Only used in tests
	bool force_confirm ();

	friend class confirmation_solicitor_different_hash_Test;
	friend class confirmation_solicitor_bypass_max_requests_cap_Test;
	friend class votes_add_existing_Test;
	friend class votes_add_old_Test;
};
}
