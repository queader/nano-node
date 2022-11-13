#pragma once

#include <nano/lib/locks.hpp>

#include <atomic>
#include <condition_variable>
#include <thread>

namespace nano
{
class stat;
class store;
class election_scheduler;

class backlog_population final
{
public:
	struct config
	{
		bool ongoing_backlog_population_enabled;

		/** Percentage of time to spend doing frontier scanning. Should be limited in order not to steal too much IO from other node operations. (0-100 range) */
		uint duty_cycle;

	public: // Helpers
		/**
		 * Converts duty cycle percentage to thread sleep time.
		 */
		std::chrono::duration<double> duty_to_sleep_time () const;
	};

	backlog_population (const config &, nano::store &, nano::election_scheduler &, nano::stat &);
	~backlog_population ();

	void start ();
	void stop ();
	void trigger ();

	/** Other components call this to notify us about external changes, so we can check our predicate. */
	void notify ();

private: // Dependencies
	nano::store & store;
	nano::election_scheduler & scheduler;
	nano::stat & stats;

	config config_m;

private:
	void run ();
	bool predicate () const;

	void populate_backlog ();

	/** This is a manual trigger, the ongoing backlog population does not use this.
	 *  It can be triggered even when backlog population (frontiers confirmation) is disabled. */
	bool triggered{ false };

	std::atomic<bool> stopped{ false };

	nano::condition_variable condition;
	mutable nano::mutex mutex;

	/** Thread that runs the backlog implementation logic. The thread always runs, even if
	 *  backlog population is disabled, so that it can service a manual trigger (e.g. via RPC). */
	std::thread thread;

private: // Config
	/**
	 * How many accounts to scan in one internal loop pass
	 * Should not be too high, as not to hold a database read transaction for too long
	 */
	static uint64_t const chunk_size = 1024;
};
}
