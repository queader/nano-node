#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/node/fwd.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <string>
#include <thread>

namespace nano::scheduler
{
class bucket;

class priority_config
{
public:
	// TODO: Serialization & deserialization

public:
	bool enabled{ true };
	size_t max_blocks{ 250000 };
	size_t elections_reserved{ 100 };
	size_t elections_max{ 150 };
};

class priority final
{
public:
	priority (nano::node &, nano::stats &);
	~priority ();

	void start ();
	void stop ();

	/**
	 * Activates the first unconfirmed block of \p account_a
	 * @return true if account was activated
	 */
	bool activate (secure::transaction const &, nano::account const &);
	void notify ();
	std::size_t size () const;
	bool empty () const;

	std::unique_ptr<container_info_component> collect_container_info (std::string const & name);

private: // Dependencies
	priority_config const & config;
	nano::node & node;
	nano::stats & stats;

private:
	void run ();
	bool empty_locked () const;
	bool predicate () const;
	bool available (nano::scheduler::bucket const &) const;

	nano::scheduler::bucket & bucket (nano::uint128_t balance) const;

private:
	std::vector<std::unique_ptr<nano::scheduler::bucket>> buckets;

	bool stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;
};
}
