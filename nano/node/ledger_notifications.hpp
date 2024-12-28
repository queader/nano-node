#pragma once

#include <nano/lib/function.hpp>
#include <nano/lib/observer_set.hpp>
#include <nano/node/block_context.hpp>
#include <nano/node/fwd.hpp>
#include <nano/secure/common.hpp>

#include <deque>
#include <functional>
#include <thread>

namespace nano
{
class ledger_notifications
{
public: // Events
	// All processed blocks including forks, rejected etc
	using processed_batch_t = std::deque<std::pair<nano::block_status, nano::block_context>>;
	using processed_batch_event_t = nano::observer_set<processed_batch_t>;
	processed_batch_event_t batch_processed;

	// Rolled back blocks <rolled back blocks, root of rollback>
	using rolled_back_batch_t = std::deque<std::shared_ptr<nano::block>>;
	using rolled_back_event_t = nano::observer_set<std::deque<std::shared_ptr<nano::block>>, nano::qualified_root>;
	rolled_back_event_t rolled_back;

public:
	ledger_notifications (nano::node_config const &, nano::stats &, nano::logger &);
	~ledger_notifications ();

	void start ();
	void stop ();

	void wait (std::function<void ()> cooldown_action = nullptr);

	// Must pass a write transaction to ensure that notifications are dispatched in the correct order
	void notify_processed (nano::store::write_transaction const &, processed_batch_t batch, std::function<void ()> callback = nullptr);
	void notify_rolled_back (nano::store::write_transaction const &, rolled_back_batch_t batch, nano::qualified_root rollback_root, std::function<void ()> callback = nullptr);

	nano::container_info container_info () const;

private: // Dependencies
	nano::node_config const & config;
	nano::stats & stats;
	nano::logger & logger;

private:
	void run ();

private:
	std::deque<std::function<void ()>> notifications;

	std::thread thread;
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	bool stopped{ false };
};
}