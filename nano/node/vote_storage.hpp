#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/processing_queue.hpp>
#include <nano/store/component.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mi = boost::multi_index;

namespace nano
{
class network;
class stats;
class ledger;
class vote;
namespace transport
{
	class channel;
}

class vote_storage final
{
public:
	using vote_list_t = std::vector<std::shared_ptr<nano::vote>>;

public:
	vote_storage (nano::node &, nano::store::component & vote_store, nano::network &, nano::ledger &, nano::stats &);
	~vote_storage ();

	void start ();
	void stop ();

	std::unique_ptr<container_info_component> collect_container_info (std::string const & name) const;

public:
	/**
	 * Store vote in database
	 */
	void vote (std::shared_ptr<nano::vote>);

	/**
	 * Trigger vote broadcast for hash
	 */
	void trigger (nano::block_hash const &, std::shared_ptr<nano::transport::channel> const &);

public: // Dependencies
	nano::node & node;
	nano::store::component & vote_store;
	nano::network & network;
	nano::ledger & ledger;
	nano::stats & stats;

private:
	using reply_entry_t = std::pair<nano::block_hash, std::shared_ptr<nano::transport::channel>>;

	nano::processing_queue<std::shared_ptr<nano::vote>> store_queue;
	nano::processing_queue<reply_entry_t> reply_queue;

private:
	class recently_broadcasted
	{
	public:
		bool check (nano::block_hash const &);
		bool check_and_insert (nano::block_hash const &);
		bool check (nano::block_hash const &, std::shared_ptr<nano::transport::channel> const &);
		bool check_and_insert (nano::block_hash const &, std::shared_ptr<nano::transport::channel> const &);

		std::unique_ptr<container_info_component> collect_container_info (std::string const & name) const;

		static std::chrono::seconds constexpr rebroadcast_interval{ 10 };
		static std::chrono::seconds constexpr cleanup_interval{ rebroadcast_interval / 2 };

	private:
		void cleanup ();

		using recently_broadcasted_entries_t = std::unordered_map<nano::block_hash, std::chrono::steady_clock::time_point>;
		std::unordered_map<std::shared_ptr<nano::transport::channel>, recently_broadcasted_entries_t> recently_broadcasted_map;

		std::unordered_map<nano::block_hash, std::chrono::steady_clock::time_point> recently_broadcasted_hashes;

		std::chrono::steady_clock::time_point last_cleanup{ std::chrono::steady_clock::now () };
		mutable nano::mutex mutex;
	};

	vote_storage::recently_broadcasted recently_broadcasted;

private:
	struct request_entry
	{
		nano::block_hash hash;
		size_t count;
		std::chrono::steady_clock::time_point time{ std::chrono::steady_clock::now () };
	};

	// clang-format off
	class tag_sequenced {};
	class tag_hash {};
	class tag_count {};

	using ordered_requests = boost::multi_index_container<request_entry,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<request_entry, nano::block_hash, &request_entry::hash>>,
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::ordered_non_unique<mi::tag<tag_count>,
			mi::member<request_entry, size_t, &request_entry::count>, std::greater<>> // DESC
	>>;
	// clang-format on

	static size_t constexpr max_requests = 1024 * 512;
	ordered_requests requests;

private:
	void run ();
	std::unordered_set<nano::block_hash> run_broadcasts (ordered_requests);
	void cleanup ();
	void wait_peers ();

	void process_batch (decltype (store_queue)::batch_t &);
	void process_batch (decltype (reply_queue)::batch_t &);

	uint128_t weight (vote_list_t const &) const;
	uint128_t weight_final (vote_list_t const &) const;
	vote_list_t filter (vote_list_t const &) const;

	void reply (vote_list_t const &, nano::block_hash const &, std::shared_ptr<nano::transport::channel> const &);
	void broadcast (vote_list_t const &, nano::block_hash const &);

	vote_list_t query_hash (nano::store::transaction const & vote_transaction, nano::block_hash const &, bool final_only = false);
	/** @returns <votes, votes frontier> */
	std::pair<vote_list_t, nano::block_hash> query_frontier (nano::store::transaction const & ledger_transaction, nano::store::transaction const & vote_transaction, nano::block_hash const &);

public:
	vote_list_t query_hash (nano::block_hash);

private:
	std::atomic<bool> stopped{ false };
	mutable nano::mutex mutex;
	nano::condition_variable condition;
	std::thread thread;

private:
	// TODO: Use nodeconfig
	uint128_t const vote_weight_threshold{ 65600 * nano::Gxrb_ratio }; // Quorum
	uint128_t const vote_final_weight_threshold{ 65600 * nano::Gxrb_ratio }; // Quorum
	uint128_t const rep_weight_threshold{ 100 * nano::Gxrb_ratio };
	bool const trigger_pr_only{ true };
	bool const store_final_only{ false };
	bool const ignore_255_votes{ true };
	float const max_busy_ratio{ 0.5f };
	std::chrono::seconds const request_age_cutoff{ 30 };
	bool const reply_priority_accounts_only{ true };

	static bool constexpr enable_broadcast = false;
	static bool constexpr enable_replies = true;
	static bool constexpr enable_pr_broadcast = true;
	static bool constexpr enable_random_broadcast = false;
	static bool constexpr enable_query_frontier = false;
};
}