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

class bootstrap_ascending final
{
public:
	bootstrap_ascending (nano::node &, nano::store &, nano::block_processor &, nano::ledger &, nano::network &, nano::stat &);
	~bootstrap_ascending ();

	void start ();
	void stop ();

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
	boost::property_tree::ptree collect_info () const;

	std::size_t blocked_size () const;
	std::size_t priority_size () const;

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
	public:
		explicit account_sets (nano::stat &, nano::store & store);

		void advance (nano::account const & account);
		void suppress (nano::account const & account);
		void block (nano::account const & account, nano::block_hash const & dependency);
		void send (nano::account const & account, nano::block_hash const & source);

		nano::account next ();

	public: // Stats
		enum class stat_type
		{
			request,
			timeout,
			reply,
			old,
			blocks,
			progress,
			gap_source,
			gap_previous,
			corrupt,
			nothing_new,
			wtf,
		};

		void stat (nano::account const & account, stat_type, std::size_t increase = 1);

	private:
		/**
		 * If an account is not blocked, increase its priority.
		 * Priority is increased whether it is in the normal priority set, or it is currently blocked and in the blocked set.
		 * Current implementation increases priority by 1.0f each increment
		 */
		void priority_up (nano::account const & account, float priority_increase = 0.4f);
		/**
		 * Decreases account priority
		 * Current implementation divides priority by 2.0f and saturates down to 1.0f.
		 */
		void priority_down (nano::account const & account);
		bool unblock (nano::account const & account, std::optional<nano::block_hash> const & source = std::nullopt);
		void timestamp (nano::account const & account, nano::millis_t time);

		/**
		 * Selects a random account from either:
		 * 1) The priority set in memory
		 * 2) The accounts in the ledger
		 * 3) Pending entries in the ledger
		 * Creates consideration set of "consideration_count" items and returns on randomly weighted by priority
		 * Half are considered from the "priorities" container, half are considered from the ledger.
		 */

		nano::account next_prioritization ();
		nano::account next_database ();

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

	private:
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

	private:
		mutable std::recursive_mutex mutex;

		iterator_t iter;

		// A blocked account is an account that has failed to insert a block because the source block is gapped.
		// An account is unblocked once it has a block successfully inserted.
		// Maps "blocked account" -> ["blocked hash", "Priority count"]
		std::map<nano::account, nano::block_hash> blocking;

	public:
		struct priority_entry
		{
			uint64_t id{ 0 };

			nano::account account{ 0 };
			float priority{ 0 };
			nano::millis_t last_request{ 0 };

			struct stat
			{
				std::size_t request{ 0 };
				std::size_t timeout{ 0 };
				std::size_t reply{ 0 };
				std::size_t old{ 0 };
				std::size_t blocks{ 0 };
				std::size_t progress{ 0 };
				std::size_t gap_source{ 0 };
				std::size_t gap_previous{ 0 };
				std::size_t corrupt{ 0 };
				std::size_t nothing_new{ 0 };
				std::size_t wtf{ 0 };
			};

			stat stats{};

			boost::property_tree::ptree collect_info () const;
		};

	private:
		// clang-format off
		class tag_sequenced {};
		class tag_account {};
		class tag_priority {};
		class tag_id {};

		using ordered_priorities = boost::multi_index_container<priority_entry,
		mi::indexed_by<
			mi::sequenced<mi::tag<tag_sequenced>>,
			mi::ordered_unique<boost::multi_index::tag<tag_account>,
				mi::member<priority_entry, nano::account, &priority_entry::account>>,
			mi::ordered_non_unique<boost::multi_index::tag<tag_priority>,
				mi::member<priority_entry, float, &priority_entry::priority>>,
			mi::ordered_non_unique<boost::multi_index::tag<tag_id>,
				mi::member<priority_entry, uint64_t, &priority_entry::id>>
		>>;
		// clang-format on

		// Tracks the ongoing account priorities
		// This only stores account priorities > 1.0f.
		// Accounts in the ledger but not in this list are assumed priority 1.0f.
		// Blocked accounts are assumed priority 0.0f
		ordered_priorities priorities;

		std::default_random_engine rng;

	private: // Constants
		static std::size_t constexpr consideration_count = 4;

		/** Accounts below this priority value will be evicted */
		static float constexpr priority_cutoff = 1.0f;

		static std::size_t constexpr max_priorities_size = 64 * 1024;

		/** Minimum time between subsequent request for the same account */
		static nano::millis_t constexpr cooldown = 1000;

	public:
		//		using info_t = std::tuple<std::vector<nano::account>, std::vector<priority_entry>>; // <blocking, priorities>
		using info_t = std::tuple<decltype (blocking), std::vector<priority_entry>>; // <blocking, priorities>

		info_t info () const;

		boost::property_tree::ptree collect_info () const;
	};

	account_sets::info_t info () const;

	using id_t = uint64_t;

	struct async_tag
	{
		id_t id{ 0 };
		nano::hash_or_account start{ 0 };
		nano::millis_t time{ 0 };
		nano::account account{ 0 };

		bool pulling_by_account () const
		{
			return account == start;
		}

		boost::property_tree::ptree collect_info () const;
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
	nano::account wait_available_account (nano::unique_lock<nano::mutex> &);

	static id_t generate_id ();

	bool request_one ();
	bool request (nano::unique_lock<nano::mutex> &, nano::account &, std::shared_ptr<nano::transport::channel> &);
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
	class tag_account {};

	using ordered_tags = boost::multi_index_container<async_tag,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_id>,
			mi::member<async_tag, id_t, &async_tag::id>>,
		mi::hashed_non_unique<mi::tag<tag_account>,
			mi::member<async_tag, nano::account , &async_tag::account>>
	>>;
	// clang-format on
	ordered_tags tags;

	std::atomic<bool> stopped{ false };
	mutable nano::mutex mutex;
	mutable nano::condition_variable condition;
	std::vector<std::thread> threads;
	std::thread timeout_thread;

private:
	//	static std::size_t constexpr requests_max = 16;
	//	static std::size_t constexpr requests_max = 1024;
	//	static std::size_t constexpr requests_max = 2;
	static std::size_t constexpr requests_max = 4096;
};
}
