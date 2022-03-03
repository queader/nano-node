#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/secure/store.hpp>

#include <set>
#include <unordered_set>
#include <map>

namespace nano
{
class node;

class bootstrap_prioritization final
{
public:
	explicit bootstrap_prioritization (nano::node &);
	~bootstrap_prioritization ();

	void queue_send (nano::transaction const &, nano::account const & account, nano::account const & destination, nano::amount const & balance, bool force = false);
	void queue_receive (nano::transaction const &, nano::account const & account, nano::account const & source, nano::amount const & balance, bool force = false);

private:
	using priority_value = unsigned;

	class value_type
	{
	public:
		nano::amount amount;
		nano::account account;
		priority_value priority;
		bool forced;
		bool operator< (value_type const & other) const;
		bool operator== (value_type const & other) const;
	};

	value_type top ();
	void requeue (value_type entry);

	void run_lazy_bootstrap ();
	void run_initialize ();

	std::set<value_type> missing;
	std::unordered_set<nano::account> done_accounts;
	std::unordered_map<nano::account, priority_value> priority_map;

	nano::node & node;
	nano::mutex mutex;
	std::thread thread_lazy_bootsrap;
	std::thread thread_initialize;

	void check_status ();
	bool exists (nano::account const & account);
};
}