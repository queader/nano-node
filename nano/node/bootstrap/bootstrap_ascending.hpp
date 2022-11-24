#pragma once

#include <nano/lib/observer_set.hpp>
#include <nano/lib/timer.hpp>
#include <nano/node/bootstrap/bootstrap_attempt.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <random>
#include <thread>

namespace mi = boost::multi_index;

namespace nano
{
class block_processor;
class ledger;
class network;

namespace transport
{
	class channel;
}

class bootstrap_ascending
{
public:
	bootstrap_ascending (nano::node &, nano::store &, nano::block_processor &, nano::ledger &, nano::network &, nano::stat &);
	~bootstrap_ascending ();

	void start ();
	void stop ();

	/**
	 * If an account is not blocked, increase its priority.
	 * Priority is increased whether it is in the normal priority set, or it is currently blocked and in the blocked set.
	 * Current implementation increases priority by 1.0f each increment
	 */
	void priority_up (nano::account const & account_a);
	/**
	 * Decreases account priority
	 * Current implementation divides priority by 2.0f and saturates down to 1.0f.
	 */
	void priority_down (nano::account const & account_a);

	/**
	 * Inspects a block that has been processed by the block processor
	 * - Marks an account as blocked if the result code is gap source as there is no reason request additional blocks for this account until the dependency is resolved
	 * - Marks an account as forwarded if it has been recently referenced by a block that has been inserted.
	 */
	void inspect (nano::transaction const & tx, nano::process_return const & result, nano::block const & block);

	/**
	 * Process `asc_pull_ack` message coming from network
	 */
	void process (nano::asc_pull_ack const & message);

public: // Container info
	std::unique_ptr<nano::container_info_component> collect_container_info (std::string const & name);
	size_t blocked_size () const;
	size_t priority_size () const;

private: // Dependencies
	nano::node & node;
	nano::store & store;
	nano::block_processor & block_processor;
	nano::ledger & ledger;
	nano::network & network;
	nano::stat & stats;

public:
	class async_tag;

	/** This class tracks accounts various account sets which are shared among the multiple bootstrap threads */
	class account_sets
	{
		class iterator_t
		{
			enum class table_t
			{
				account,
				pending
			};
		public:
			iterator_t (nano::store & store);
			nano::account operator* () const;
			void next (nano::transaction & tx);
		private:
			nano::store & store;
			nano::account current{ 0 };
			table_t table{ table_t::account };
		};
	public:
		explicit account_sets (nano::stat &, nano::store & store);

		void priority_up (nano::account const & account);
		void priority_down (nano::account const & account);
		void block (nano::account const & account, nano::block_hash const & dependency);
		void unblock (nano::account const & account, std::optional<nano::block_hash> const & hash);
		/**
		 * Selects a random account from either:
		 * 1) The priority set in memory
		 * 2) The accounts in the ledger
		 * 3) Pending entries in the ledger
		 * Creates consideration set of "consideration_count" items and returns on randomly weighted by priority
		 * Half are considered from the "priorities" container, half are considered from the ledger.
		 */
		nano::account random ();

	public:
		bool blocked (nano::account const & account) const;
		size_t priority_size () const;
		size_t blocked_size () const;
		float priority (nano::account const & account) const;
		void dump () const;

	public: // Container info
		std::unique_ptr<nano::container_info_component> collect_container_info (std::string const & name);

	private: // Dependencies
		nano::stat & stats;
		nano::store & store;
		iterator_t iter;

	private:
		static size_t constexpr consideration_count = 2;

		// A blocked account is an account that has failed to insert a block because the source block is gapped.
		// An account is unblocked once it has a block successfully inserted.
		// Maps "blocked account" -> ["blocked hash", "Priority count"]
		std::map<nano::account, std::pair<nano::block_hash, float>> blocking;
		class priority_t
		{
		public:
			nano::account account;
			float priority;
		};
		class tag_hash
		{
		};
		class tag_priority
		{
		};
		// Tracks the ongoing account priorities
		// This only stores account priorities > 1.0f.
		// Accounts in the ledger but not in this list are assumed priority 1.0f.
		// Blocked accounts are assumed priority 0.0f
		boost::multi_index_container<priority_t,
		boost::multi_index::indexed_by<
		boost::multi_index::ordered_unique<boost::multi_index::tag<tag_hash>,
		boost::multi_index::member<priority_t, nano::account, &priority_t::account>>,
		boost::multi_index::ordered_non_unique<boost::multi_index::tag<tag_priority>,
		boost::multi_index::member<priority_t, float, &priority_t::priority>>>>
		priorities;
		static size_t const priorities_max = 64 * 1024;

		std::default_random_engine rng;
		/**
		 * Minimum time between subsequent request for the same account
		 */
		static nano::millis_t constexpr cooldown = 1000;

	public:
		using info_t = std::tuple<decltype (blocking), decltype (priorities)>; // <blocking, priorities>

		info_t info () const;
	};

	account_sets::info_t info () const;

	using id_t = uint64_t;

	struct async_tag
	{
		id_t id{ 0 };
		nano::hash_or_account start{ 0 };
		nano::millis_t time{ 0 };
		nano::account account{ 0 };
	};

public: // Events
	nano::observer_set<async_tag const &, std::shared_ptr<nano::transport::channel> &> on_request;
	nano::observer_set<async_tag const &> on_reply;
	nano::observer_set<async_tag const &> on_timeout;

private:
	void run ();
	void run_timeouts ();

	/**
	 * Limits the number of parallel requests we make
	 */
	void wait_available_request () const;
	/**
	 * Throttle receiving new blocks, not to overwhelm blockprocessor
	 */
	void wait_blockprocessor () const;
	/**
	 * Waits for channel with free capacity for bootstrap messages
	 */
	std::shared_ptr<nano::transport::channel> wait_available_channel ();
	std::shared_ptr<nano::transport::channel> available_channel ();
	/**
	 * Waits until a suitable account outside of cooldown period is available
	 */
	nano::account wait_available_account ();

	id_t generate_id () const;
	bool request_one ();
	bool request (nano::account &, std::shared_ptr<nano::transport::channel> &);
	void send (std::shared_ptr<nano::transport::channel>, async_tag tag);
	void track (async_tag const & tag);

	/**
	 * Process blocks response
	 */
	void process (nano::asc_pull_ack::blocks_payload const & response, async_tag const & tag);
	/**
	 * Process account info response
	 */
	void process (nano::asc_pull_ack::account_info_payload const & response, async_tag const & tag);
	/**
	 * Handle empty payload response
	 */
	void process (nano::empty_payload const & response, async_tag const & tag);

	/**
	 * Verify that blocks response is valid
	 */
	bool verify (nano::asc_pull_ack::blocks_payload const & response, async_tag const & tag) const;

private:
	void debug_log (const std::string &) const;

private:
	account_sets accounts;

	// clang-format off
	class tag_sequenced {};
	class tag_id {};

	using ordered_tags = boost::multi_index_container<async_tag,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_id>,
			mi::member<async_tag, id_t, &async_tag::id>>>>;
	// clang-format on
	ordered_tags tags;

	std::atomic<bool> stopped{ false };
	mutable nano::mutex mutex;
	mutable nano::condition_variable condition;
	std::vector<std::thread> threads;
	std::thread timeout_thread;

private:
	//		static std::size_t constexpr requests_max = 16;
	//	static std::size_t constexpr requests_max = 1024;
	static std::size_t constexpr requests_max = 128;
};
}
