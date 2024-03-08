#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/rep_tiers.hpp>
#include <nano/secure/common.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace nano
{
class node;
class stats;
}

namespace nano
{
class vote_broadcaster_config final
{
public:
	size_t max_recently_broadcasted{ 1024 * 32 };
};

class vote_rebroadcaster final
{
public:
	explicit vote_rebroadcaster (nano::node &);
	~vote_rebroadcaster ();

	void start ();
	void stop ();

	bool rebroadcast (std::shared_ptr<nano::vote> const & vote, nano::rep_tier tier);

private: // Dependencies
	nano::vote_broadcaster_config const & config;
	nano::node & node;
	nano::stats & stats;

private:
	void run ();

private:
	//	class recently_broadcasted_entry
	//	{
	//		std::unordered_map<nano::account, uint64_t> representatives; // <representative account, vote timestamp>
	//	};
	//
	//	std::unordered_map<nano::block_hash, recently_broadcasted_entry> recently_broadcasted;

	enum class vote_state
	{
		empty = 0,
		non_final,
		final,
	};

	struct recently_broadcasted_entry
	{
		std::unordered_map<nano::block_hash, vote_state> votes; // <block, is final>
	};

	std::unordered_map<nano::account, recently_broadcasted_entry> recently_broadcasted;

private:
	std::atomic<bool> stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;
};
}