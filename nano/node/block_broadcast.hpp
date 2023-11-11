#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/lib/locks.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <memory>
#include <thread>
#include <unordered_set>

namespace mi = boost::multi_index;

namespace nano
{
class block_arrival;
class block_processor;
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
public:
	block_broadcast (nano::block_processor &, nano::network &, nano::block_arrival &, bool enabled = false);

	// Mark a block as originating locally
	void track_local (nano::block_hash const &);

private: // Dependencies
	nano::block_processor & block_processor;
	nano::network & network;
	nano::block_arrival & block_arrival;

private:
	// Block_processor observer
	void observe (std::shared_ptr<nano::block> const & block);

	void run ();

private:
	class hash_tracker
	{
	public:
		void add (nano::block_hash const &);
		void erase (nano::block_hash const &);
		bool contains (nano::block_hash const &) const;

	private:
		mutable nano::mutex mutex;

		// clang-format off
		class tag_sequenced {};
		class tag_hash {};

		using ordered_hashes = boost::multi_index_container<nano::block_hash,
		mi::indexed_by<
			mi::sequenced<mi::tag<tag_sequenced>>,
			mi::hashed_unique<mi::tag<tag_hash>,
				mi::identity<nano::block_hash>>
		>>;
		// clang-format on

		// Blocks originated on this node
		ordered_hashes hashes;

		static std::size_t constexpr max_size = 1024 * 128;
	};

	hash_tracker local;

	bool enabled{ false };
	bool stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;
};
}
