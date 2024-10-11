#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/observer_set.hpp>
#include <nano/node/fwd.hpp>
#include <nano/secure/account_info.hpp>
#include <nano/secure/common.hpp>

#include <condition_variable>
#include <thread>

namespace nano
{
class backlog_scan_config final
{
public:
	nano::error deserialize (nano::tomlconfig &);
	nano::error serialize (nano::tomlconfig &) const;

public:
	/** Control if ongoing backlog population is enabled. If not, backlog population can still be triggered by RPC */
	bool enable{ true };
	/** Number of accounts per second to process. Number of accounts per single batch is this value divided by `frequency` */
	unsigned batch_size{ 10 * 1000 };
	/** Number of batches to run per second. Batches run in 1 second / `frequency` intervals */
	unsigned frequency{ 10 };
};

class backlog_scan final
{
public:
	backlog_scan (backlog_scan_config const &, nano::ledger &, nano::stats &);
	~backlog_scan ();

	void start ();
	void stop ();

	/** Manually trigger backlog population */
	void trigger ();

	/** Notify about AEC vacancy */
	void notify ();

public:
	struct activated_info
	{
		nano::account account;
		nano::account_info account_info;
		nano::confirmation_height_info conf_info;
	};

	/**
	 * Callback called for each backlogged account
	 */
	using callback_t = nano::observer_set<nano::secure::transaction const &, activated_info const &>;
	callback_t activated;

private: // Dependencies
	backlog_scan_config const & config;
	nano::ledger & ledger;
	nano::stats & stats;

private:
	void run ();
	bool predicate () const;
	void populate_backlog (nano::unique_lock<nano::mutex> & lock);
	void activate (secure::transaction const &, nano::account const &, nano::account_info const &);

private:
	/** This is a manual trigger, the ongoing backlog population does not use this.
	 *  It can be triggered even when backlog population (frontiers confirmation) is disabled. */
	bool triggered{ false };

	bool stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;

	/** Thread that runs the backlog implementation logic. The thread always runs, even if
	 *  backlog population is disabled, so that it can service a manual trigger (e.g. via RPC). */
	std::thread thread;
};
}
