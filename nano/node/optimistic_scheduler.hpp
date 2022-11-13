#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/timer.hpp>
#include <nano/lib/utility.hpp>
#include <nano/secure/common.hpp>

#include <condition_variable>
#include <memory>
#include <queue>
#include <thread>
#include <vector>

namespace nano
{
class node;
class active_transactions;

/**
 * TODO: Docs
 */
class optimistic_scheduler final
{
public: // Config
	struct config final
	{
		/** Minimum difference between confirmation frontier and account frontier to become a candidate for optimistic confirmation */
		uint optimistic_gap_threshold;
	};

public:
	optimistic_scheduler (config const &, nano::node &, nano::active_transactions &, nano::stat &);
	~optimistic_scheduler ();

	void start ();
	void stop ();

	/**
	 * TODO: Docs
	 * Flow: backlog_population frontier scan > election_scheduler::activate > (gather account info) > optimistic_scheduler::activate
	 */
	bool activate (nano::account const &, nano::account_info const &, nano::confirmation_height_info const &);

	/**
	 * Notify about changes in AEC vacancy
	 */
	void notify ();

private:
	bool predicate () const;
	void run ();
	void run_one (nano::unique_lock<nano::mutex> &);

private: // Dependencies
	nano::node & node;
	nano::active_transactions & active;
	nano::stat & stats;

	config const config_m;

private:
	std::deque<nano::account> candidates;

	std::atomic<bool> stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;

private: // Config
	static std::size_t constexpr max_size = 1024 * 64;
};
}