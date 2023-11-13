#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/processing_queue.hpp>
#include <nano/node/blockprocessor.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <memory>
#include <thread>
#include <unordered_set>

namespace mi = boost::multi_index;

namespace nano
{
class node;
class network;
}

namespace nano
{
/**
 * Broadcasts blocks to the network
 * Tracks local blocks for more aggressive propagation
 */
class block_broadcast
{
	enum class broadcast_strategy
	{
		normal,
		aggressive,
	};

public:
	block_broadcast (nano::node &, nano::block_processor &, nano::network &, nano::stats &, bool enabled = false);
	~block_broadcast ();

	void start ();
	void stop ();

	std::unique_ptr<container_info_component> collect_container_info (std::string const & name) const;

private: // Dependencies
	nano::node & node;
	nano::block_processor & block_processor;
	nano::network & network;
	nano::stats & stats;

private:
	struct local_entry
	{
		std::shared_ptr<nano::block> block;
		std::chrono::steady_clock::time_point arrival;
		mutable std::chrono::steady_clock::time_point last_broadcast{}; // Not part of any index

		nano::block_hash hash () const
		{
			return block->hash ();
		}
	};

	// clang-format off
	class tag_sequenced	{};
	class tag_hash {};

	using ordered_locals = boost::multi_index_container<local_entry,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::const_mem_fun<local_entry, nano::block_hash, &local_entry::hash>>
	>>;
	// clang-format on

	ordered_locals local_blocks;

private:
	void run ();
	void run_once (nano::unique_lock<nano::mutex> &);
	void cleanup ();

private:
	//	class hash_tracker
	//	{
	//	public:
	//		void add (nano::block_hash const &);
	//		void erase (nano::block_hash const &);
	//		bool contains (nano::block_hash const &) const;
	//
	//	private:
	//		mutable nano::mutex mutex;
	//
	//		// clang-format off
	//		class tag_sequenced {};
	//		class tag_hash {};
	//
	//		using ordered_hashes = boost::multi_index_container<nano::block_hash,
	//		mi::indexed_by<
	//			mi::sequenced<mi::tag<tag_sequenced>>,
	//			mi::hashed_unique<mi::tag<tag_hash>,
	//				mi::identity<nano::block_hash>>
	//		>>;
	//		// clang-format on
	//
	//		// Blocks originated on this node
	//		ordered_hashes hashes;
	//
	//		static std::size_t constexpr max_size = 1024 * 128;
	//	};
	//	hash_tracker local;

private:
	bool enabled{ false };

	bool stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;

	static std::size_t constexpr local_max_size{ 1024 * 32 };
	static std::chrono::seconds constexpr local_check_interval{ 30 };
	static std::chrono::seconds constexpr local_broadcast_interval{ 60 };
	static std::chrono::seconds constexpr local_age_cutoff{ 60 * 60 };
};
}
