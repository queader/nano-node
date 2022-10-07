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
		explicit account_sets (nano::stat &);

		void prioritize (nano::account const & account, float priority);
		void block (nano::account const & account, nano::block_hash const & dependency);
		void unblock (nano::account const & account, nano::block_hash const & hash);
		void force_unblock (nano::account const & account);
		std::optional<nano::account> next ();

	public:
		bool blocked (nano::account const & account) const;

	public: // Container info
		std::unique_ptr<nano::container_info_component> collect_container_info (std::string const & name);

	private: // Dependencies
		nano::stat & stats;

	private:
		std::optional<nano::account> random ();

		struct backoff_entry
		{
			float backoff{ 0 };
			nano::millis_t last_request{ 0 };
		};

		// A forwarded account is an account that has recently had a new block inserted or has been a destination reference and therefore is a more likely candidate for furthur block retrieval
		std::unordered_set<nano::account> forwarding;
		// A blocked account is an account that has failed to insert a block because the source block is gapped.
		// An account is unblocked once it has a block successfully inserted.
		std::map<nano::account, nano::block_hash> blocking;
		// Tracks the number of requests for additional blocks without a block being successfully returned
		// Each time a block is inserted to an account, this number is reset.
		std::map<nano::account, backoff_entry> backoff;

		std::default_random_engine rng;

		/**
			Convert a vector of attempt counts to a probability vector suitable for std::discrete_distribution
			This implementation applies 1/2^i for each element, effectivly an exponential backoff
		*/
		std::vector<double> probability_transform (std::vector<decltype (backoff)::mapped_type> const & attempts) const;

	private:
		/**
		 * Number of backoff accounts to sample when selecting random account
		 */
		static std::size_t constexpr backoff_exclusion = 32;

		/**
		 * Minimum time between subsequent request for the same account
		 */
		static nano::millis_t constexpr cooldown = 1000;

	public:
		using backoff_info_t = std::tuple<decltype (forwarding), decltype (blocking), decltype (backoff)>; // <forwarding, blocking, backoff>

		backoff_info_t backoff_info () const;
	};

	account_sets::backoff_info_t backoff_info () const;

	using id_t = uint64_t;

	struct async_tag
	{
		id_t id{ 0 };
		nano::hash_or_account start{ 0 };
		nano::millis_t time{ 0 };
	};

public: // Events
	nano::observer_set<async_tag const &, std::shared_ptr<nano::transport::channel> &> on_request;
	nano::observer_set<async_tag const &> on_reply;
	nano::observer_set<async_tag const &> on_timeout;

private:
	/**
	 * Seed backoffs with accounts from the ledger
	 */
	void seed ();
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
	std::optional<nano::account> pick_account ();
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
	static std::size_t constexpr requests_max = 1024 * 4;

private:
	std::atomic<int> responses{ 0 };
	std::atomic<int> requests_total{ 0 };
	std::atomic<float> weights{ 0 };
	std::atomic<int> forwarded{ 0 };
	std::atomic<int> block_total{ 0 };
};
}
